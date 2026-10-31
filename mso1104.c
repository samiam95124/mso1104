/*
 * MSO1104 front panel: drive a Rigol MSO1104Z or DS1104Z oscilloscope from
 * the desktop.
 *
 * The window is drawn as the face of the scope, laid out from a photograph
 * of an MSO1104Z: the screen with a column of soft keys each side, the run
 * keys along the top right, the intensity knob and the menu keys, the
 * vertical section with the four channel keys and its knobs, the
 * horizontal section, the trigger section, and the power key, USB port,
 * logic pod and the four inputs along the bottom. The screen shows the
 * instrument's own display fetched over USB about once a second. Every key
 * sends the same key press the real one does and the knobs turn the real
 * knobs, so the scope's menus work as they do on the bench. Everything is
 * drawn with the AMI graphics calls and scaled to the window, so the panel
 * can be resized.
 *
 * Use:
 *   mso1104             find the scope on USB
 *   mso1104 --sim       no hardware: a simulated scope
 *
 * Mouse: click a key to press it. Turn a knob by dragging it up or down,
 * by the wheel over it, or by clicking its left or right half; the wheel
 * anywhere else turns the knob last used. A right click pushes a knob
 * (position to zero, trigger level to half). Click a channel's BNC to make
 * it the channel the vertical knobs act on.
 *
 * Keys: 1-4 channel keys, r run/stop, s single, a auto, c clear, h help,
 * m measure, q acquire, d display, u utility, t trigger menu, F1-F6 the
 * soft keys, Left/Right horizontal position, Up/Down horizontal scale,
 * +/- trigger level, PgUp/PgDn vertical scale, Home/End vertical position,
 * x save the screen as PNG to the Desktop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <localdefs.h>
#include <services.h>
#include <graphics.h>

#include "scope.h"

#ifdef SHOT
/* test build: MSO1104_SHOT=<file> takes two pictures of the window and quits */
void screen_capture(void);
void screen_capture_name(const char *fn);
static const char *shot_file;
static long shot_at;
static int  shots;
/* MSO1104_SCRIPT: comma separated actions run 1 s in: a hit name such as CH3
   or UTIL presses that key, HSC+ HSC- HPOS+ TLEV- VSC+ VPOS- turn a knob */
static const char *script;
static long script_at;
static void run_script(void);
#endif

/* the panel is laid out in these units and scaled to the window; the case
   is the top 560, a status strip for the link sits under it */
#define PANW 1050
#define PANH 600
#define CASEH 560

#define TICK_ID  1
#define TICK     500   /* 50 ms, in 100 us units */

/* the screen glass: the scope's 800 by 480 picture goes here */
#define GLX1 84
#define GLY1 70
#define GLX2 604
#define GLY2 382
/* the scope's screen, in pixels; the default window puts it on the glass at
   1:1, since its dim dotted graticule does not survive being shrunk */
#define SCRW 800
#define SCRH 480
#define WINW 1616  /* glass of 520 by 312 panel units scaled to 800 by 480 */
#define WINH 924

#define PICT 1         /* picture slot the screen is loaded into */

/* things that can be clicked */
enum {
    H_NONE,
    H_LMENU, H_L1, H_L2, H_L3, H_L4, H_L5, H_L6, H_LUP, H_LDOWN,
    H_RUP, H_F1, H_F2, H_F3, H_F4, H_F5, H_F6, H_RDOWN,
    H_POWER, H_BNC1, H_BNC2, H_BNC3, H_BNC4,
    H_CLEAR, H_AUTO, H_RUNSTOP, H_SINGLE, H_HELP, H_PRINT,
    H_MULTI, H_MEAS, H_ACQ, H_STOR, H_CURS, H_DISP, H_UTIL,
    H_HPOS, H_HMENU, H_HSCALE,
    H_CH1, H_CH2, H_CH3, H_CH4, H_MATH, H_REF, H_LA, H_SOURCE, H_VPOS, H_VSCALE,
    H_TMODE, H_TMENU, H_TLEVEL, H_FORCE,
    H_SHOT, H_SCREEN,
    H_LAST
};

/* key looks */
enum { KS_SOFT, KS_LIGHT, KS_BLUE, KS_GREEN, KS_CH };

typedef struct { int x1, y1, x2, y2, id; } hitbox;

static scope_state sc;

/* clickable areas, rebuilt on each draw */
static hitbox hits[100];
static int    nhits;
static int    pressed = H_NONE;
static int    mx, my;

/* knobs */
#define DRAGSTEP 8     /* panel units of vertical drag per step */
static int    dragging = H_NONE, drag_y, drag_moved;
static int    last_knob = H_HSCALE;
static double angle[H_LAST];
static struct { double cx, cy, r; } knobpos[H_LAST];

/* the screen picture */
static double   vscale, voffx, voffy;   /* the view transform of this frame */
static unsigned shown_seq;
static int      have_pict;
static long     frame_at;        /* when the last frame arrived */
static double   fps;

static char   note[128];         /* brief message in the status strip */
static long   note_until;

static const struct { int r, g, b; } chcol[SCOPE_NCH] = {
    { 250, 220, 0 }, { 0, 200, 240 }, { 240, 60, 200 }, { 60, 110, 230 }
};

/* milliseconds since the first call. ami_clock wraps every 48 hours, so whole
   milliseconds are taken off the elapsed count each call and accumulated */
static long now_ms(void)
{
    static ami_long ref;
    static long     acc;
    static int      started;
    ami_long        ms;

    if (!started) { ref = ami_clock(); started = 1; }
    ms = ami_elapsed(ref) / 10;
    if (ms > 0) { ref += ms * 10; acc += ms; }
    return acc;
}

/* a file in the user's directory, or in a subdirectory of it when sub is given */
static void user_file(char *fn, int fnl, const char *sub, const char *name, const char *ext)
{
    char usr[256], dir[512];

    ami_getusr(usr, sizeof usr - 1);
    usr[sizeof usr - 1] = 0;
    if (sub) {
        ami_maknam(dir, sizeof dir - 1, usr, (char *)sub, "");
        dir[sizeof dir - 1] = 0;
    } else strcpy(dir, usr);
    ami_maknam(fn, fnl - 1, dir, (char *)name, (char *)ext);
    fn[fnl - 1] = 0;
}

/* local wall clock time broken into year, month, day, hour, minute, second */
static void local_time(ami_long t, int *y, int *mo, int *d, int *h, int *mi, int *sec)
{
    long days = (long)(t / 86400), rem = (long)(t % 86400), era, doe, yoe, doy, mp;

    if (rem < 0) { rem += 86400; days--; }
    *h = (int)(rem / 3600); *mi = (int)(rem / 60 % 60); *sec = (int)(rem % 60);
    days += 10957 + 719468;
    era = (days >= 0 ? days : days - 146096) / 146097;
    doe = days - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*mo <= 2));
}

static void set_note(const char *s)
{
    snprintf(note, sizeof note, "%s", s);
    note_until = now_ms() + 4000;
}

/* a value with an engineering prefix: 200us, 2.00V, 500MSa/s */
static void fmt_eng(double v, const char *unit, char *buf, size_t len)
{
    static const char *pfx[] = { "p", "n", "u", "m", "", "k", "M", "G" };
    double a = fabs(v);
    int    e = 4;

    if (a == 0) { snprintf(buf, len, "0%s", unit); return; }
    while (a >= 1000 && e < 7) { a /= 1000; e++; }
    while (a < 1 && e > 0) { a *= 1000; e--; }
    if (a >= 100) snprintf(buf, len, "%s%.0f%s%s", v < 0 ? "-" : "", a, pfx[e], unit);
    else if (a >= 10) snprintf(buf, len, "%s%.1f%s%s", v < 0 ? "-" : "", a, pfx[e], unit);
    else snprintf(buf, len, "%s%.2f%s%s", v < 0 ? "-" : "", a, pfx[e], unit);
}

/* ---------------------------------------------------------------- actions */

static void save_shot(void)
{
    char     path[600], name[64], msg[128];
    ami_long wall = ami_local(ami_time());
    int      y, mo, d, h, mi, sec;
    long     n;

    local_time(wall, &y, &mo, &d, &h, &mi, &sec);
    snprintf(name, sizeof name, "MSO1104_%04d%02d%02d_%02d%02d%02d", y, mo, d, h, mi, sec);
    user_file(path, sizeof path, "Desktop", name, "png");
    n = scope_save_png(&sc, path);
    if (n > 0) snprintf(msg, sizeof msg, "Saved screen to Desktop/%s.png", name);
    else snprintf(msg, sizeof msg, sc.sim ? "No PNG from the simulator" : "No screen to save yet");
    set_note(msg);
}

static void cycle_sweep(void)
{
    const char *next = !strcmp(sc.trswe, "AUTO") ? "NORM" : !strcmp(sc.trswe, "NORM") ? "SING" : "AUTO";
    char c[32];

    snprintf(c, sizeof c, ":TRIG:SWE %s", next);
    snprintf(sc.trswe, sizeof sc.trswe, "%s", next);
    scope_cmd(&sc, c);
}

static void click(int id)
{
    switch (id) {
        case H_F1: case H_F2: case H_F3: case H_F4: case H_F5: case H_F6: {
            char k[4];

            snprintf(k, sizeof k, "F%d", id - H_F1 + 1);
            scope_key(&sc, k);
            break;
        }
        case H_LMENU: case H_L1: case H_L2: case H_L3: case H_L4: case H_L5: case H_L6:
        case H_LUP: case H_LDOWN: case H_RUP: case H_RDOWN:
            set_note("That key has no USB command on this scope");
            break;
        case H_PRINT:   save_shot(); break;
        case H_POWER:   set_note("The power key is not reachable over USB"); break;
        case H_BNC1: case H_BNC2: case H_BNC3: case H_BNC4:
            scope_set_active(&sc, id - H_BNC1);
            break;
        case H_CLEAR:   scope_key(&sc, "CLE"); break;
        case H_AUTO:    scope_cmd(&sc, ":AUT"); break;
        case H_RUNSTOP: scope_key(&sc, "RSTOP"); break;
        case H_SINGLE:  scope_key(&sc, "SING"); break;
        case H_HELP:    scope_key(&sc, "HELP"); break;
        case H_MEAS:    scope_key(&sc, "MEAS"); break;
        case H_ACQ:     scope_key(&sc, "ACQ"); break;
        case H_STOR:    scope_key(&sc, "STOR"); break;
        case H_CURS:    scope_key(&sc, "CURS"); break;
        case H_DISP:    scope_key(&sc, "DISP"); break;
        case H_UTIL:    scope_key(&sc, "UTIL"); break;
        case H_HMENU:   scope_key(&sc, "HMEN"); break;
        case H_CH1: case H_CH2: case H_CH3: case H_CH4: {
            char k[4];

            scope_set_active(&sc, id - H_CH1);
            snprintf(k, sizeof k, "CH%d", id - H_CH1 + 1);
            scope_key(&sc, k);
            break;
        }
        case H_MATH:    scope_key(&sc, "MATH"); break;
        case H_REF:     scope_key(&sc, "REF"); break;
        case H_LA:      scope_key(&sc, "LA"); break;
        case H_SOURCE:  scope_key(&sc, "SOUR"); break;
        case H_TMODE:   cycle_sweep(); break;
        case H_TMENU:   scope_key(&sc, "TMEN"); break;
        case H_FORCE:   scope_key(&sc, "TFOR"); break;
        case H_SHOT:    save_shot(); break;
        default: break;
    }
}

static int is_knob(int id)
{
    return id == H_MULTI || id == H_HPOS || id == H_HSCALE || id == H_VPOS ||
           id == H_VSCALE || id == H_TLEVEL;
}

/* turn a knob one detent */
static void turn(int id, int dir)
{
    switch (id) {
        case H_HSCALE: scope_knob(&sc, "HSC", dir); break;
        case H_HPOS:   scope_knob(&sc, "HPOS", dir); break;
        case H_TLEVEL: scope_knob(&sc, "TLEV", dir); break;
        case H_VSCALE: scope_vscale(&sc, dir); break;
        case H_VPOS:   scope_vpos(&sc, dir); break;
        case H_MULTI:  set_note("The intensity knob has no USB command"); break;
        default: return;
    }
    angle[id] += dir * 0.35;
    last_knob = id;
}

/* push a knob */
static void push(int id)
{
    switch (id) {
        case H_HPOS:   scope_cmd(&sc, ":TIM:MAIN:OFFS 0"); break;
        case H_TLEVEL: scope_cmd(&sc, ":TLHA"); break;
        case H_VPOS:   scope_vpos_zero(&sc); break;
        default: break;
    }
}

/* ---------------------------------------------------------------- drawing */

/* The panel is laid out in panel units, PANW by PANH, and drawn on an integer
   grid GRID times finer, so fractional positions survive. AMI's view scale and
   offset then fit the grid to the window, text and line widths included. */
#define GRID 10

static int X(double x) { return (int)lround(x * GRID); }
static int Y(double y) { return (int)lround(y * GRID); }
static int S(double d) { int v = (int)lround(d * GRID); return v < 1 ? 1 : v; }

static ami_long cv(int v) { return (ami_long)v * (LONG_MAX / 255); }
static void color(int r, int g, int b) { ami_fcolorg(stdout, cv(r), cv(g), cv(b)); }

static void addhit(double x1, double y1, double x2, double y2, int id)
{
    if (nhits >= (int)(sizeof hits / sizeof hits[0])) return;
    hits[nhits].x1 = X(x1); hits[nhits].y1 = Y(y1);
    hits[nhits].x2 = X(x2); hits[nhits].y2 = Y(y2);
    hits[nhits].id = id;
    nhits++;
}

static int hit(int x, int y)
{
    int i;

    for (i = nhits - 1; i >= 0; i--)
        if (x >= hits[i].x1 && x <= hits[i].x2 && y >= hits[i].y1 && y <= hits[i].y2)
            return hits[i].id;
    return H_NONE;
}

static void frect(double x1, double y1, double x2, double y2)
{
    ami_frect(stdout, X(x1), Y(y1), X(x2), Y(y2));
}

static void frrect(double x1, double y1, double x2, double y2, double r)
{
    ami_frrect(stdout, X(x1), Y(y1), X(x2), Y(y2), S(r), S(r));
}

static void rrect(double x1, double y1, double x2, double y2, double r, double w)
{
    ami_linewidth(stdout, S(w));
    ami_rrect(stdout, X(x1), Y(y1), X(x2), Y(y2), S(r), S(r));
}

static void fcircle(double cx, double cy, double r)
{
    ami_fellipse(stdout, X(cx - r), Y(cy - r), X(cx + r), Y(cy + r));
}

static void line(double x1, double y1, double x2, double y2, double w)
{
    ami_linewidth(stdout, S(w));
    ami_line(stdout, X(x1), Y(y1), X(x2), Y(y2));
}

static void ftri(double x1, double y1, double x2, double y2, double x3, double y3)
{
    ami_ftriangle(stdout, X(x1), Y(y1), X(x2), Y(y2), X(x3), Y(y3));
}

static void font(int face, double size, int bold)
{
    ami_font(stdout, face);
    ami_fontsiz(stdout, S(size));
    ami_bold(stdout, bold);
}

/* width of a string in panel units */
static double tw(const char *s) { return (double)ami_strsiz(stdout, s) / GRID; }

/* text with its top left at x,y; align -1 left, 0 centre, 1 right of x */
static void text(double x, double y, int align, const char *s)
{
    double w = tw(s);

    if (align == 0) x -= w / 2;
    else if (align > 0) x -= w;
    ami_cursorg(stdout, X(x), Y(y));
    ami_binvis(stdout);
    fputs(s, stdout);
}

/* text centred in a box */
static void ctext(double x1, double y1, double x2, double y2, const char *s)
{
    double h = (double)ami_chrsizy(stdout) / GRID;

    text((x1 + x2) / 2, (y1 + y2) / 2 - h / 2, 0, s);
}

/* a small label in the case's print colour */
static void label(double x, double y, int align, const char *s)
{
    font(AMI_FONT_SIGN, 9, TRUE);
    color(105, 106, 114);
    text(x, y, align, s);
}

/* a smaller label, for the sweep lamps */
static void slabel(double x, double y, int align, const char *s)
{
    font(AMI_FONT_SIGN, 7.5, TRUE);
    color(105, 106, 114);
    text(x, y, align, s);
}

/* a grouping outline on the case, as the moulded lines around a section */
static void group(double x1, double y1, double x2, double y2)
{
    color(212, 213, 217);
    frrect(x1, y1, x2, y2, 8);
    color(190, 191, 196);
    rrect(x1, y1, x2, y2, 8, 1);
}

/* one front panel key, in one of the looks; ch is the channel for KS_CH */
static void key(double x1, double y1, double x2, double y2, const char *lab, int id, int style, int ch)
{
    int    down = pressed == id;
    double dy = down ? 1.5 : 0;

    /* the ring in the channel colour, bright when the channel is on */
    if (style == KS_CH) {
        if (sc.chdisp[ch] && sc.connected) color(chcol[ch].r, chcol[ch].g, chcol[ch].b);
        else color(chcol[ch].r / 2 + 100, chcol[ch].g / 2 + 100, chcol[ch].b / 2 + 100);
        frrect(x1 - 3, y1 - 3, x2 + 3, y2 + 5.5, 7);
    }
    /* shadow, body */
    color(150, 151, 156);
    frrect(x1, y1 + 2.5, x2, y2 + 2.5, 5);
    switch (style) {
        case KS_SOFT:  color(down ? 120 : 140, down ? 150 : 170, down ? 180 : 200); break;
        case KS_BLUE:  color(down ? 160 : 180, down ? 175 : 195, down ? 195 : 212); break;
        case KS_GREEN: color(down ? 120 : 150, down ? 200 : 225, down ? 100 : 120); break;
        default:       color(down ? 215 : 240, down ? 215 : 240, down ? 218 : 243); break;
    }
    frrect(x1, y1 + dy, x2, y2 + dy, 5);
    if (style == KS_CH && sc.active == ch) {
        /* the channel the vertical knobs act on: a dot in the corner */
        color(60, 60, 66);
        fcircle(x2 - 5, y1 + dy + 5, 2);
    }
    if (lab && *lab) {
        if (style == KS_CH && sc.chdisp[ch] && sc.connected) color(chcol[ch].r * 3 / 4, chcol[ch].g * 3 / 4, chcol[ch].b * 3 / 4);
        else color(40, 40, 46);
        font(AMI_FONT_SIGN, (x2 - x1) < 44 ? 9.5 : 10.5, TRUE);
        ctext(x1, y1 + dy, x2, y2 + dy, lab);
    }
    addhit(x1, y1, x2, y2 + 3, id);
}

/* a soft key with a page arrow */
static void arrowkey(double x1, double y1, double x2, double y2, int up, int id)
{
    double cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;

    key(x1, y1, x2, y2, "", id, KS_SOFT, 0);
    color(240, 244, 250);
    if (up) ftri(cx - 6, cy + 3.5, cx + 6, cy + 3.5, cx, cy - 3.5);
    else ftri(cx - 6, cy - 3.5, cx + 6, cy - 3.5, cx, cy + 3.5);
}

/* a knob: big ones have the grip ring, all have a pointer */
static void knob(double cx, double cy, double r, int id)
{
    int    i;
    double a;

    knobpos[id].cx = cx; knobpos[id].cy = cy; knobpos[id].r = r;
    color(150, 151, 156);
    fcircle(cx, cy + 2.5, r);
    color(196, 197, 202);
    fcircle(cx, cy, r);
    if (r >= 20) {
        color(170, 171, 177);
        for (i = 0; i < 30; i++) {
            a = i * M_PI / 15;
            line(cx + cos(a) * (r - 3), cy + sin(a) * (r - 3), cx + cos(a) * r, cy + sin(a) * r, 1.5);
        }
        color(214, 215, 220);
        fcircle(cx, cy, r - 6);
        color(228, 229, 233);
        fcircle(cx - r / 8, cy - r / 8, r - 13);
    } else {
        color(224, 225, 229);
        fcircle(cx, cy, r - 4);
    }
    color(90, 90, 98);
    fcircle(cx + cos(angle[id]) * (r * 0.68), cy + sin(angle[id]) * (r * 0.68), r >= 20 ? 3 : 2);
    addhit(cx - r, cy - r, cx + r, cy + r, id);
}

/* the BNC input of a channel, with its coloured tag */
static void bnc(double cx, double cy, int ch)
{
    char l[16];

    if (sc.active == ch) color(chcol[ch].r, chcol[ch].g, chcol[ch].b);
    else color(190, 191, 196);
    frrect(cx - 46, cy - 42, cx + 46, cy + 42, 10);
    color(206, 207, 212);
    frrect(cx - 43.5, cy - 39.5, cx + 43.5, cy + 39.5, 9);
    color(120, 121, 126);
    fcircle(cx, cy + 1.5, 25);
    color(200, 201, 206);
    fcircle(cx, cy, 25);
    color(150, 151, 156);
    fcircle(cx, cy, 18);
    color(225, 226, 230);
    fcircle(cx, cy, 13);
    color(60, 60, 64);
    fcircle(cx, cy, 8);
    color(200, 200, 204);
    fcircle(cx, cy, 2.5);
    /* the tag at the top left corner */
    color(chcol[ch].r, chcol[ch].g, chcol[ch].b);
    frrect(cx - 46, cy - 42, cx - 34, cy - 8, 3);
    snprintf(l, sizeof l, "%d", ch + 1);
    font(AMI_FONT_SIGN, 8, TRUE);
    color(255, 255, 255);
    text(cx - 40, cy - 28, 0, l);
    addhit(cx - 46, cy - 42, cx + 46, cy + 42, H_BNC1 + ch);
}

/* the scope's screen: its own picture when there is one */
static void draw_screen(void)
{
    color(30, 30, 34);
    frrect(72, 58, 616, 394, 6);
    color(50, 50, 55);
    rrect(72, 58, 616, 394, 6, 1);
    if (sc.frame_seq != shown_seq && sc.frame[0]) {
        ami_loadpict(stdout, PICT, sc.frame);
        shown_seq = sc.frame_seq;
        have_pict = 1;
    }
    if (have_pict) {
        /* in pixels: a picture scaled through the view would be huge */
        ami_long px1 = (ami_long)(voffx + X(GLX1) * vscale), py1 = (ami_long)(voffy + Y(GLY1) * vscale);
        ami_long pw = (ami_long)(voffx + X(GLX2) * vscale) - px1, ph = (ami_long)(voffy + Y(GLY2) * vscale) - py1;

        /* within rounding of the screen's own size, draw it 1:1 */
        if (labs(pw - SCRW) <= 2 && labs(ph - SCRH) <= 2) { pw = SCRW; ph = SCRH; }
        ami_viewoffg(stdout, 0, 0);
        ami_viewscale(stdout, 1, 1);
        ami_picture(stdout, PICT, px1, py1, px1 + pw - 1, py1 + ph - 1);
        ami_viewoffg(stdout, (ami_long)voffx, (ami_long)voffy);
        ami_viewscale(stdout, (float)vscale, (float)vscale);
    } else {
        color(4, 8, 16);
        frect(GLX1, GLY1, GLX2, GLY2);
        font(AMI_FONT_SIGN, 14, FALSE);
        color(120, 140, 170);
        ctext(GLX1, GLY1, GLX2, GLY2, sc.status);
    }
    if (!sc.connected && have_pict) {
        /* the last picture stays, with a banner */
        color(120, 30, 30);
        frect(GLX1, GLY2 - 24, GLX2, GLY2);
        font(AMI_FONT_SIGN, 12, TRUE);
        color(255, 255, 255);
        ctext(GLX1, GLY2 - 24, GLX2, GLY2, sc.status);
    }
    addhit(GLX1, GLY1, GLX2, GLY2, H_SCREEN);
}

/* the strip under the case: the link, a few settings, messages */
static void draw_status(void)
{
    char   b1[48], b2[48], buf[256];
    double y1 = CASEH + 6, y2 = PANH - 4;

    color(14, 14, 16);
    frrect(2, y1, PANW - 2, y2, 6);
    font(AMI_FONT_SIGN, 11, TRUE);
    if (sc.sim) { color(230, 180, 40); snprintf(buf, sizeof buf, "SIMULATED"); }
    else if (sc.connected) { color(40, 200, 90); snprintf(buf, sizeof buf, "%s  %s", sc.model, sc.serial); }
    else { color(235, 120, 80); snprintf(buf, sizeof buf, "%s", sc.status); }
    text(14, y1 + 9, -1, buf);
    if (sc.have_settings && sc.connected) {
        font(AMI_FONT_TERM, 11, FALSE);
        color(200, 205, 215);
        fmt_eng(sc.tscale, "s", b1, sizeof b1);
        fmt_eng(sc.trlev, "V", b2, sizeof b2);
        snprintf(buf, sizeof buf, "H %s/div   T %s %s %s %s   CH%d active", b1, sc.trsrc, b2, sc.trswe,
                 sc.trstat, sc.active + 1);
        text(300, y1 + 9, -1, buf);
    }
    if (note_until) {
        font(AMI_FONT_SIGN, 11, FALSE);
        color(250, 215, 50);
        text(PANW - 90, y1 + 9, 1, note);
    } else if (sc.connected && sc.frames > 0) {
        font(AMI_FONT_SIGN, 10, FALSE);
        color(130, 140, 160);
        snprintf(buf, sizeof buf, "%.1f fps", fps);
        text(PANW - 90, y1 + 10, 1, buf);
    }
    key(PANW - 80, y1 + 6, PANW - 10, y2 - 6, "SAVE PNG", H_SHOT, KS_BLUE, 0);
}

static void redraw(void)
{
    static int buf = 1;
    int    w, h, k, running;
    double scale;
    static const char *fkl[2][3] = { { "MEASURE", "ACQUIRE", "STORAGE" }, { "CURSOR", "DISPLAY", "UTILITY" } };
    static const int   fki[2][3] = { { H_MEAS, H_ACQ, H_STOR }, { H_CURS, H_DISP, H_UTIL } };
    static const double sky[6] = { 100, 141, 183, 220, 262, 299 }; /* soft key tops */
    int r, c;

    /* draw off screen, then show it: no flicker at the frame rate */
    buf = 3 - buf;
    ami_select(stdout, buf, 3 - buf);
    ami_auto(stdout, FALSE);
    ami_curvis(stdout, FALSE);
    ami_binvis(stdout);
    ami_viewoffg(stdout, 0, 0);
    ami_viewscale(stdout, 1, 1);
    w = (int)ami_maxxg(stdout);
    h = (int)ami_maxyg(stdout);
    ami_bcolorg(stdout, cv(30), cv(31), cv(34));
    ami_fcolorg(stdout, cv(30), cv(31), cv(34));
    ami_frect(stdout, 1, 1, w, h);
    scale = fmin((double)w / (PANW * GRID), (double)h / (PANH * GRID));
    vscale = scale;
    voffx = floor((w - PANW * GRID * scale) / 2);
    voffy = floor((h - PANH * GRID * scale) / 2);
    ami_viewoffg(stdout, (ami_long)voffx, (ami_long)voffy);
    ami_viewscale(stdout, (float)scale, (float)scale);
    nhits = 0;

    /* the case: light grey, a darker lip along the bottom */
    color(160, 161, 166);
    frrect(2, 2, PANW - 2, CASEH - 2, 16);
    color(226, 227, 230);
    frrect(6, 6, PANW - 6, CASEH - 8, 14);
    color(205, 206, 210);
    frrect(6, CASEH - 30, PANW - 6, CASEH - 8, 6);
    color(226, 227, 230);
    frect(6, CASEH - 30, PANW - 6, CASEH - 24);
    /* branding */
    font(AMI_FONT_SIGN, 15, TRUE);
    color(230, 170, 20);
    text(108, 26, -1, "RIGOL");
    font(AMI_FONT_SIGN, 13, TRUE);
    color(60, 60, 66);
    text(176, 27, -1, "MSO1104Z");
    label(296, 31, -1, "OSCILLOSCOPE");
    font(AMI_FONT_SIGN, 10, TRUE);
    color(40, 100, 200);
    text(372, 29, -1, "UltraVision");
    label(440, 26, -1, "4 CHANNEL");
    label(440, 36, -1, "100MHz  1GSa/s");

    draw_screen();

    /* soft keys left of the screen: menu, six keys, two small arrows */
    key(24, 56, 60, 84, "MENU", H_LMENU, KS_SOFT, 0);
    for (k = 0; k < 6; k++) key(24, sky[k], 60, sky[k] + 26, "", H_L1 + k, KS_SOFT, 0);
    arrowkey(24, 346, 60, 362, 1, H_LUP);
    arrowkey(24, 368, 60, 384, 0, H_LDOWN);
    /* and right of it: arrow, six keys, arrow */
    arrowkey(628, 60, 668, 86, 1, H_RUP);
    for (k = 0; k < 6; k++) key(628, sky[k], 668, sky[k] + 26, "", H_F1 + k, KS_SOFT, 0);
    arrowkey(628, 340, 668, 366, 0, H_RDOWN);

    /* top row: the run keys */
    running = sc.connected && strcmp(sc.trstat, "STOP") != 0;
    key(732, 40, 786, 74, "CLEAR", H_CLEAR, KS_LIGHT, 0);
    key(798, 40, 852, 74, "AUTO", H_AUTO, KS_LIGHT, 0);
    if (sc.connected && !running) {
        color(230, 60, 60);
        frrect(861, 37, 927, 79.5, 7);
    }
    key(864, 40, 924, 74, "RUN/STOP", H_RUNSTOP, sc.connected ? (running ? KS_GREEN : KS_LIGHT) : KS_BLUE, 0);
    key(936, 40, 992, 74, "SINGLE", H_SINGLE, KS_BLUE, 0);

    /* the intensity knob, the menu keys, help and print */
    group(690, 84, 1010, 166);
    knob(738, 128, 22, H_MULTI);
    label(714, 92, -1, "INTENSITY");
    group(786, 90, 942, 162);
    for (r = 0; r < 2; r++)
        for (c = 0; c < 3; c++)
            key(792 + c * 50, 98 + r * 32, 836 + c * 50, 122 + r * 32, fkl[r][c], fki[r][c], KS_LIGHT, 0);
    label(944, 110, -1, "M");
    label(944, 118, -1, "E");
    label(944, 126, -1, "N");
    label(944, 134, -1, "U");
    key(960, 96, 1004, 124, "HELP", H_HELP, KS_GREEN, 0);
    key(960, 130, 1004, 158, "", H_PRINT, KS_GREEN, 0);
    color(60, 60, 66);
    frrect(975, 138, 989, 150, 2);
    color(230, 230, 234);
    frect(978, 134, 986, 140);
    frect(978, 148, 986, 154);

    /* vertical */
    group(690, 174, 842, 392);
    for (c = 0; c < SCOPE_NCH; c++) {
        char l[16];

        snprintf(l, sizeof l, "CH%d", c + 1);
        key(700, 180 + c * 36, 744, 206 + c * 36, l, H_CH1 + c, KS_CH, c);
    }
    key(756, 252, 796, 276, "MATH", H_MATH, KS_BLUE, 0);
    key(804, 252, 840, 276, "REF", H_REF, KS_BLUE, 0);
    key(700, 324, 744, 348, "LA", H_LA, KS_BLUE, 0);
    key(700, 356, 744, 378, "SOURCE", H_SOURCE, KS_BLUE, 0);
    knob(786, 206, 20, H_VPOS);
    label(786, 230, 0, "POSITION");
    knob(786, 328, 34, H_VSCALE);
    label(786, 366, 0, "SCALE");
    label(766, 380, 0, "VERTICAL");

    /* horizontal */
    group(852, 174, 950, 392);
    label(901, 178, 0, "HORIZONTAL");
    knob(901, 212, 20, H_HPOS);
    label(901, 238, 0, "POSITION");
    key(878, 252, 924, 276, "MENU", H_HMENU, KS_LIGHT, 0);
    knob(901, 328, 34, H_HSCALE);
    label(901, 366, 0, "SCALE");

    /* trigger */
    group(958, 174, 1036, 392);
    slabel(976, 188, 0, "AUTO");
    slabel(1000, 188, 0, "NORM");
    slabel(1024, 188, 0, "SING");
    for (c = 0; c < 3; c++) {
        static const char *sw[3] = { "AUTO", "NORM", "SING" };
        int lit = sc.connected && !strcmp(sc.trswe, sw[c]);

        color(lit ? 255 : 150, lit ? 150 : 151, lit ? 30 : 156);
        fcircle(976 + c * 24, 183, 2.5);
    }
    key(972, 198, 1022, 222, "MODE", H_TMODE, KS_LIGHT, 0);
    label(997, 228, 0, "LEVEL");
    knob(997, 270, 20, H_TLEVEL);
    key(972, 300, 1022, 324, "MENU", H_TMENU, KS_LIGHT, 0);
    key(972, 334, 1022, 358, "FORCE", H_FORCE, KS_LIGHT, 0);
    label(997, 380, 0, "TRIGGER");

    /* along the bottom: power, USB, the logic pod, the inputs, probe comp */
    color(170, 171, 176);
    frrect(44, 426, 108, 480, 6);
    color(236, 237, 240);
    frrect(48, 430, 104, 476, 5);
    color(120, 200, 60);
    fcircle(76, 453, 7);
    color(236, 237, 240);
    fcircle(76, 453, 4);
    frect(75, 444, 77, 452);
    addhit(44, 426, 108, 480, H_POWER);
    color(170, 171, 176);
    frrect(118, 424, 150, 484, 4);
    color(236, 237, 240);
    frrect(122, 428, 146, 480, 3);
    color(60, 60, 66);
    frect(126, 436, 142, 472);
    color(200, 200, 205);
    frect(129, 440, 139, 468);
    label(134, 488, 0, "USB");
    color(150, 151, 156);
    frrect(184, 440, 360, 478, 4);
    color(60, 60, 66);
    frrect(188, 444, 356, 474, 3);
    color(120, 120, 126);
    for (c = 0; c < 24; c++) { frect(194 + c * 6.6, 450, 197 + c * 6.6, 456); frect(194 + c * 6.6, 462, 197 + c * 6.6, 468); }
    label(380, 448, -1, "LOGIC");
    label(380, 456, -1, "D0-D15");
    /* the input bay */
    color(190, 191, 196);
    frrect(524, 396, 982, 498, 10);
    color(214, 215, 219);
    frrect(528, 400, 978, 494, 9);
    label(760, 402, 0, "ALL INPUTS 1M OHM / 13pF   300V RMS   CAT I");
    for (c = 0; c < SCOPE_NCH; c++) bnc(584 + c * 116, 453, c);
    /* probe compensation terminals at the right edge */
    color(150, 151, 156);
    frrect(1012, 426, 1036, 478, 3);
    color(60, 60, 66);
    frect(1018, 434, 1030, 446);
    frect(1018, 458, 1030, 470);
    label(1024, 484, 0, "3V");

    draw_status();
    ami_select(stdout, buf, buf);
}

#ifdef SHOT
static void run_script(void)
{
    static const struct { const char *n; int id; } tab[] = {
        { "F1", H_F1 }, { "F2", H_F2 }, { "F3", H_F3 }, { "F4", H_F4 }, { "F5", H_F5 }, { "F6", H_F6 },
        { "CLEAR", H_CLEAR }, { "AUTO", H_AUTO }, { "RUNSTOP", H_RUNSTOP }, { "SINGLE", H_SINGLE },
        { "HELP", H_HELP }, { "MEAS", H_MEAS }, { "ACQ", H_ACQ }, { "STOR", H_STOR }, { "CURS", H_CURS },
        { "DISP", H_DISP }, { "UTIL", H_UTIL }, { "HMENU", H_HMENU }, { "CH1", H_CH1 }, { "CH2", H_CH2 },
        { "CH3", H_CH3 }, { "CH4", H_CH4 }, { "MATH", H_MATH }, { "REF", H_REF }, { "LA", H_LA },
        { "SOURCE", H_SOURCE }, { "TMODE", H_TMODE }, { "TMENU", H_TMENU }, { "FORCE", H_FORCE },
        { "SHOT", H_SHOT }, { "HSC", H_HSCALE }, { "HPOS", H_HPOS }, { "TLEV", H_TLEVEL },
        { "VSC", H_VSCALE }, { "VPOS", H_VPOS }
    };
    char  buf[256], *tok, *save;
    int   i, dir;

    snprintf(buf, sizeof buf, "%s", script);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        size_t n = strlen(tok);

        dir = 0;
        if (n > 1 && (tok[n - 1] == '+' || tok[n - 1] == '-')) { dir = tok[n - 1] == '+' ? 1 : -1; tok[n - 1] = 0; }
        for (i = 0; i < (int)(sizeof tab / sizeof tab[0]); i++)
            if (!strcmp(tab[i].n, tok)) {
                if (dir) turn(tab[i].id, dir); else click(tab[i].id);
                fprintf(stderr, "script: %s%s\n", tok, dir > 0 ? "+" : dir < 0 ? "-" : "");
            }
    }
}
#endif

/* ---------------------------------------------------------------- input */

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [--sim]\n", prog);
    exit(1);
}

int main(int argc, char *argv[])
{
    ami_evtrec er;
    ami_long   wx, wy;
    int sim = 0, i, id, dirty;
    char title[160];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim") || !strcmp(argv[i], "-s")) sim = 1;
        else usage(argv[0]);
    }
    for (i = 0; i < H_LAST; i++) angle[i] = -2.2;
#ifdef SHOT
    shot_file = getenv("MSO1104_SHOT");
    if (shot_file) { screen_capture_name(shot_file); shot_at = now_ms() + 4000; }
    script = getenv("MSO1104_SCRIPT");
    if (script) script_at = now_ms() + 1000;
#endif
    scope_init(&sc, sim);
    ami_autohold(FALSE);
    ami_auto(stdout, FALSE);
    ami_curvis(stdout, FALSE);
    ami_binvis(stdout);
    snprintf(title, sizeof title, "MSO1104%s", sim ? " (simulated)" : "");
    ami_title(stdout, title);
    ami_autobuf(stdout, FALSE);
    ami_winclientg(stdout, WINW, WINH, &wx, &wy,
                   BIT(ami_wmframe) | BIT(ami_wmsize) | BIT(ami_wmsysbar));
    ami_setsizg(stdout, wx, wy);
    ami_timer(stdout, TICK_ID, TICK, TRUE);
    redraw();
    do {
        ami_event(stdin, &er);
        dirty = 1;
        switch (er.etype) {
            case ami_ettim:
                dirty = scope_poll(&sc);
                if (dirty && sc.frame_seq != shown_seq) {
                    long t = now_ms();

                    if (frame_at) fps = 0.7 * fps + 0.3 * (1000.0 / (t - frame_at > 0 ? t - frame_at : 1));
                    frame_at = t;
                }
                if (note_until && now_ms() >= note_until) { note_until = 0; dirty = 1; }
#ifdef SHOT
                if (script && now_ms() >= script_at) { run_script(); script = NULL; dirty = 1; }
                if (shot_file && now_ms() >= shot_at) {
                    screen_capture();
                    shot_at = now_ms() + 3000;
                    if (++shots >= 2) er.etype = ami_etterm;
                }
#endif
                break;
            case ami_etmoumovg:
                mx = (int)ami_scalex(stdout, er.moupxg);
                my = (int)ami_scaley(stdout, er.moupyg);
                dirty = 0;
                if (dragging != H_NONE) {
                    int step = S(DRAGSTEP);

                    while (drag_y - my >= step) { turn(dragging, 1); drag_y -= step; drag_moved = dirty = 1; }
                    while (my - drag_y >= step) { turn(dragging, -1); drag_y += step; drag_moved = dirty = 1; }
                }
                break;
            case ami_etmouba:
                id = hit(mx, my);
                if (er.amoubn == 4 || er.amoubn == 5) {
                    turn(is_knob(id) ? id : last_knob, er.amoubn == 4 ? 1 : -1);
                    break;
                }
                if (er.amoubn == 3) {
                    if (is_knob(id)) push(id); else dirty = 0;
                    break;
                }
                if (er.amoubn != 1) { dirty = 0; break; }
                if (is_knob(id)) {
                    dragging = id;
                    drag_y = my;
                    drag_moved = 0;
                    dirty = 0;
                } else pressed = id;
                break;
            case ami_etmoubd:
                if (er.dmoubn == 1 && dragging != H_NONE) {
                    /* a click without a drag steps by the side clicked */
                    if (!drag_moved) turn(dragging, mx < X(knobpos[dragging].cx) ? -1 : 1);
                    dragging = H_NONE;
                } else if (er.dmoubn == 1 && pressed != H_NONE) {
                    if (hit(mx, my) == pressed) click(pressed);
                    pressed = H_NONE;
                } else dirty = 0;
                break;
            case ami_etchar:
                switch (er.echar) {
                    case '1': case '2': case '3': case '4': click(H_CH1 + er.echar - '1'); break;
                    case 'r': case 'R': click(H_RUNSTOP); break;
                    case 's': case 'S': click(H_SINGLE); break;
                    case 'a': case 'A': click(H_AUTO); break;
                    case 'c': case 'C': click(H_CLEAR); break;
                    case 'h': case 'H': click(H_HELP); break;
                    case 'm': case 'M': click(H_MEAS); break;
                    case 'q': case 'Q': click(H_ACQ); break;
                    case 'd': case 'D': click(H_DISP); break;
                    case 'u': case 'U': click(H_UTIL); break;
                    case 't': case 'T': click(H_TMENU); break;
                    case 'x': case 'X': save_shot(); break;
                    case '+': case '=': turn(H_TLEVEL, 1); break;
                    case '-': case '_': turn(H_TLEVEL, -1); break;
                    default: dirty = 0; break;
                }
                break;
            case ami_etfun:
                if (er.fkey >= 1 && er.fkey <= 6) click(H_F1 + (int)er.fkey - 1);
                else dirty = 0;
                break;
            case ami_etleft:  turn(H_HPOS, -1); break;
            case ami_etright: turn(H_HPOS, 1); break;
            case ami_etup:    turn(H_HSCALE, 1); break;
            case ami_etdown:  turn(H_HSCALE, -1); break;
            case ami_etpagu:  turn(H_VSCALE, 1); break;
            case ami_etpagd:  turn(H_VSCALE, -1); break;
            case ami_ethome:  turn(H_VPOS, 1); break;
            case ami_etend:   turn(H_VPOS, -1); break;
            case ami_etresize:
                ami_sizbufg(stdout, er.rszxg, er.rszyg);
                break;
            case ami_etredraw:
                break;
            case ami_etterm:
                dirty = 0;
                break;
            default:
                dirty = 0;
                break;
        }
        if (dirty && er.etype != ami_etterm) redraw();
    } while (er.etype != ami_etterm);
    scope_shutdown(&sc);
    return 0;
}
