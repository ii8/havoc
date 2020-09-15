WAYLAND_PROTOCOLS_DIR != pkg-config --variable=pkgdatadir wayland-protocols
WAYLAND_SCANNER := wayland-scanner

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

VERSION="0.3.1"

CFLAGS ?= -Wall -Wextra -Wno-unused-parameter -Wno-parentheses
CFLAGS += `pkg-config --cflags xkbcommon` \
	`pkg-config --cflags wayland-client` \
	`pkg-config --cflags wayland-cursor`
override CFLAGS += -DVERSION=\"$(VERSION)\"

VPATH=$(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell
LIBS=-lrt -lm -lutil -lwayland-client -lwayland-cursor -lxkbcommon -Ltsm -lhtsm
LIBS+=`pkg-config --libs xkbcommon` \
	`pkg-config --libs wayland-client` \
	`pkg-config --libs wayland-cursor`

OBJ=xdg-shell.o gtk-primary-selection.o glyph.o main.o
GEN=xdg-shell.c xdg-shell.h gtk-primary-selection.c gtk-primary-selection.h

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
