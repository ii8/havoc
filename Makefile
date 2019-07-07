
SCANNER     := wayland-scanner
PROTDATADIR != pkg-config --variable=datarootdir wayland-protocols
PROTDIR     := $(PROTDATADIR)/wayland-protocols

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin

CFLAGS=-Wall -Wextra -Wno-unused-parameter -Wno-parentheses
LDFLAGS=-lrt -lm -lutil -lwayland-client -lxkbcommon -Ltsm -lhtsm

VPATH=$(PROTDIR)/stable/xdg-shell
OBJ=xdg-shell.o gtk-primary-selection.o glyph.o main.o
GEN=xdg-shell.c xdg-shell.h gtk-primary-selection.c gtk-primary-selection.h

havoc: tsm $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

install: havoc
	install -D -t $(DESTDIR)$(BINDIR) havoc

clean:
	$(MAKE) -C tsm clean
	rm -f havoc $(GEN) $(OBJ)

$(OBJ): $(GEN)

%.c: %.xml
	$(SCANNER) private-code < $< > $@

%.h: %.xml
	$(SCANNER) client-header < $< > $@

tsm:
	$(MAKE) -C $@

.PHONY: install clean tsm
