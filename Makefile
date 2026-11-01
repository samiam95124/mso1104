#
# MSO1104 front panel, built on the AMI tool kit (graphical model, static).
#
# AMIDIR is where amitk lives; ". setpath" in amitk sets it, otherwise the
# default below is used. The scope link needs libusb-1.0 and libpng.
#
# Windows is the MSYS2 shell (uname says MINGW64_NT or MSYS_NT) or a plain
# command shell, where OS is set and uname may not exist.
UNAME := $(shell uname 2>/dev/null)
ifeq ($(OS),Windows_NT)
    OSTYPE := Windows_NT
else ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME)))
    OSTYPE := Windows_NT
else
    OSTYPE := $(UNAME)
endif

ifeq ($(OSTYPE),Windows_NT)
    # amitk's setpath.bat does not set AMIDIR, and HOME is MSYS's own, so
    # the Windows default is a sister directory of this one
    AMIDIR ?= ../amitk
else
    AMIDIR ?= $(HOME)/projects/amitk
endif
AMILIB = $(AMIDIR)/lib/libami_graph.a

ifeq ($(OSTYPE),Windows_NT)
    # MinGW from MSYS2: libusb and libpng from pacman (see README). The
    # amitk Windows build is static and runs its stdio under coined names
    # (STDIO_BYPASS), so the program is compiled the same way.
    INCS = $(shell pkg-config --cflags libusb-1.0 libpng) -DSTDIO_BYPASS
    LIBS = $(shell pkg-config --libs --static libusb-1.0 libpng) -lz \
           -lwinmm -lgdi32 -lcomdlg32 -lssl -lcrypto -lws2_32 -liphlpapi -lcrypt32
    KEEP =
    AMILINK = -static -Wl,--whole-archive $(AMILIB) -Wl,--no-whole-archive
    CAPTURE = $(AMIDIR)/windows/screen_capture.o
    EXE = .exe
else ifeq ($(OSTYPE),Darwin)
    BREW := /opt/homebrew/bin/brew
    SSL_PREFIX := $(shell $(BREW) --prefix openssl@3)
    USB_PREFIX := $(shell $(BREW) --prefix libusb)
    PNG_PREFIX := $(shell $(BREW) --prefix libpng)
    INCS = -I$(USB_PREFIX)/include/libusb-1.0 -I$(PNG_PREFIX)/include
    LIBS = $(SSL_PREFIX)/lib/libssl.dylib $(SSL_PREFIX)/lib/libcrypto.dylib \
           $(USB_PREFIX)/lib/libusb-1.0.dylib $(PNG_PREFIX)/lib/libpng.dylib \
           -framework CoreFoundation -framework CoreGraphics \
           -framework ImageIO -framework CoreMIDI -framework AudioToolbox \
           -framework Cocoa -framework CoreText -framework QuartzCore \
           -framework IOKit
    KEEP = -Wl,-u,_ami_maxx
    AMILINK = -Wl,-force_load,$(AMILIB) -Wl,-e,_pa_entry
    CAPTURE = $(AMIDIR)/macosx/screen_capture.o
else
    INCS = $(shell pkg-config --cflags libusb-1.0 libpng)
    LIBS = $(shell pkg-config --libs libusb-1.0 libpng) \
           -lasound -lfluidsynth -lssl -lcrypto -lwayland-client -lwayland-cursor \
           -lxkbcommon -lfreetype -lfontconfig -lm -lpthread
    KEEP = -Wl,-u,ami_maxx
    AMILINK = $(AMIDIR)/stub/keeper.o $(AMILIB)
    CAPTURE = $(AMIDIR)/linux/wayland/screen_capture.o
endif

CC = gcc
CFLAGS = -g3 -Wall -I$(AMIDIR)/include -I$(AMIDIR)/libc $(INCS)

SRCS = mso1104.c scope.c tiff.c

all: mso1104

mso1104: $(SRCS) scope.h tiff.h $(AMILIB)
	$(CC) $(CFLAGS) $(SRCS) $(KEEP) $(AMILINK) $(LIBS) -lm -o $@$(EXE)

# test build: takes pictures of its own window, see MSO1104_SHOT in mso1104.c
shot: $(SRCS) scope.h tiff.h $(AMILIB)
	$(CC) $(CFLAGS) -DSHOT $(SRCS) $(CAPTURE) $(KEEP) $(AMILINK) $(LIBS) -lm -o mso1104_shot$(EXE)

sim: mso1104
	./mso1104$(EXE) --sim

clean:
	rm -rf mso1104 mso1104_shot mso1104.exe mso1104_shot.exe mso1104.dSYM

.PHONY: all sim shot clean
