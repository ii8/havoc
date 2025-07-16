VERSION = "0.6.0-git"

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

CFLAGS ?= -g -O2
CDEFS = -DVERSION='$(VERSION)' -D_XOPEN_SOURCE=700

WAYLAND_PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

LIBRARIES = wayland-client wayland-cursor xkbcommon
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(LIBRARIES))
PKG_LIBS := $(shell $(PKG_CONFIG) --libs $(LIBRARIES))

LIBS = -lm -lutil $(PKG_LIBS)

XML = \
	xdg-shell.xml \
	xdg-decoration-unstable-v1.xml \
	primary-selection-unstable-v1.xml

GEN = \
	xdg-shell.h \
	xdg-shell.c \
	xdg-decoration-unstable-v1.h \
	xdg-decoration-unstable-v1.c \
	primary-selection-unstable-v1.h \
	primary-selection-unstable-v1.c

OBJ = \
	main.o \
	glyph.o \
	xdg-shell.o \
	xdg-decoration-unstable-v1.o \
	primary-selection-unstable-v1.o \
	tsm/wcwidth.o \
	tsm/shl-htable.o \
	tsm/tsm-render.o \
	tsm/tsm-screen.o \
	tsm/tsm-selection.o \
	tsm/tsm-unicode.o \
	tsm/tsm-vte-charsets.o \
	tsm/tsm-vte.o

.SUFFIXES:
.SUFFIXES: .xml .h .c .o

havoc: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

$(OBJ): $(GEN)

.c.o:
	$(CC) $(PKG_CFLAGS) $(CFLAGS) $(CDEFS) -c $< -o $@

.xml.c:
	$(WAYLAND_SCANNER) private-code < $< > $@

.xml.h:
	$(WAYLAND_SCANNER) client-header < $< > $@

xdg-shell.xml:
	cp $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/$@ $@

xdg-decoration-unstable-v1.xml:
	cp $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/$@ $@

primary-selection-unstable-v1.xml:
	cp $(WAYLAND_PROTOCOLS_DIR)/unstable/primary-selection/$@ $@

install: havoc
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 havoc $(DESTDIR)$(BINDIR)/havoc

uninstall:
	rm $(DESTDIR)$(BINDIR)/havoc

clean:
	rm -f havoc $(XML) $(GEN) $(OBJ)

.PHONY: install uninstall clean
