WAYLAND_PROTOCOLS_DIR != pkg-config --variable=pkgdatadir wayland-protocols
WAYLAND_SCANNER := wayland-scanner

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

VERSION="0.6.0"

CFLAGS ?= -Wall -Wextra -Wno-unused-parameter -Wno-parentheses -Wno-format-overflow
override CFLAGS += -DVERSION=\"$(VERSION)\"

VPATH = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell \
	$(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration \
	$(WAYLAND_PROTOCOLS_DIR)/unstable/primary-selection

GEN =	xdg-shell.h xdg-shell.c \
	xdg-decoration-unstable-v1.h xdg-decoration-unstable-v1.c \
	primary-selection-unstable-v1.h primary-selection-unstable-v1.c

LIBS=-lrt -lm -lutil -lwayland-client -lwayland-cursor -lxkbcommon -Ltsm -lhtsm
OBJ=xdg-shell.o xdg-decoration-unstable-v1.o primary-selection-unstable-v1.o glyph.o main.o

havoc: tsm $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

install: havoc
	install -D -t $(DESTDIR)$(BINDIR) havoc

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/havoc

clean:
	$(MAKE) -C tsm clean
	rm -f havoc $(GEN) $(OBJ)

$(OBJ): $(GEN)

%.c: %.xml
	$(WAYLAND_SCANNER) private-code < $< > $@

%.h: %.xml
	$(WAYLAND_SCANNER) client-header < $< > $@

tsm:
	$(MAKE) -C $@

.PHONY: install uninstall clean tsm
