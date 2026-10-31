# MSO1104 front panel

A remote control for the Rigol MSO1104Z and DS1104Z oscilloscopes, and the
rest of the DS1000Z / MSO1000Z family. It drives the scope over its USB port
and is written in C using the AMI tool kit. The window is drawn to look like
the front of the scope: the screen at the left shows the instrument's own
display, fetched about once a second, and the keys and knobs around it work
the real ones. The scope's menus, cursors, measurements and so on are all
driven through its own screen, so the panel is a remote control rather than
a re-implementation.

**Works with the DS1104Z too.** The MSO1104Z and the DS1104Z Plus are the
same instrument; the MSO is the DS with the logic analyser option fitted,
and both report themselves on the bus as `DS1104Z Plus`, which is what the
status strip shows. The badge on the panel says MSO1104Z as the real one
does. The other models in the family (DS1054Z, DS1074Z, DS1104Z, the Plus
and MSO versions) share the front panel layout, the screen size and the
command set, so they work the same; only the badge is different.

![The panel](panel.png)

![The scope, with the panel behind it](scope_and_panel.jpg)

## Build and run

    make                     # needs ~/projects/amitk built (or set AMIDIR),
                             # libusb-1.0 and libpng
    ./mso1104                # finds the scope on USB
    ./mso1104 --sim          # no hardware: a simulated scope
    make sim                 # same as ./mso1104 --sim

On Linux the scope is USB device `1ab1:04ce`. The program talks to it
through libusb and detaches the kernel's usbtmc driver itself, so the only
setup is permission to open the device: a udev rule such as

    SUBSYSTEM=="usb", ATTRS{idVendor}=="1ab1", MODE="0660", GROUP="plugdev"

in `/etc/udev/rules.d/99-usbtmc.rules`, with you in `plugdev`. On the Mac
libusb and libpng come from Homebrew (`brew install libusb libpng`) and no
driver work is needed; the same `make` builds there. The program looks for
the scope every 2 s, so it can be plugged in after starting, and it
reconnects after a cable pull.

## Using it

| Control | Action |
|---|---|
| Soft keys right of the screen | The six menu keys. The bottom one is also the back key in a submenu |
| CLEAR, AUTO, RUN/STOP, SINGLE, Help | As on the scope. RUN/STOP is lit green running, ringed red stopped |
| Print key | Saves the screen picture, as SAVE PNG does |
| MEASURE, ACQUIRE, STORAGE, CURSOR, DISPLAY, UTILITY | Open those menus |
| HORIZONTAL knobs and MENU | Position and scale knobs, horizontal menu |
| CH1 to CH4, MATH, REF, LA, Source | Channel keys ringed bright in the channel colour when the channel is on, with a dot on the one the vertical knobs act on. A channel key also makes that channel the active one |
| VERTICAL knobs | Position and scale of the active channel |
| TRIGGER MODE, MENU, LEVEL, FORCE | MODE cycles Auto / Normal / Single sweep, shown by the lamps above it; the rest as on the scope |
| BNC of a channel | Choose it as the active channel without pressing its key |
| SAVE PNG | Save the current screen picture to `~/Desktop/MSO1104_<date>_<time>.png` |

Knobs: drag up or down, use the mouse wheel over the knob, or click its
left or right half for one step. The wheel anywhere else turns the knob
last used. A right click pushes a knob: horizontal position to zero,
trigger level to half, vertical position to zero.

Keyboard shortcuts:

- `1` to `4` channel keys, `r` run/stop, `s` single, `a` auto, `c` clear,
  `h` help, `m` measure, `q` acquire, `d` display, `u` utility, `t`
  trigger menu, `F1` to `F6` the soft keys.
- Left/Right horizontal position, Up/Down horizontal scale, `+`/`-`
  trigger level, PgUp/PgDn vertical scale, Home/End vertical position.
- `x` saves the screen picture.

The window opens at 1616 by 924 so that the glass is the scope's own 800 by
480 pixels and the picture is drawn 1:1. The scope's graticule is one pixel
dotted lines in a dim grey, and it fades out if the window is made smaller
and the picture has to be shrunk; it is fine at any larger size.

The strip under the case shows the link, the timebase, the trigger and the
active channel, re-read every 2 s so changes made on the scope itself also
show up, the picture rate, and brief messages.

## What does not work

- The intensity (multi-function) knob, the soft keys left of the screen
  with their MENU key, and the page arrows have no USB command on this
  firmware. Menu items that need the knob (brightness, a measurement list)
  have to be turned on the scope.
- The power key, naturally.
- The vertical knobs are not the scope's own knobs but set the channel
  scale (1-2-5 steps) and offset (a fifth of a division per step) directly,
  so a menu that the real knobs would drive sees nothing.
- `AUTO` is sent as the `:AUToscale` command, since the key itself is not
  reachable; the effect is the same.

## Protocol

USBTMC over libusb, bulk transfers only, SCPI commands. Besides the
documented command set the scope accepts these, found by trying:

| Command | Meaning |
|---|---|
| `:SYST:KEY:PRES <key>` | Press a key. Keys: `CH1`..`CH4` `MATH` `REF` `LA` `MEAS` `ACQ` `STOR` `CURS` `DISP` `UTIL` `HELP` `SOUR` `RSTOP` `SING` `TFOR` `CLE` `HMEN` `TMEN` `F1`..`F6` |
| `:SYST:KEY:INCR <knob>,1` | Turn a knob one detent clockwise. Knobs: `HSC` `HPOS` `TLEV` |
| `:SYST:KEY:DECR <knob>,1` | One detent anticlockwise |
| `:DISP:DATA? ON,OFF,PNG` | The screen as a PNG in an IEEE 488.2 block |

Two things about the instrument's USB shape the link code in `scope.c`:

- It sends 64 byte bulk packets although it enumerates at high speed, so
  the host sees every packet as a short packet. Replies are gathered piece
  by piece to the declared size, and the zero length packet that follows a
  transfer of a multiple of 64 bytes is consumed. The kernel usbtmc driver
  cannot cope with this for anything longer than 52 bytes, which is why the
  program does not use it.
- A VISA style open (remote enable, capabilities request) hangs the
  scope's USB stack until the cable is pulled. Only bulk transfers are
  used, and a half finished transfer is cleared with a USB port reset.

The screen picture (35 to 50 kB of PNG) takes about 1.2 s to arrive. It is
decoded with libpng and written as a 24 bit BMP, the picture format the AMI
graphics module loads, into two alternating files under `$TMPDIR`.

## Files

- `mso1104.c`: the front panel (drawing, input).
- `scope.c`, `scope.h`: the USB link, its thread and command queue, the
  picture conversion, and the simulator.
- `make shot` builds `mso1104_shot`, a test version that takes pictures of
  its own window (`MSO1104_SHOT=<file>`) and can run scripted key presses
  and knob turns (`MSO1104_SCRIPT=CH3,HSC-,TLEV+`).
- `Rigol_MSO1104Z-S.jpg`: the photograph the panel is laid out from.
- `panel.png`, `scope_and_panel.jpg`: the panel, and the scope itself with
  the panel on the monitor behind it.

## Caveats

- The panel is laid out from a photograph of an MSO1104Z-S, so the
  proportions are right but the key shapes and lettering are simplified.
- The knob step for the vertical offset and the 1-2-5 scale sequence are
  the program's, and ignore the scope's fine adjustment mode.
