/*
 * Rigol DS1000Z / MSO1000Z link. See scope.h.
 *
 * Two transports. Over the LAN the scope's raw SCPI socket (TCP port 5555)
 * takes a command line and answers with a line, or with an IEEE 488.2 block
 * followed by a newline. Finding the scope is a probe with its own short
 * timeouts: port 5555 on every host of the local subnet at once (or on the
 * address given), and *IDN? of each that answers. Once found, the link is
 * an AMI network stream (ami_opennetname) and plain stdio, and the thread
 * blocks on it for each reply, as the tool kit intends: a request and
 * reply protocol on one stream needs one thread and no readiness polling.
 * Nothing bounds those waits. If the scope stops answering, the front
 * panel sees the frames stop (scope_poll), retires this link thread by
 * generation number and starts another; the old one exits when its read
 * finally returns. USB is the richer case:
 *
 * USBTMC over libusb, bulk transfers only. The DS1000Z declares
 * and uses 64 byte bulk packets although it runs at high speed, so the host
 * sees every packet as a short packet: a reply is gathered piece by piece
 * until the declared size is in, and when the last piece is a full 64 bytes
 * the zero length packet the scope sends after it is taken too. The scope
 * also pads replies inconsistently, so a read stops at the declared size,
 * not the padded one. A reply larger than the request comes in several
 * transfers, each ended by the EOM flag of the last.
 *
 * The screen picture is fetched as TIFF (about 47 kB, 0.45 s: the scope
 * encodes it two to three times faster than PNG, which takes 0.8 to 1.6 s)
 * and decoded by tiff.c; if that fails twice the PNG form is used instead,
 * decoded with libpng. Either way the picture is written as a 24 bit BMP,
 * the only picture format the AMI graphics module loads. Two files
 * alternate, so the front panel can load one while the next is written.
 * The last picture is kept as pixels so SAVE PNG can encode it.
 *
 * Commands found on the instrument, not in Rigol's programming guide:
 *   :SYST:KEY:PRES <key>       press a front panel key
 *   :SYST:KEY:INCR <knob>,1    turn a knob one detent up; DECR down
 * Keys: CH1..CH4 MATH REF LA MEAS ACQ STOR CURS DISP UTIL HELP SOUR
 *       RSTOP SING TFOR CLE HMEN TMEN F1..F6.  Knobs: HSC HPOS TLEV.
 * AUTO is not a key (there is :AUT), and the vertical knobs have no name,
 * so those go as :CHAN<n>:SCAL and :CHAN<n>:OFFS on the active channel.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <setjmp.h>
#include <stdarg.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <libusb.h>
#include <png.h>

#include <localdefs.h>
#include <services.h>
#include <network.h>

#include "scope.h"
#include "tiff.h"

#define QMAX          64      /* command queue depth */
#define CMDLEN        64
#define SETTINGS_MS   2000    /* re-read settings this often, when a pass is slow */
#define SETTINGS_FAST 100     /* a pass under this (ms) is repeated every frame */
#define RETRY_MS      2000    /* reconnect interval */
#define REPLY_MS      5000    /* bus timeout for a reply */
#define FRAME_MS      15000   /* bus timeout for the screen picture */
#define CONNECT_MS    1500    /* LAN probe: connect timeout for a given address */
#define STALE_MS      10000   /* LAN: no frame for this long means the link is dead */
#define MIN_CYCLE_MS  100     /* a link faster than this would swamp the window */
#define PROBE_MS      500     /* LAN search: connect timeout per batch */
#define PROBE_BATCH   128     /* LAN search: hosts tried at once */
#define PROBE_MAX     1024    /* LAN search: largest subnet tried */
#define SCREEN_W      800
#define SCREEN_H      480

/* ------------------------------------------------------------ shared state */

/* Everything below is guarded by lk. The link thread works on its own copy
   (st) and publishes it here; the front panel copies it out in scope_poll. */
static ami_long     lk;
static scope_state  shared;
static int          shared_changed;
static char         queue[QMAX][CMDLEN];
static int          qhead, qtail;
static int          stop_req, thread_done;
static int          gen;             /* link thread generation; a bump retires the thread */
static uint8_t     *pix_last;        /* the last picture, BGR top down */
static int          pix_w, pix_h;

/* ------------------------------------------------------------ thread state */

static scope_state           st;
static libusb_context       *ctx;
static libusb_device_handle *dev;          /* USB: open device */
static __thread FILE        *netf;         /* LAN: the AMI stream, this thread's own */
static __thread int          my_gen;       /* the generation this thread is */
static __thread jmp_buf      worker_exit;  /* where a retired thread leaves from */
static __thread jmp_buf      net_jmp;      /* where a network open error lands */
static __thread char         net_err[128];
static char                  lan_host[64]; /* LAN: address given, or "" to search */
static int                   lan_port = SCOPE_PORT;
static uint8_t               ep_in, ep_out;
static uint8_t               tag = 1;
static char                  frame_path[2][256];
static int                   frame_ix;
static long                  next_settings, next_retry;
static int                   tiff_fails;    /* fetch as PNG once this reaches 2 */
static int                   sim_running = 1, sim_single;
static double                sim_phase;

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

static void set_status(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(st.status, sizeof st.status, fmt, ap);
    va_end(ap);
}

/* publish the thread's state */
static void publish(void)
{
    ami_lock(lk);
    st.active = shared.active;  /* the front panel owns this one */
    shared = st;
    shared_changed = 1;
    ami_unlock(lk);
}

static int dequeue(char *out)
{
    int got = 0;

    ami_lock(lk);
    if (qhead != qtail) {
        strcpy(out, queue[qhead]);
        qhead = (qhead + 1) % QMAX;
        got = 1;
    }
    ami_unlock(lk);
    return got;
}

static void enqueue(const char *cmd)
{
    int next;

    ami_lock(lk);
    next = (qtail + 1) % QMAX;
    if (next != qhead) { /* full: drop, the next poll recovers */
        strncpy(queue[qtail], cmd, CMDLEN - 1);
        queue[qtail][CMDLEN - 1] = 0;
        qtail = next;
    }
    ami_unlock(lk);
}

/* ------------------------------------------------------------ usbtmc */

static void tmc_header(uint8_t *h, int msgid, uint32_t size, int eom)
{
    memset(h, 0, 12);
    h[0] = (uint8_t)msgid;
    h[1] = tag;
    h[2] = (uint8_t)~tag;
    h[4] = (uint8_t)size;
    h[5] = (uint8_t)(size >> 8);
    h[6] = (uint8_t)(size >> 16);
    h[7] = (uint8_t)(size >> 24);
    h[8] = eom ? 1 : 0;
    tag = (uint8_t)(tag % 255 + 1);
}

static int tmc_write(const char *s)
{
    uint8_t buf[12 + 256];
    size_t  n = strlen(s), pad = (4 - n % 4) % 4;
    int     got;

    if (n > 240) return LIBUSB_ERROR_INVALID_PARAM;
    tmc_header(buf, 1, (uint32_t)n, 1);
    memcpy(buf + 12, s, n);
    memset(buf + 12 + n, 0, pad);
    return libusb_bulk_transfer(dev, ep_out, buf, (int)(12 + n + pad), &got, 2000);
}

/* read one reply, all its transfers, into a malloc'd buffer */
static int tmc_read(uint8_t **out, size_t *outlen, int timeout)
{
    uint8_t *acc = NULL, *buf = NULL;
    size_t   acclen = 0, len, cap;
    uint8_t  h[12], zlp[64];
    uint32_t tsize = 0;
    int      r, got, eom, have_hdr, last, zeros;

    for (;;) {
        tmc_header(h, 2, 1u << 20, 0); /* REQUEST_DEV_DEP_MSG_IN */
        r = libusb_bulk_transfer(dev, ep_out, h, 12, &got, 2000);
        if (r < 0) goto fail;
        cap = 65536;
        buf = malloc(cap);
        if (!buf) { r = LIBUSB_ERROR_NO_MEM; goto fail; }
        len = 0; have_hdr = 0; eom = 0; last = 0; zeros = 0;
        while (!have_hdr || len < 12 + tsize) {
            if (cap - len < 65536) {
                uint8_t *nb = realloc(buf, cap * 2);

                if (!nb) { r = LIBUSB_ERROR_NO_MEM; goto fail; }
                buf = nb; cap *= 2;
            }
            r = libusb_bulk_transfer(dev, ep_in, buf + len, 65536, &got, timeout);
            if (r < 0) goto fail;
            if (got == 0 && ++zeros > 4) { r = LIBUSB_ERROR_IO; goto fail; }
            len += (size_t)got;
            last = got;
            if (!have_hdr && len >= 12) {
                tsize = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                        ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
                eom = buf[8] & 1;
                have_hdr = 1;
            }
        }
        if (last == 64) libusb_bulk_transfer(dev, ep_in, zlp, 64, &got, 300);
        {
            uint8_t *na = realloc(acc, acclen + tsize + 1);

            if (!na) { r = LIBUSB_ERROR_NO_MEM; goto fail; }
            acc = na;
        }
        memcpy(acc + acclen, buf + 12, tsize);
        acclen += tsize;
        free(buf); buf = NULL;
        if (eom) break;
    }
    acc[acclen] = 0;
    *out = acc;
    *outlen = acclen;
    return 0;
fail:
    free(buf);
    free(acc);
    *out = NULL;
    *outlen = 0;
    return r;
}

/* ------------------------------------------------------------ lan probe */

/* The probe has its own sockets and timeouts, since finding the scope
   means trying many addresses at once and giving up on the silent ones
   quickly. Once the scope has answered *IDN? the probe socket is closed
   and the link proper is opened as an AMI stream. */

/* Start a connect to each of n addresses at once and wait up to timeout
   for them: fds[i] is the connected socket, or -1. A single address is
   the ordinary connect; the subnet search does a batch. */
static void connect_batch(const struct sockaddr_in *sa, int n, int timeout, int *fds)
{
    struct pollfd *pf = calloc((size_t)n, sizeof *pf);
    int   i, r, err, left = 0;
    long  end = now_ms() + timeout;
    socklen_t el;

    for (i = 0; i < n; i++) {
        fds[i] = -1;
        if (!pf) continue;
        pf[i].fd = -1;
        r = socket(AF_INET, SOCK_STREAM, 0);
        if (r < 0) continue;
        fcntl(r, F_SETFL, fcntl(r, F_GETFL) | O_NONBLOCK);
        if (connect(r, (const struct sockaddr *)&sa[i], sizeof sa[i]) < 0 && errno != EINPROGRESS) {
            close(r);
            continue;
        }
        pf[i].fd = r;
        pf[i].events = POLLOUT;
        left++;
    }
    while (left > 0 && pf) {
        long t = end - now_ms();

        if (t <= 0) break;
        if (poll(pf, (nfds_t)n, (int)t) <= 0) break;
        for (i = 0; i < n; i++) {
            if (pf[i].fd < 0 || !pf[i].revents) continue;
            err = 0; el = sizeof err;
            getsockopt(pf[i].fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err == 0 && !(pf[i].revents & (POLLERR | POLLHUP))) fds[i] = pf[i].fd;
            else close(pf[i].fd);
            pf[i].fd = -1;
            left--;
        }
    }
    for (i = 0; pf && i < n; i++) if (pf[i].fd >= 0) close(pf[i].fd);
    free(pf);
    for (i = 0; i < n; i++) {
        int one = 1;

        if (fds[i] < 0) continue;
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) & ~O_NONBLOCK);
        setsockopt(fds[i], IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
}

/* write a command line to fd; -1 with errno on failure */
static int sock_write(int fd, const char *s)
{
    char   buf[300];
    size_t n = strlen(s), off = 0;
    ssize_t w;

    if (n > sizeof buf - 2) { errno = EINVAL; return -1; }
    memcpy(buf, s, n);
    buf[n++] = '\n';
    while (off < n) {
        w = send(fd, buf + off, n - off, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)w;
    }
    return 0;
}

/* read one reply from a probe socket into a malloc'd buffer: a line, or
   an IEEE 488.2 block (#, digit, length, data). -1 with errno on failure,
   ETIMEDOUT when nothing came. */
static int sock_read(int fd, uint8_t **out, size_t *outlen, int timeout)
{
    uint8_t *buf = NULL, *nb;
    size_t   len = 0, cap = 0, skip = 0, want = 0, i;
    int      digits = 0, kind = 0; /* 0 unknown, 1 line, 2 block */
    ssize_t  got;
    struct pollfd pf = { fd, POLLIN, 0 };

    for (;;) {
        /* enough yet? */
        for (; skip < len && (buf[skip] == '\n' || buf[skip] == '\r'); skip++) ;
        if (!kind && len > skip) kind = buf[skip] == '#' ? 2 : 1;
        if (kind == 1) {
            for (i = skip; i < len; i++) if (buf[i] == '\n') break;
            if (i < len) { want = i; break; }
        } else if (kind == 2 && len > skip + 1) {
            digits = buf[skip + 1] - '0';
            if (digits < 1 || digits > 9) { kind = 1; continue; }
            if (len >= skip + 2 + (size_t)digits) {
                size_t bl = 0;

                for (i = 0; i < (size_t)digits; i++) bl = bl * 10 + (size_t)(buf[skip + 2 + i] - '0');
                want = skip + 2 + (size_t)digits + bl;
                if (len >= want) break;
            }
        }
        /* more */
        if (cap - len < 65536) {
            nb = realloc(buf, cap + 131072);
            if (!nb) { free(buf); errno = ENOMEM; return -1; }
            buf = nb; cap += 131072;
        }
        got = poll(&pf, 1, timeout);
        if (got == 0) { free(buf); errno = ETIMEDOUT; return -1; }
        if (got < 0) { if (errno == EINTR) continue; free(buf); return -1; }
        got = recv(fd, buf + len, cap - len - 1, 0); /* a byte spare for the terminator */
        if (got == 0) { free(buf); errno = ECONNRESET; return -1; }
        if (got < 0) { if (errno == EINTR) continue; free(buf); return -1; }
        len += (size_t)got;
    }
    memmove(buf, buf + skip, want - skip);
    *outlen = want - skip;
    buf[*outlen] = 0;
    *out = buf;
    return 0;
}

/* ask *IDN? on a connected socket; 1 if a Rigol answers, else 0 */
static int is_rigol(int fd, int timeout, char *idn, size_t idnlen)
{
    uint8_t *rep;
    size_t   n;

    if (sock_write(fd, "*IDN?") < 0) return 0;
    if (sock_read(fd, &rep, &n, timeout) < 0) return 0;
    snprintf(idn, idnlen, "%s", (char *)rep);
    free(rep);
    return strstr(idn, "RIGOL") != NULL;
}

/* Search the local subnets for a Rigol on the SCPI port. Sets host and
   returns 0, or -1 with the status set. */
static int find_lan_scope(char *host, size_t hostlen)
{
    struct ifaddrs     *ifa, *p;
    struct sockaddr_in *cand = NULL, *sa, *nm;
    int    ncand = 0, i, n, fds[PROBE_BATCH], found = -1;
    unsigned long net, mask, hosts, a, mine;
    char   idn[160], subnet[32] = "";

    if (getifaddrs(&ifa) < 0) {
        set_status("LAN: %s", strerror(errno));
        return -1;
    }
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !p->ifa_netmask) continue;
        if (p->ifa_flags & IFF_LOOPBACK || !(p->ifa_flags & IFF_UP)) continue;
        sa = (struct sockaddr_in *)p->ifa_addr;
        nm = (struct sockaddr_in *)p->ifa_netmask;
        mine = ntohl(sa->sin_addr.s_addr);
        mask = ntohl(nm->sin_addr.s_addr);
        net = mine & mask;
        hosts = ~mask & 0xffffffffUL;
        if (hosts < 2 || hosts > PROBE_MAX) continue; /* a point to point link, or too big */
        {
            struct sockaddr_in *nc = realloc(cand, (ncand + hosts) * sizeof *cand);

            if (!nc) break;
            cand = nc;
        }
        if (!subnet[0]) {
            int bits = 32;

            for (a = ~mask & 0xffffffffUL; a; a >>= 1) bits--;
            snprintf(subnet, sizeof subnet, "%lu.%lu.%lu.%lu/%d", net >> 24, (net >> 16) & 255, (net >> 8) & 255, net & 255, bits);
        }
        for (a = net + 1; a < net + hosts; a++) {
            if (a == mine) continue;
            memset(&cand[ncand], 0, sizeof cand[ncand]);
            cand[ncand].sin_family = AF_INET;
            cand[ncand].sin_port = htons((uint16_t)lan_port);
            cand[ncand].sin_addr.s_addr = htonl(a);
            ncand++;
        }
    }
    freeifaddrs(ifa);
    if (ncand == 0) {
        set_status("No LAN to search (no subnet of /22 or smaller): give the address");
        free(cand);
        return -1;
    }
    set_status("Looking for the scope on %s", subnet);
    publish();
    for (i = 0; i < ncand && found < 0 && !stop_req; i += n) {
        int k;

        n = ncand - i < PROBE_BATCH ? ncand - i : PROBE_BATCH;
        connect_batch(cand + i, n, PROBE_MS, fds);
        for (k = 0; k < n; k++) {
            if (fds[k] < 0) continue;
            if (found < 0 && is_rigol(fds[k], 1000, idn, sizeof idn)) {
                found = 0;
                snprintf(host, hostlen, "%s", inet_ntoa(cand[i + k].sin_addr));
            }
            close(fds[k]);
        }
    }
    free(cand);
    if (found < 0) set_status("No scope found on %s", subnet);
    return found;
}

/* Probe the address given: resolve it, connect with a timeout, and check
   that a Rigol answers. Sets host to the address and returns 0, or -1
   with the status set. */
static int probe_host(char *host, size_t hostlen)
{
    struct addrinfo hints, *res, *r;
    struct sockaddr_in sa;
    char   port[8], idn[160];
    int    fd = -1, e, ok = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%d", lan_port);
    if ((e = getaddrinfo(lan_host, port, &hints, &res)) != 0) {
        set_status("%s: %s", lan_host, gai_strerror(e));
        return -1;
    }
    for (r = res; r && fd < 0; r = r->ai_next) {
        if (r->ai_addrlen != sizeof sa) continue;
        memcpy(&sa, r->ai_addr, sizeof sa);
        connect_batch(&sa, 1, CONNECT_MS, &fd);
        if (fd >= 0) snprintf(host, hostlen, "%s", inet_ntoa(sa.sin_addr));
    }
    freeaddrinfo(res);
    if (fd < 0) {
        set_status("No answer from %s port %d", lan_host, lan_port);
        return -1;
    }
    if (is_rigol(fd, 1000, idn, sizeof idn)) ok = 0;
    else set_status("%s is not a Rigol scope", lan_host);
    close(fd);
    return ok;
}

/* ------------------------------------------------------------ the link */

/* the link is up: a device or a stream */
static int linked(void)
{
    return dev != NULL || netf != NULL;
}

/* This thread has been retired by the front panel while it was blocked
   (see scope_poll): leave at once, without touching the shared state the
   thread that replaced it now owns. */
static void stale_check(void)
{
    int stale;

    ami_lock(lk);
    stale = gen != my_gen;
    ami_unlock(lk);
    if (stale) longjmp(worker_exit, 1);
}

/* one reply from the AMI stream into a malloc'd buffer: a line without
   its newline, or an IEEE 488.2 block (#, digit, length, data) and the
   newline after it. Blocks until it is all in. -1 on a dead stream. */
static int net_read(uint8_t **out, size_t *outlen)
{
    uint8_t *buf;
    size_t   len = 0, cap = 256;
    int      c, digits, i;

    do c = fgetc(netf); while (c == '\n' || c == '\r');
    if (c == EOF) return -1;
    if (c == '#') {
        c = fgetc(netf);
        digits = c - '0';
        if (c == EOF || digits < 1 || digits > 9) return -1;
        for (i = 0; i < digits; i++) {
            c = fgetc(netf);
            if (c < '0' || c > '9') return -1;
            len = len * 10 + (size_t)(c - '0');
        }
        buf = malloc(2 + (size_t)digits + len + 1);
        if (!buf) return -1;
        /* hand it back as the scope sent it, header and all, as USB does */
        cap = (size_t)snprintf((char *)buf, 2 + (size_t)digits + 1, "#%d%0*zu", digits, digits, len);
        if (fread(buf + cap, 1, len, netf) != len) { free(buf); return -1; }
        c = fgetc(netf);                        /* the newline after the block */
        if (c != EOF && c != '\n') ungetc(c, netf);
        *out = buf;
        *outlen = cap + len;
        buf[*outlen] = 0;
        return 0;
    }
    buf = malloc(cap);
    if (!buf) return -1;
    while (c != EOF && c != '\n') {
        if (len + 2 > cap) {
            uint8_t *nb = realloc(buf, cap * 2);

            if (!nb) { free(buf); return -1; }
            buf = nb; cap *= 2;
        }
        buf[len++] = (uint8_t)c;
        c = fgetc(netf);
    }
    if (c == EOF) { free(buf); return -1; }
    buf[len] = 0;
    *out = buf;
    *outlen = len;
    return 0;
}

static const char *net_why(void)
{
    return ferror(netf) ? strerror(errno) : "connection closed";
}

/* write a command on whichever link is up; 0, or -1 with why set */
static int link_write(const char *cmd, const char **why)
{
    int r;

    if (netf) {
        r = fputs(cmd, netf) == EOF || fputc('\n', netf) == EOF || fflush(netf) == EOF;
        stale_check();
        if (!r) return 0;
        *why = net_why();
        return -1;
    }
    if ((r = tmc_write(cmd)) < 0) { *why = libusb_strerror(r); return -1; }
    return 0;
}

static int link_read(uint8_t **out, size_t *outlen, int timeout, const char **why)
{
    int r;

    if (netf) {
        r = net_read(out, outlen);
        if (gen != my_gen && r == 0) free(*out); /* retired while blocked: leave nothing behind */
        stale_check();
        if (r == 0) return 0;
        *why = net_why();
        return -1;
    }
    if ((r = tmc_read(out, outlen, timeout)) < 0) { *why = libusb_strerror(r); return -1; }
    return 0;
}

static void link_fail(const char *what, const char *why)
{
    set_status("%s: %s", what, why);
    if (dev) {
        libusb_release_interface(dev, 0);
        libusb_reset_device(dev); /* clears a half finished transfer in the scope */
        libusb_close(dev);
        dev = NULL;
    }
    if (netf) {
        fclose(netf);
        netf = NULL;
    }
    st.connected = 0;
    st.errors++;
    next_retry = now_ms() + RETRY_MS;
    publish();
}

/* query: reply as a trimmed string in buf; nonzero on failure (link dropped) */
static int query(const char *cmd, char *buf, size_t buflen)
{
    uint8_t    *rep;
    size_t      n;
    const char *why;

    if (link_write(cmd, &why) < 0) { link_fail(cmd, why); return -1; }
    if (link_read(&rep, &n, REPLY_MS, &why) < 0) { link_fail(cmd, why); return -1; }
    while (n > 0 && (rep[n - 1] == '\n' || rep[n - 1] == '\r' || rep[n - 1] == ' ')) n--;
    rep[n] = 0;
    snprintf(buf, buflen, "%s", (char *)rep);
    free(rep);
    return 0;
}

static int qnum(const char *cmd, double *v)
{
    char b[64];

    if (query(cmd, b, sizeof b)) return -1;
    *v = atof(b);
    return 0;
}

static int send_cmd(const char *cmd)
{
    const char *why;

    if (link_write(cmd, &why) < 0) { link_fail(cmd, why); return -1; }
    return 0;
}

/* ------------------------------------------------------------ pictures */

/* 24 bit BMP from top-down BGR rows */
static int bmp_write(const char *path, int w, int h, uint8_t **rows)
{
    FILE    *f;
    uint8_t  hdr[54], pad[3] = { 0, 0, 0 };
    int      rowb = (w * 3 + 3) & ~3, y;
    uint32_t img = (uint32_t)rowb * (uint32_t)h, size = 54 + img;

    f = fopen(path, "wb");
    if (!f) return -1;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)size; hdr[3] = (uint8_t)(size >> 8); hdr[4] = (uint8_t)(size >> 16); hdr[5] = (uint8_t)(size >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (uint8_t)w; hdr[19] = (uint8_t)(w >> 8);
    hdr[22] = (uint8_t)h; hdr[23] = (uint8_t)(h >> 8);
    hdr[26] = 1;
    hdr[28] = 24;
    hdr[34] = (uint8_t)img; hdr[35] = (uint8_t)(img >> 8); hdr[36] = (uint8_t)(img >> 16); hdr[37] = (uint8_t)(img >> 24);
    hdr[38] = 0x13; hdr[39] = 0x0b; /* 2835 pixels per metre */
    hdr[42] = 0x13; hdr[43] = 0x0b;
    fwrite(hdr, 1, 54, f);
    for (y = h - 1; y >= 0; y--) {
        fwrite(rows[y], 1, (size_t)w * 3, f);
        if (rowb > w * 3) fwrite(pad, 1, (size_t)(rowb - w * 3), f);
    }
    fclose(f);
    return 0;
}

typedef struct { const uint8_t *p; size_t n, off; } memrd;

static void png_mem_read(png_structp png, png_bytep out, png_size_t len)
{
    memrd *m = png_get_io_ptr(png);

    if (m->off + len > m->n) png_error(png, "short PNG");
    memcpy(out, m->p + m->off, len);
    m->off += len;
}

/* decode a PNG in memory to top-down BGR pixels; returns 0 on success */
static int png_decode(const uint8_t *data, size_t n, int *wout, int *hout, uint8_t **pix)
{
    png_structp png = NULL;
    png_infop   info = NULL;
    png_bytep  *rows = NULL;
    uint8_t    *out = NULL;
    memrd       m = { data, n, 0 };
    png_uint_32 w, h, y;
    int         depth, ctype, ok = -1;

    if (n < 8 || png_sig_cmp(data, 0, 8)) return -1;
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return -1;
    info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return -1; }
    if (setjmp(png_jmpbuf(png))) goto done;
    png_set_read_fn(png, &m, png_mem_read);
    png_read_info(png, info);
    png_get_IHDR(png, info, &w, &h, &depth, &ctype, NULL, NULL, NULL);
    if (ctype == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (ctype == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (depth == 16) png_set_strip_16(png);
    if (ctype == PNG_COLOR_TYPE_GRAY || ctype == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_strip_alpha(png);
    png_set_bgr(png);
    png_read_update_info(png, info);
    if (png_get_rowbytes(png, info) != w * 3) goto done;
    if (w > 4096 || h > 4096) goto done;
    rows = calloc(h, sizeof *rows);
    out = malloc((size_t)w * h * 3);
    if (!rows || !out) goto done;
    for (y = 0; y < h; y++) rows[y] = out + (size_t)y * w * 3;
    png_read_image(png, rows);
    ok = 0;
done:
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    if (ok) { free(out); return -1; }
    *wout = (int)w;
    *hout = (int)h;
    *pix = out;
    return 0;
}

/* A picture is decoded: write it as the next BMP, tell the front panel,
   and keep the pixels for SAVE PNG. pix is top-down BGR, w*h*3. */
static void frame_ready(const uint8_t *pix, int w, int h, int ms)
{
    uint8_t **rows = malloc((size_t)h * sizeof *rows);
    int       y;

    if (!rows) return;
    for (y = 0; y < h; y++) rows[y] = (uint8_t *)pix + (size_t)y * w * 3;
    frame_ix ^= 1;
    if (bmp_write(frame_path[frame_ix], w, h, rows) == 0) {
        ami_lock(lk);
        snprintf(shared.frame, sizeof shared.frame, "%s", frame_path[frame_ix]);
        shared.frame_seq++;
        shared.frame_ms = ms;
        shared.frames++;
        shared_changed = 1;
        {
            uint8_t *np = realloc(pix_last, (size_t)w * h * 3);

            if (np) { memcpy(np, pix, (size_t)w * h * 3); pix_last = np; pix_w = w; pix_h = h; }
        }
        ami_unlock(lk);
        st.frame_seq = shared.frame_seq;
        st.frames = shared.frames;
        st.frame_ms = ms;
        snprintf(st.frame, sizeof st.frame, "%s", frame_path[frame_ix]);
    }
    free(rows);
}

/* fetch the screen, as TIFF while that works and as PNG otherwise, decode,
   publish */
static void fetch_frame(void)
{
    uint8_t    *rep, *pix;
    size_t      n, start, len;
    long        t0 = now_ms();
    int         r, digits, w, h, tiff = tiff_fails < 2, ok = -1;
    const char *why;

    if (link_write(tiff ? ":DISP:DATA? ON,OFF,TIFF" : ":DISP:DATA? ON,OFF,PNG", &why) < 0) {
        if (tiff) tiff_fails++;
        link_fail("screen", why);
        return;
    }
    if (link_read(&rep, &n, FRAME_MS, &why) < 0) {
        if (tiff) tiff_fails++;
        link_fail("screen", why);
        return;
    }
    /* IEEE 488.2 block: '#', one digit n, n digits of length, data */
    digits = n >= 2 && rep[0] == '#' ? rep[1] - '0' : 0;
    if (digits >= 1 && digits <= 9 && n >= (size_t)(2 + digits)) {
        len = 0;
        for (r = 0; r < digits; r++) len = len * 10 + (size_t)(rep[2 + r] - '0');
        start = (size_t)(2 + digits);
        if (start + len > n) len = n - start;
        ok = tiff ? tiff_decode(rep + start, len, &w, &h, &pix) : png_decode(rep + start, len, &w, &h, &pix);
    }
    free(rep);
    if (ok) {
        if (tiff && ++tiff_fails >= 2) fprintf(stderr, "scope: no usable TIFF screen picture, using PNG\n");
        return;
    }
    frame_ready(pix, w, h, (int)(now_ms() - t0));
    free(pix);
}

/* ------------------------------------------------------------ settings */

/* The 21 values the panel uses: the strip, the key lamps, and the vertical
   knobs' starting points. A query takes about 1 ms over the LAN, so the
   pass is cheap; what is not cheap is the first command after a screen
   fetch, which the scope holds for about 350 ms two times in three, and
   that is paid whether this pass or the next fetch comes first. */
static int read_settings(void)
{
    char   b[64];
    int    c;
    double v;

    if (qnum(":TIM:SCAL?", &st.tscale)) return -1;
    for (c = 0; c < SCOPE_NCH; c++) {
        snprintf(b, sizeof b, ":CHAN%d:DISP?", c + 1);
        if (qnum(b, &v)) return -1;
        st.chdisp[c] = v != 0;
        snprintf(b, sizeof b, ":CHAN%d:SCAL?", c + 1);
        if (qnum(b, &st.chscale[c])) return -1;
        snprintf(b, sizeof b, ":CHAN%d:OFFS?", c + 1);
        if (qnum(b, &st.choffs[c])) return -1;
        snprintf(b, sizeof b, ":CHAN%d:PROB?", c + 1);
        if (qnum(b, &st.chprobe[c])) return -1;
    }
    if (qnum(":TRIG:EDG:LEV?", &st.trlev)) return -1;
    if (query(":TRIG:EDG:SOUR?", st.trsrc, sizeof st.trsrc)) return -1;
    if (query(":TRIG:STAT?", st.trstat, sizeof st.trstat)) return -1;
    if (query(":TRIG:SWE?", st.trswe, sizeof st.trswe)) return -1;
    st.have_settings = 1;
    return 0;
}

/* ------------------------------------------------------------ device */

/* the link is up: ask who it is, record it, and say connected */
static int identify(void)
{
    char b[160], *p, *q;

    if (query("*IDN?", b, sizeof b)) return -1; /* link_fail already ran */
    snprintf(st.idn, sizeof st.idn, "%s", b);
    /* RIGOL TECHNOLOGIES,DS1104Z Plus,DS1ZC195002530,00.04.04.SP3 */
    st.model[0] = st.serial[0] = 0;
    p = strchr(b, ',');
    if (p) {
        q = strchr(++p, ',');
        if (q) {
            snprintf(st.model, sizeof st.model, "%.*s", (int)(q - p), p);
            p = q + 1;
            q = strchr(p, ',');
            snprintf(st.serial, sizeof st.serial, "%.*s", q ? (int)(q - p) : (int)strlen(p), p);
        }
    }
    st.connected = 1;
    st.frames = 0;
    set_status("%s %s", st.model, st.serial);
    next_settings = 0;
    publish();
    return 0;
}

/* the AMI network module reports an error here, instead of aborting */
static void net_error(const char *es)
{
    snprintf(net_err, sizeof net_err, "%s", es);
    longjmp(net_jmp, 1);
}

/* the LAN: probe for the scope, at the address given or over the subnet,
   then open the link to it as an AMI stream */
static int open_lan(void)
{
    if ((lan_host[0] ? probe_host(st.host, sizeof st.host)
                     : find_lan_scope(st.host, sizeof st.host)) < 0) return -1;
    if (setjmp(net_jmp)) {
        set_status("%s: %s", st.host, net_err);
        return -1;
    }
    netf = ami_opennetname(st.host, lan_port, FALSE);
    stale_check();
    return identify();
}

static int open_device(void)
{
    struct libusb_config_descriptor *cfg;
    const struct libusb_interface_descriptor *ifd;
    libusb_device *d;
    int    r, i;

    if (!ctx && (r = libusb_init(&ctx)) < 0) {
        set_status("libusb: %s", libusb_strerror(r));
        return -1;
    }
    dev = libusb_open_device_with_vid_pid(ctx, SCOPE_VID, SCOPE_PID);
    if (!dev) {
        set_status("No scope found on USB");
        return -1;
    }
    libusb_set_auto_detach_kernel_driver(dev, 1); /* not supported everywhere: fine */
    d = libusb_get_device(dev);
    ep_in = ep_out = 0;
    if (libusb_get_active_config_descriptor(d, &cfg) == 0) {
        if (cfg->bNumInterfaces > 0 && cfg->interface[0].num_altsetting > 0) {
            ifd = &cfg->interface[0].altsetting[0];
            for (i = 0; i < ifd->bNumEndpoints; i++) {
                const struct libusb_endpoint_descriptor *e = &ifd->endpoint[i];

                if ((e->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
                if (e->bEndpointAddress & LIBUSB_ENDPOINT_IN) ep_in = e->bEndpointAddress;
                else ep_out = e->bEndpointAddress;
            }
        }
        libusb_free_config_descriptor(cfg);
    }
    if (!ep_in || !ep_out) {
        set_status("Scope has no bulk endpoints");
        libusb_close(dev); dev = NULL;
        return -1;
    }
    if ((r = libusb_claim_interface(dev, 0)) < 0) {
        set_status("Cannot claim scope: %s", libusb_strerror(r));
        libusb_close(dev); dev = NULL;
        return -1;
    }
    tag = 1;
    return identify();
}

/* ------------------------------------------------------------ simulator */

static const double tsteps[] = { 5e-9, 1e-8, 2e-8, 5e-8, 1e-7, 2e-7, 5e-7, 1e-6, 2e-6, 5e-6, 1e-5,
    2e-5, 5e-5, 1e-4, 2e-4, 5e-4, 1e-3, 2e-3, 5e-3, 1e-2, 2e-2, 5e-2, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50 };
#define NTSTEPS ((int)(sizeof tsteps / sizeof tsteps[0]))

/* next 1-2-5 value up or down from v, within lo..hi */
static double step125(double v, int dir, double lo, double hi)
{
    double e = pow(10, floor(log10(v) + 1e-9)), m = v / e, nv;

    if (dir > 0) nv = m < 1.5 ? 2 * e : m < 3.5 ? 5 * e : 10 * e;
    else nv = m > 3.5 ? 2 * e : m > 1.5 ? e : 0.5 * e;
    if (nv < lo) nv = lo;
    if (nv > hi) nv = hi;
    return nv;
}

static void sim_init(void)
{
    int c;

    st.connected = 1;
    st.tscale = 2e-4;
    st.toffs = 0;
    for (c = 0; c < SCOPE_NCH; c++) {
        st.chdisp[c] = c < 2;
        st.chscale[c] = 1.0;
        st.choffs[c] = c == 1 ? -2.0 : 0;
        st.chprobe[c] = 10;
    }
    st.trlev = 0.5;
    snprintf(st.trsrc, sizeof st.trsrc, "CHAN1");
    snprintf(st.trstat, sizeof st.trstat, "AUTO");
    snprintf(st.trswe, sizeof st.trswe, "AUTO");
    st.have_settings = 1;
    snprintf(st.idn, sizeof st.idn, "RIGOL TECHNOLOGIES,DS1104Z Plus,SIMULATED,00.00");
    snprintf(st.model, sizeof st.model, "DS1104Z Plus");
    snprintf(st.serial, sizeof st.serial, "SIMULATED");
    set_status("Simulated scope");
}

/* the simulator takes the same commands the scope would */
static void sim_command(const char *cmd)
{
    const char *p;
    int    c;
    double v;

    if (!strncmp(cmd, ":SYST:KEY:PRES ", 15)) {
        p = cmd + 15;
        if (!strcmp(p, "RSTOP")) { sim_running = !sim_running; sim_single = 0; }
        else if (!strcmp(p, "SING")) { sim_running = 1; sim_single = 1; }
        else if (!strncmp(p, "CH", 2) && p[2] >= '1' && p[2] <= '4') st.chdisp[p[2] - '1'] ^= 1;
    } else if (!strncmp(cmd, ":SYST:KEY:INCR ", 15) || !strncmp(cmd, ":SYST:KEY:DECR ", 15)) {
        int dir = cmd[10] == 'I' ? 1 : -1;

        p = cmd + 15;
        if (!strncmp(p, "HSC", 3)) {
            for (c = 0; c < NTSTEPS; c++) if (fabs(tsteps[c] - st.tscale) < st.tscale * 0.01) break;
            c += dir;
            if (c < 0) c = 0;
            if (c >= NTSTEPS) c = NTSTEPS - 1;
            st.tscale = tsteps[c];
        } else if (!strncmp(p, "HPOS", 4)) st.toffs += dir * st.tscale / 10;
        else if (!strncmp(p, "TLEV", 4)) st.trlev += dir * st.chscale[0] / 25;
    } else if (!strcmp(cmd, ":AUT")) {
        st.tscale = 5e-4; st.toffs = 0; st.chscale[0] = 1; st.choffs[0] = 0; st.trlev = 0; sim_running = 1;
    } else if (!strcmp(cmd, ":RUN")) sim_running = 1;
    else if (!strcmp(cmd, ":STOP")) sim_running = 0;
    else if (!strcmp(cmd, ":TLHA")) st.trlev = 0;
    else if (!strcmp(cmd, ":TIM:MAIN:OFFS 0")) st.toffs = 0;
    else if (sscanf(cmd, ":CHAN%d:SCAL %lf", &c, &v) == 2 && c >= 1 && c <= 4) st.chscale[c - 1] = v;
    else if (sscanf(cmd, ":CHAN%d:OFFS %lf", &c, &v) == 2 && c >= 1 && c <= 4) st.choffs[c - 1] = v;
    snprintf(st.trstat, sizeof st.trstat, sim_running ? "AUTO" : "STOP");
}

/* draw a screen that looks enough like the scope's own */
static void sim_frame(void)
{
    static uint8_t  pix[SCREEN_H][SCREEN_W][3];
    static const uint8_t chcol[4][3] = { { 0, 255, 255 }, { 255, 255, 0 }, { 255, 0, 255 }, { 255, 128, 0 } }; /* BGR */
    int    x, y, c, gx1 = 85, gy1 = 40, gx2 = 685, gy2 = 440, gw = gx2 - gx1, gh = gy2 - gy1;
    double t, v, py;
    long   t0 = now_ms();

    memset(pix, 0, sizeof pix);
    /* the bars: top and bottom status, and the menu column at the right */
    for (y = 0; y < 30; y++) for (x = 0; x < SCREEN_W; x++) { pix[y][x][0] = 40; pix[y][x][1] = 40; pix[y][x][2] = 40; }
    for (y = 450; y < SCREEN_H; y++) for (x = 0; x < SCREEN_W; x++) { pix[y][x][0] = 30; pix[y][x][1] = 30; pix[y][x][2] = 30; }
    for (y = 30; y < 450; y++) for (x = 700; x < SCREEN_W; x++) { pix[y][x][0] = 56; pix[y][x][1] = 56; pix[y][x][2] = 56; }
    /* the run state and the channel tabs along the bottom */
    for (y = 6; y < 24; y++) for (x = 90; x < 130; x++) { pix[y][x][0] = 0; pix[y][x][1] = sim_running ? 180 : 0; pix[y][x][2] = sim_running ? 0 : 200; }
    for (c = 0; c < SCOPE_NCH; c++)
        for (y = 455; y < 475; y++) for (x = 10 + c * 118; x < 110 + c * 118; x++) {
            pix[y][x][0] = st.chdisp[c] ? chcol[c][0] / 3 : 50;
            pix[y][x][1] = st.chdisp[c] ? chcol[c][1] / 3 : 50;
            pix[y][x][2] = st.chdisp[c] ? chcol[c][2] / 3 : 50;
        }
    /* graticule: 12 by 8 divisions, dotted */
    for (c = 0; c <= 12; c++) {
        x = gx1 + c * gw / 12;
        for (y = gy1; y <= gy2; y += (c == 6 ? 2 : 5)) pix[y][x][0] = pix[y][x][1] = pix[y][x][2] = c == 6 ? 110 : 70;
    }
    for (c = 0; c <= 8; c++) {
        y = gy1 + c * gh / 8;
        for (x = gx1; x <= gx2; x += (c == 4 ? 2 : 5)) pix[y][x][0] = pix[y][x][1] = pix[y][x][2] = c == 4 ? 110 : 70;
    }
    /* traces: a 1 kHz 3 V sine on 1, a square on 2, a ramp on 3, noise on 4 */
    if (sim_running) sim_phase += 0.37;
    for (c = 0; c < SCOPE_NCH; c++) {
        int lasty = -1;

        if (!st.chdisp[c]) continue;
        for (x = gx1; x <= gx2; x++) {
            t = ((x - gx1) / (double)gw - 0.5) * 12 * st.tscale - st.toffs;
            switch (c) {
                case 0:  v = 1.5 * sin(2 * M_PI * 1000 * t + sim_phase); break;
                case 1:  v = fmod(fabs(t) * 1000 + 0.5 * (t < 0), 1.0) < 0.5 ? 2.0 : 0.0; break;
                case 2:  v = fmod(fabs(t) * 2000, 1.0) * 2 - 1; break;
                default: v = ((rand() % 100) - 50) / 200.0; break;
            }
            py = gy1 + gh / 2.0 - (v + st.choffs[c]) / st.chscale[c] * (gh / 8.0);
            y = (int)lround(py);
            if (y < gy1) y = gy1;
            if (y > gy2) y = gy2;
            if (lasty < 0) lasty = y;
            for (;;) {
                pix[lasty][x][0] = chcol[c][0]; pix[lasty][x][1] = chcol[c][1]; pix[lasty][x][2] = chcol[c][2];
                if (lasty == y) break;
                lasty += lasty < y ? 1 : -1;
            }
        }
    }
    /* trigger level marker at the right edge, orange */
    y = (int)lround(gy1 + gh / 2.0 - (st.trlev + st.choffs[0]) / st.chscale[0] * (gh / 8.0));
    if (y >= gy1 && y <= gy2)
        for (x = gx2 + 2; x < gx2 + 12; x++) { pix[y][x][0] = 0; pix[y][x][1] = 140; pix[y][x][2] = 255; pix[y - 1][x][0] = 0; pix[y - 1][x][1] = 140; pix[y - 1][x][2] = 255; }
    frame_ready(pix[0][0], SCREEN_W, SCREEN_H, (int)(now_ms() - t0));
    if (sim_single && sim_running) { sim_running = 0; snprintf(st.trstat, sizeof st.trstat, "STOP"); }
}

/* ------------------------------------------------------------ the thread */

static void worker(void)
{
    char cmd[CMDLEN];
    long t, last_cycle = 0;
    int  busy;

    ami_lock(lk);
    my_gen = gen;
    ami_unlock(lk);
    if (setjmp(worker_exit)) {
        /* retired: a later thread has the link now; just go */
        if (netf) fclose(netf);
        return;
    }
    st.connected = 0; /* a replacement starts over; the first is already so */
    next_retry = 0;
    while (!stop_req) {
        if (st.sim) {
            while (dequeue(cmd)) sim_command(cmd);
            sim_frame();
            publish();
            sleep_ms(200);
            continue;
        }
        t = now_ms();
        if (!linked()) {
            if (t >= next_retry) {
                if ((st.lan ? open_lan() : open_device()) < 0) {
                    next_retry = now_ms() + RETRY_MS;
                    publish();
                }
            }
            sleep_ms(100);
            continue;
        }
        /* the front panel first: keys and knobs are what the user is waiting on */
        busy = 0;
        while (linked() && dequeue(cmd)) {
            if (strchr(cmd, '?')) { char b[128]; query(cmd, b, sizeof b); }
            else send_cmd(cmd);
            busy = 1;
        }
        if (!linked()) continue;
        if (busy) {
            next_settings = 0; /* a change: read the settings straight away */
            sleep_ms(50);      /* let the scope act before its screen is taken */
        }
        if (t - last_cycle < MIN_CYCLE_MS) sleep_ms((int)(MIN_CYCLE_MS - (t - last_cycle)));
        last_cycle = now_ms();
        if (t >= next_settings) {
            long t1 = now_ms();

            if (read_settings() == 0) {
                /* every frame when the pass is quick (the LAN), else every 2 s */
                next_settings = now_ms() - t1 < SETTINGS_FAST ? 0 : now_ms() + SETTINGS_MS;
                publish();
            }
            if (!linked()) continue;
        }
        fetch_frame();
    }
    if (dev) {
        libusb_release_interface(dev, 0);
        libusb_close(dev);
        dev = NULL;
    }
    if (netf) {
        fclose(netf);
        netf = NULL;
    }
    thread_done = 1;
}

/* ------------------------------------------------------------ interface */

void scope_init(scope_state *s, int sim, const char *host)
{
    const char *tmp = getenv("TMPDIR");
    char *p;
    int c;

    if (!tmp || !*tmp) tmp = "/tmp";
    memset(&st, 0, sizeof st);
    st.sim = sim;
    st.lan = host != NULL;
    if (host) {
        snprintf(lan_host, sizeof lan_host, "%s", host);
        p = strrchr(lan_host, ':');
        if (p && atoi(p + 1) > 0) { lan_port = atoi(p + 1); *p = 0; }
    }
    for (c = 0; c < SCOPE_NCH; c++) { st.chscale[c] = 1; st.chprobe[c] = 10; }
    st.tscale = 1e-3;
    for (c = 0; c < 2; c++)
        snprintf(frame_path[c], sizeof frame_path[c], "%s/mso1104-%d-%c.bmp", tmp, (int)getpid(), 'a' + c);
    if (sim) sim_init();
    else if (st.lan && lan_host[0]) set_status("Connecting to %s", lan_host);
    else if (st.lan) set_status("Looking for the scope on the LAN");
    else set_status("Looking for the scope on USB");
    lk = ami_initlock();
    shared = st;
    shared_changed = 1;
    *s = st;
    if (st.lan) {
        signal(SIGPIPE, SIG_IGN);  /* a write to a scope that went away is an error, not a death */
        ami_neterror(net_error);   /* and so is a failed open */
    }
    ami_newthread(worker);
}

/* Also the LAN watchdog. The link thread blocks for each reply with no
   bound, so if the scope goes quiet it is this, the front panel's timer,
   that notices: no new frame for STALE_MS while connected. The thread is
   retired by bumping the generation and another is started; the old one
   leaves when its read returns, which may be never, and that is fine. */
int scope_poll(scope_state *s)
{
    static unsigned last_seq;
    static long     fresh_at;
    int  changed, restart = 0;
    long t = now_ms();

    ami_lock(lk);
    if (shared.lan && shared.connected && !shared.sim) {
        if (shared.frame_seq != last_seq || !fresh_at) { last_seq = shared.frame_seq; fresh_at = t; }
        else if (t - fresh_at > STALE_MS) {
            gen++;
            shared.connected = 0;
            shared.errors++;
            snprintf(shared.status, sizeof shared.status, "No reply from %s: reconnecting", shared.host);
            shared_changed = 1;
            fresh_at = 0;
            restart = 1;
        }
    } else fresh_at = 0;
    changed = shared_changed;
    if (changed) {
        shared.active = s->active;
        *s = shared;
        shared_changed = 0;
    }
    ami_unlock(lk);
    if (restart) ami_newthread(worker);
    return changed;
}

void scope_shutdown(scope_state *s)
{
    int i;

    stop_req = 1;
    for (i = 0; i < 40 && !thread_done; i++) sleep_ms(50); /* a frame fetch may be under way */
    for (i = 0; i < 2; i++) unlink(frame_path[i]);
    s->connected = 0;
}

void scope_key(scope_state *s, const char *key)
{
    char c[CMDLEN];

    (void)s;
    snprintf(c, sizeof c, ":SYST:KEY:PRES %s", key);
    enqueue(c);
}

void scope_knob(scope_state *s, const char *knob, int dir)
{
    char c[CMDLEN];

    (void)s;
    if (!dir) return;
    snprintf(c, sizeof c, ":SYST:KEY:%s %s,1", dir > 0 ? "INCR" : "DECR", knob);
    enqueue(c);
}

void scope_cmd(scope_state *s, const char *cmd)
{
    (void)s;
    enqueue(cmd);
}

void scope_set_active(scope_state *s, int ch)
{
    if (ch < 0 || ch >= SCOPE_NCH) return;
    s->active = ch;
    ami_lock(lk);
    shared.active = ch;
    ami_unlock(lk);
}

/* The vertical knobs step the setting from the value last read back, kept up
   to date optimistically so that quick turns compound before the readback. */
void scope_vscale(scope_state *s, int dir)
{
    char   c[CMDLEN];
    int    ch = s->active;
    double pr = s->chprobe[ch] > 0 ? s->chprobe[ch] : 1, v;

    v = step125(s->chscale[ch], dir, 1e-3 * pr, 10 * pr);
    s->chscale[ch] = v;
    ami_lock(lk);
    shared.chscale[ch] = v;
    ami_unlock(lk);
    snprintf(c, sizeof c, ":CHAN%d:SCAL %.4g", ch + 1, v);
    enqueue(c);
}

void scope_vpos(scope_state *s, int dir)
{
    char   c[CMDLEN];
    int    ch = s->active;
    double v = s->choffs[ch] + dir * s->chscale[ch] / 5; /* a fifth of a division */

    s->choffs[ch] = v;
    ami_lock(lk);
    shared.choffs[ch] = v;
    ami_unlock(lk);
    snprintf(c, sizeof c, ":CHAN%d:OFFS %.4g", ch + 1, v);
    enqueue(c);
}

void scope_vpos_zero(scope_state *s)
{
    char c[CMDLEN];
    int  ch = s->active;

    s->choffs[ch] = 0;
    ami_lock(lk);
    shared.choffs[ch] = 0;
    ami_unlock(lk);
    snprintf(c, sizeof c, ":CHAN%d:OFFS 0", ch + 1);
    enqueue(c);
}

/* libpng writes through this rather than its own stdio call: the FILE
   comes from this program's stdio layer, which is not the one inside the
   shared library (the AMI tool kit wraps stdio) */
static void png_file_write(png_structp png, png_bytep data, png_size_t len)
{
    FILE *f = png_get_io_ptr(png);

    if (fwrite(data, 1, len, f) != len) png_error(png, "write");
}

static void png_file_flush(png_structp png)
{
    (void)png;
}

/* the last picture, encoded as PNG with libpng */
long scope_save_png(scope_state *s, const char *path)
{
    FILE       *f;
    uint8_t    *pix = NULL, **rows = NULL;
    int         w, h, y;
    long        n = 0;
    png_structp png = NULL;
    png_infop   info = NULL;

    (void)s;
    ami_lock(lk);
    if (pix_last) {
        pix = malloc((size_t)pix_w * pix_h * 3);
        if (pix) memcpy(pix, pix_last, (size_t)pix_w * pix_h * 3);
        w = pix_w; h = pix_h;
    }
    ami_unlock(lk);
    if (!pix) return 0;
    rows = malloc((size_t)h * sizeof *rows);
    f = fopen(path, "wb");
    if (!rows || !f) goto done;
    for (y = 0; y < h; y++) rows[y] = pix + (size_t)y * w * 3;
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png) info = png_create_info_struct(png);
    if (!info) goto done;
    if (setjmp(png_jmpbuf(png))) goto done;
    png_set_write_fn(png, f, png_file_write, png_file_flush);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_set_bgr(png);
    png_write_image(png, rows);
    png_write_end(png, NULL);
    n = ftell(f);
done:
    if (png) png_destroy_write_struct(&png, info ? &info : NULL);
    if (f) fclose(f);
    free(rows);
    free(pix);
    return n;
}
