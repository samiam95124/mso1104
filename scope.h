/*
 * Rigol MSO1000Z / DS1000Z scope link: SCPI commands over USBTMC (libusb)
 * or over the scope's raw socket on its LAN port.
 *
 * The scope is USB 1ab1:04ce, or TCP port 5555 on the LAN, where it can be
 * found by probing the local subnet. It is driven from a thread of its own, so the
 * window never waits on the bus: the front panel queues key presses, knob
 * turns and commands, and the thread sends them, reads the settings back
 * every couple of seconds, and fetches the screen picture again and again.
 * scope_poll() copies the thread's state out for the front panel.
 *
 * Two quirks of the instrument shape the transport (see scope.c): it sends
 * 64 byte packets on a high speed link, and a VISA style open sequence
 * (remote enable, capabilities) hangs its USB stack until the cable is
 * pulled. Only bulk transfers are used here.
 */
#ifndef SCOPE_H
#define SCOPE_H

#define SCOPE_VID  0x1ab1
#define SCOPE_PID  0x04ce
#define SCOPE_PORT 5555        /* raw SCPI socket on the LAN port */

#define SCOPE_NCH 4

typedef struct {
    /* link */
    int    connected;          /* device open and answering */
    int    sim;                /* simulated scope, no hardware */
    int    lan;                /* link is the LAN port, else USB */
    char   host[64];           /* LAN: the address in use, or found */
    char   status[128];        /* short human readable link status */
    char   idn[160];           /* *IDN? reply */
    char   model[32];          /* from idn: DS1104Z Plus */
    char   serial[32];

    /* settings, as last read back: just what the panel shows and steps */
    double tscale;             /* s/div */
    double toffs;              /* s; the simulator's only, not read back */
    int    chdisp[SCOPE_NCH];
    double chscale[SCOPE_NCH]; /* V/div */
    double choffs[SCOPE_NCH];  /* V */
    double chprobe[SCOPE_NCH]; /* ratio */
    double trlev;              /* V */
    char   trsrc[12];          /* CHAN1 .. */
    char   trstat[8];          /* TD WAIT RUN AUTO STOP */
    char   trswe[8];           /* AUTO NORM SING */
    int    have_settings;

    /* front panel's choice: the channel the vertical knobs act on, 0 based */
    int    active;

    /* the screen picture: a 24 bit BMP file rewritten for each frame, with
       the sequence number going up each time */
    char     frame[256];
    unsigned frame_seq;
    int      frame_ms;         /* time the last fetch took */
    long     frames;           /* fetched since connect */
    long     errors;           /* bus errors since start */
} scope_state;

/* Open the scope and start the link thread. sim nonzero simulates. host
   NULL uses USB; "" finds the scope on the LAN by probing the local subnet;
   otherwise it is the scope's address or name, with an optional :port. */
void scope_init(scope_state *s, int sim, const char *host);

/* Service the link: copy the thread's state out. Returns nonzero if changed. */
int scope_poll(scope_state *s);

/* Stop the thread, free the device and the frame files. */
void scope_shutdown(scope_state *s);

/* Requests, queued for the thread. */
void scope_key(scope_state *s, const char *key);        /* :SYST:KEY:PRES */
void scope_knob(scope_state *s, const char *knob, int dir); /* :SYST:KEY:INCR/DECR */
void scope_cmd(scope_state *s, const char *cmd);        /* any set command */

/* The vertical knobs have no key name, so they go as settings on the active
   channel: scale steps 1-2-5, position steps by a fraction of a division. */
void scope_set_active(scope_state *s, int ch);
void scope_vscale(scope_state *s, int dir);
void scope_vpos(scope_state *s, int dir);
void scope_vpos_zero(scope_state *s);

/* Copy the most recent screen picture, as PNG, to a file. Returns bytes. */
long scope_save_png(scope_state *s, const char *path);

#endif
