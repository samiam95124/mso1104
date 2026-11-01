/*
 * Rigol DS1000Z / MSO1000Z link. See scope.h.
 *
 * Transport: USBTMC over libusb, bulk transfers only. The DS1000Z declares
 * and uses 64 byte bulk packets although it runs at high speed. Linux
 * presents every one of them as a short packet, Windows (WinUSB) keeps to
 * the declared size and ends a read only on a short packet or a full
 * buffer, so a reply is read in a way that suits both: its first packet
 * alone, for the header, then exactly the packets the header promises.
 * When the reply ends on a full packet the scope follows it with a zero
 * length packet, which is taken before the next request goes out. The
 * scope also pads replies inconsistently, so a read stops at the declared
 * size, not the padded one. A reply larger than the request comes in
 * several transfers, each ended by the EOM flag of the last; the screen
 * comes as a 500 byte transfer and then the rest.
 *
 * The screen picture is fetched as PNG (about 35 to 50 kB; 0.65 s on
 * Windows, of which 0.63 s is the scope rendering it; 1.2 s on Linux),
 * decoded with libpng and written as a 24 bit BMP, the only picture format
 * the AMI graphics module loads. Two files alternate, so the front panel
 * can load one while the next is written.
 *
 * AUTO holds the scope off the bus for about 6 s; the reply timeout allows
 * for it. MSO1104_TRACE set in the environment logs the timing of every
 * frame, every slow reply and every link loss to stderr.
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
#include <libusb.h>
#include <png.h>

#include <localdefs.h>
#include <services.h>

#include "scope.h"

#define QMAX          64      /* command queue depth */
#define CMDLEN        64
#define SETTINGS_MS   2000    /* re-read settings this often */
#define RETRY_MS      2000    /* reconnect interval */
#define REPLY_MS      15000   /* bus timeout for a reply: AUTO holds the
                                 scope off the bus for 5 s or more, and a
                                 shorter wait took that for a dead link */
#define FRAME_MS      15000   /* bus timeout for the screen picture */
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
static uint8_t     *png_last;
static size_t       png_last_len;

/* ------------------------------------------------------------ thread state */

static scope_state           st;
static libusb_context       *ctx;
static libusb_device_handle *dev;
static uint8_t               ep_in, ep_out;
static uint8_t               tag = 1;
static char                  frame_path[2][256];
static int                   frame_ix;
static long                  next_settings, next_retry;
static int                   sim_running = 1, sim_single;
static double                sim_phase;
static int                   trace;   /* MSO1104_TRACE set: timings to stderr */

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
    size_t   acclen = 0, len, want;
    uint8_t  h[12], zlp[64];
    uint32_t tsize = 0;
    int      r, got, eom, zeros;

    for (;;) {
        tmc_header(h, 2, 1u << 20, 0); /* REQUEST_DEV_DEP_MSG_IN */
        r = libusb_bulk_transfer(dev, ep_out, h, 12, &got, 2000);
        if (r < 0) goto fail;
        /* The first packet carries the header. It is read alone, and then
           exactly the packets the header says are coming are asked for, so
           that a read ends when its bytes are in and never waits on a short
           packet the scope may not send. A read sized larger than the reply
           did: on Windows, where WinUSB keeps to the 64 byte packets the
           scope declares, a reply that happened to fill its last packet left
           the read waiting until the timeout, about one screen in forty.
           Linux presents each packet as a short one and never noticed. A
           reply whose last packet is full is followed by a zero length
           packet, and that is taken before the next request goes out: left
           on the pipe it corrupted the next reply from its fourth packet
           on, under WinUSB at least. (The old test, a last read of exactly
           64 bytes, held on Linux only, where that is what a read returns.) */
        buf = malloc(64);
        if (!buf) { r = LIBUSB_ERROR_NO_MEM; goto fail; }
        len = 0; zeros = 0;
        while (len < 12) {
            r = libusb_bulk_transfer(dev, ep_in, buf, 64, &got, timeout);
            if (r < 0) goto fail;
            if (got == 0) { if (++zeros > 4) { r = LIBUSB_ERROR_IO; goto fail; } continue; }
            if (got < 12) { r = LIBUSB_ERROR_IO; goto fail; }
            len = (size_t)got;
        }
        tsize = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
        eom = buf[8] & 1;
        if (trace && buf[1] != h[1])
            fprintf(stderr, "scope: %ld: reply tag %d for request tag %d, %u bytes%s\n",
                    now_ms() % 100000, buf[1], h[1], tsize, eom ? " eom" : "");
        want = ((12 + (size_t)tsize + 63) / 64) * 64; /* whole packets, padding included */
        if (want > 64) {
            uint8_t *nb = realloc(buf, want);

            if (!nb) { r = LIBUSB_ERROR_NO_MEM; goto fail; }
            buf = nb;
        }
        while (len < 12 + tsize) {
            r = libusb_bulk_transfer(dev, ep_in, buf + len, (int)(want - len), &got, timeout);
            if (r < 0) goto fail;
            if (got == 0 && ++zeros > 4) { r = LIBUSB_ERROR_IO; goto fail; }
            len += (size_t)got;
        }
        if (len % 64 == 0) libusb_bulk_transfer(dev, ep_in, zlp, 64, &got, 300);
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

static void link_fail(const char *what, int r)
{
    set_status("%s: %s", what, libusb_strerror(r));
    if (trace) fprintf(stderr, "scope: %ld: link lost on %s: %s\n", now_ms() % 100000, what, libusb_strerror(r));
    if (dev) {
        libusb_release_interface(dev, 0);
        libusb_reset_device(dev); /* clears a half finished transfer in the scope */
        libusb_close(dev);
        dev = NULL;
    }
    st.connected = 0;
    st.errors++;
    next_retry = now_ms() + RETRY_MS;
    publish();
}

/* query: reply as a trimmed string in buf; nonzero on failure (link dropped) */
static int query(const char *cmd, char *buf, size_t buflen)
{
    uint8_t *rep;
    size_t   n;
    int      r;

    long     t0 = now_ms();

    if ((r = tmc_write(cmd)) < 0) { link_fail(cmd, r); return -1; }
    if ((r = tmc_read(&rep, &n, REPLY_MS)) < 0) { link_fail(cmd, r); return -1; }
    if (trace && now_ms() - t0 > 100)
        fprintf(stderr, "scope: %ld: %s took %ld ms\n", now_ms() % 100000, cmd, now_ms() - t0);
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
    int r;

    if ((r = tmc_write(cmd)) < 0) { link_fail(cmd, r); return -1; }
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

/* decode a PNG in memory and write it as a BMP; returns 0 on success */
static int png_to_bmp(const uint8_t *data, size_t n, const char *path)
{
    png_structp png = NULL;
    png_infop   info = NULL;
    png_bytep  *rows = NULL;
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
    rows = calloc(h, sizeof *rows);
    if (!rows) goto done;
    for (y = 0; y < h; y++) {
        rows[y] = malloc(w * 3);
        if (!rows[y]) goto done;
    }
    png_read_image(png, rows);
    ok = bmp_write(path, (int)w, (int)h, rows);
done:
    if (rows) {
        for (y = 0; y < h; y++) free(rows[y]);
        free(rows);
    }
    png_destroy_read_struct(&png, &info, NULL);
    return ok;
}

/* a new frame file is ready: tell the front panel, and keep the PNG */
static void publish_frame(const char *path, int ms, const uint8_t *png, size_t n)
{
    ami_lock(lk);
    snprintf(shared.frame, sizeof shared.frame, "%s", path);
    shared.frame_seq++;
    shared.frame_ms = ms;
    shared.frames++;
    shared_changed = 1;
    if (png) {
        uint8_t *np = realloc(png_last, n);

        if (np) { memcpy(np, png, n); png_last = np; png_last_len = n; }
    }
    ami_unlock(lk);
    st.frame_seq = shared.frame_seq;
    st.frames = shared.frames;
    st.frame_ms = ms;
    snprintf(st.frame, sizeof st.frame, "%s", path);
}

/* fetch the screen as PNG, convert, publish */
static void fetch_frame(void)
{
    uint8_t *rep;
    size_t   n, start, len;
    long     t0 = now_ms(), t1;
    int      r, digits;

    if ((r = tmc_write(":DISP:DATA? ON,OFF,PNG")) < 0) { link_fail("screen", r); return; }
    if ((r = tmc_read(&rep, &n, FRAME_MS)) < 0) { link_fail("screen", r); return; }
    /* IEEE 488.2 block: '#', one digit n, n digits of length, data */
    if (n < 2 || rep[0] != '#') {
        if (trace) fprintf(stderr, "scope: %ld: screen reply is not a block: %u bytes \"%.20s\"\n",
                           now_ms() % 100000, (unsigned)n, rep);
        free(rep);
        return;
    }
    digits = rep[1] - '0';
    if (digits < 1 || digits > 9 || n < (size_t)(2 + digits)) { free(rep); return; }
    len = 0;
    for (r = 0; r < digits; r++) len = len * 10 + (size_t)(rep[2 + r] - '0');
    start = (size_t)(2 + digits);
    if (start + len > n) len = n - start;
    frame_ix ^= 1;
    t1 = now_ms();
    if (png_to_bmp(rep + start, len, frame_path[frame_ix]) == 0)
        publish_frame(frame_path[frame_ix], (int)(now_ms() - t0), rep + start, len);
    if (trace)
        fprintf(stderr, "scope: %ld: frame %ld: fetch %ld ms, decode %ld ms, %u bytes\n",
                now_ms() % 100000, st.frames, t1 - t0, now_ms() - t1, (unsigned)len);
    free(rep);
}

/* ------------------------------------------------------------ settings */

static int read_settings(void)
{
    char   b[64];
    int    c;
    double v;

    if (qnum(":TIM:SCAL?", &st.tscale)) return -1;
    if (qnum(":TIM:MAIN:OFFS?", &st.toffs)) return -1;
    if (qnum(":ACQ:SRAT?", &st.srate)) return -1;
    if (query(":ACQ:MDEP?", st.mdepth, sizeof st.mdepth)) return -1;
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
        snprintf(b, sizeof b, ":CHAN%d:COUP?", c + 1);
        if (query(b, st.chcoup[c], sizeof st.chcoup[c])) return -1;
    }
    if (qnum(":TRIG:EDG:LEV?", &st.trlev)) return -1;
    if (query(":TRIG:EDG:SOUR?", st.trsrc, sizeof st.trsrc)) return -1;
    if (query(":TRIG:STAT?", st.trstat, sizeof st.trstat)) return -1;
    if (query(":TRIG:MODE?", st.trmode, sizeof st.trmode)) return -1;
    if (query(":TRIG:SWE?", st.trswe, sizeof st.trswe)) return -1;
    st.have_settings = 1;
    return 0;
}

/* ------------------------------------------------------------ device */

static int open_device(void)
{
    struct libusb_config_descriptor *cfg;
    const struct libusb_interface_descriptor *ifd;
    libusb_device *d;
    char   b[160], *p, *q;
    int    r, i;

    if (!ctx && (r = libusb_init(&ctx)) < 0) {
        set_status("libusb: %s", libusb_strerror(r));
        return -1;
    }
    /* find the scope by hand rather than by open_device_with_vid_pid, so that
       a scope that is there but cannot be opened is reported as such: on
       Linux that is a permission (see the udev rule in the README), and on
       Windows it is the driver, which has to be WinUSB for libusb to open
       the device at all */
    {
        libusb_device **list;
        ssize_t n, i;

        dev = NULL;
        r = 0;
        n = libusb_get_device_list(ctx, &list);
        for (i = 0; i < n && !dev; i++) {
            struct libusb_device_descriptor dd;

            if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
            if (dd.idVendor != SCOPE_VID || dd.idProduct != SCOPE_PID) continue;
            if ((r = libusb_open(list[i], &dev)) != 0) dev = NULL;
        }
        if (n >= 0) libusb_free_device_list(list, 1);
        if (!dev) {
            if (r != 0)
#ifdef _WIN32
                set_status("Scope found but not opened (%s): it needs the WinUSB driver, see README",
                           libusb_strerror(r));
#else
                set_status("Scope found but not opened: %s", libusb_strerror(r));
#endif
            else set_status("No scope found on USB");
            return -1;
        }
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
    st.srate = 5e8;
    snprintf(st.mdepth, sizeof st.mdepth, "AUTO");
    for (c = 0; c < SCOPE_NCH; c++) {
        st.chdisp[c] = c < 2;
        st.chscale[c] = 1.0;
        st.choffs[c] = c == 1 ? -2.0 : 0;
        st.chprobe[c] = 10;
        snprintf(st.chcoup[c], sizeof st.chcoup[c], "DC");
    }
    st.trlev = 0.5;
    snprintf(st.trsrc, sizeof st.trsrc, "CHAN1");
    snprintf(st.trstat, sizeof st.trstat, "AUTO");
    snprintf(st.trmode, sizeof st.trmode, "EDGE");
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
    static uint8_t *rows[SCREEN_H];
    static const uint8_t chcol[4][3] = { { 0, 255, 255 }, { 255, 255, 0 }, { 255, 0, 255 }, { 255, 128, 0 } }; /* BGR */
    int    x, y, c, gx1 = 85, gy1 = 40, gx2 = 685, gy2 = 440, gw = gx2 - gx1, gh = gy2 - gy1;
    double t, v, py;
    long   t0 = now_ms();

    for (y = 0; y < SCREEN_H; y++) rows[y] = pix[y][0];
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
    frame_ix ^= 1;
    if (bmp_write(frame_path[frame_ix], SCREEN_W, SCREEN_H, rows) == 0)
        publish_frame(frame_path[frame_ix], (int)(now_ms() - t0), NULL, 0);
    if (sim_single && sim_running) { sim_running = 0; snprintf(st.trstat, sizeof st.trstat, "STOP"); }
}

/* ------------------------------------------------------------ the thread */

static void worker(void)
{
    char cmd[CMDLEN];
    long t;
    int  busy;

    while (!stop_req) {
        if (st.sim) {
            while (dequeue(cmd)) sim_command(cmd);
            sim_frame();
            publish();
            sleep_ms(200);
            continue;
        }
        t = now_ms();
        if (!dev) {
            if (t >= next_retry) {
                if (open_device() < 0) {
                    next_retry = now_ms() + RETRY_MS;
                    publish();
                }
            }
            sleep_ms(100);
            continue;
        }
        /* the front panel first: keys and knobs are what the user is waiting on */
        busy = 0;
        while (dev && dequeue(cmd)) {
            if (trace) fprintf(stderr, "scope: %ld: send %s\n", now_ms() % 100000, cmd);
            if (strchr(cmd, '?')) { char b[128]; query(cmd, b, sizeof b); }
            else send_cmd(cmd);
            busy = 1;
        }
        if (!dev) continue;
        if (busy) {
            next_settings = 0; /* a change: read the settings straight away */
            sleep_ms(50);      /* let the scope act before its screen is taken */
        }
        if (t >= next_settings) {
            if (read_settings() == 0) {
                next_settings = now_ms() + SETTINGS_MS;
                publish();
            }
            if (!dev) continue;
        }
        fetch_frame();
    }
    if (dev) {
        libusb_release_interface(dev, 0);
        libusb_close(dev);
        dev = NULL;
    }
    thread_done = 1;
}

/* ------------------------------------------------------------ interface */

void scope_init(scope_state *s, int sim)
{
    const char *tmp = getenv("TMPDIR");
    int c;

#ifdef _WIN32
    if (!tmp || !*tmp) tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = getenv("TMP");
    if (!tmp || !*tmp) tmp = ".";
#else
    if (!tmp || !*tmp) tmp = "/tmp";
#endif
    memset(&st, 0, sizeof st);
    trace = getenv("MSO1104_TRACE") != NULL;
    st.sim = sim;
    for (c = 0; c < SCOPE_NCH; c++) { st.chscale[c] = 1; st.chprobe[c] = 10; }
    st.tscale = 1e-3;
    for (c = 0; c < 2; c++)
        snprintf(frame_path[c], sizeof frame_path[c], "%s/mso1104-%d-%c.bmp", tmp, (int)getpid(), 'a' + c);
    if (sim) sim_init();
    else set_status("Looking for the scope on USB");
    lk = ami_initlock();
    shared = st;
    shared_changed = 1;
    *s = st;
    ami_newthread(worker);
}

int scope_poll(scope_state *s)
{
    int changed;

    ami_lock(lk);
    changed = shared_changed;
    if (changed) {
        shared.active = s->active;
        *s = shared;
        shared_changed = 0;
    }
    ami_unlock(lk);
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

long scope_save_png(scope_state *s, const char *path)
{
    FILE *f;
    long  n = 0;

    (void)s;
    ami_lock(lk);
    if (png_last && png_last_len) {
        f = fopen(path, "wb");
        if (f) {
            n = (long)fwrite(png_last, 1, png_last_len, f);
            fclose(f);
        }
    }
    ami_unlock(lk);
    return n;
}
