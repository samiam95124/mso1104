#
# MSO1104 front panel, built on the AMI tool kit (graphical model, static).
#
# AMIDIR is where amitk lives; ". setpath" in amitk sets it, otherwise the
# default below is used. The scope link needs libusb-1.0 and libpng.
#
AMIDIR ?= $(HOME)/projects/amitk

OSTYPE := $(shell uname)
AMILIB = $(AMIDIR)/lib/libami_graph.a

ifeq ($(OSTYPE),Darwin)
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

SRCS = mso1104.c scope.c

all: mso1104

mso1104: $(SRCS) scope.h $(AMILIB)
	$(CC) $(CFLAGS) $(SRCS) $(KEEP) $(AMILINK) $(LIBS) -lm -o $@

# test build: takes pictures of its own window, see MSO1104_SHOT in mso1104.c
shot: $(SRCS) scope.h $(AMILIB)
	$(CC) $(CFLAGS) -DSHOT $(SRCS) $(CAPTURE) $(KEEP) $(AMILINK) $(LIBS) -lm -o mso1104_shot

sim: mso1104
	./mso1104 --sim

clean:
	rm -rf mso1104 mso1104_shot mso1104.dSYM

.PHONY: all sim shot clean
