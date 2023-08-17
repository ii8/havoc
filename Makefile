WAYLAND_PROTOCOLS_DIR != pkg-config --variable=pkgdatadir wayland-protocols
WAYLAND_SCANNER := wayland-scanner

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

VERSION="0.5.0"

CFLAGS ?= -Wall -Wextra -Wno-unused-parameter -Wno-parentheses -Wno-format-overflow
override CFLAGS += -DVERSION=\"$(VERSION)\"

LIBS=-lrt -lm -lutil -lwayland-client -lwayland-cursor -lxkbcommon -Ltsm -lhtsm
OBJ=xdg-shell.o \
	xdg-decoration-unstable-v1.o \
	primary-selection-unstable-v1.o \
	glyph.o \
	main.o
GEN=xdg-shell.c xdg-shell.h \
	xdg-decoration-unstable-v1.c xdg-decoration-unstable-v1.h \
	primary-selection-unstable-v1.h primary-selection-unstable-v1.c

havoc: tsm $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

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

primary-selection-unstable-v1.xml:
	cp $(WAYLAND_PROTOCOLS_DIR)/unstable/primary-selection/primary-selection-unstable-v1.xml ./
xdg-decoration-unstable-v1.xml:
	cp $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml ./
xdg-shell.xml:
	cp $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml ./

tsm:
	$(MAKE) -C $@

.PHONY: install uninstall clean tsm
