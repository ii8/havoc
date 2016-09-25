
CC=cc
SCANNER=wayland-scanner
PREFIX=/usr/local
BINDIR=$(PREFIX)/bin

LDFLAGS=-lrt -lm -lutil -lwayland-client -ltsm -lxkbcommon

OBJ=xdg-shell.o glyph.o main.o
GEN=xdg-shell.c xdg-shell.h

all: $(GEN) havoc

clean:
	rm -f havoc $(GEN) $(OBJ)

havoc: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

install: havoc
	install havoc $(BINDIR)

xdg-shell.c: xdg-shell-unstable-v6.xml
	$(SCANNER) code < $< > $@

xdg-shell.h: xdg-shell-unstable-v6.xml
	$(SCANNER) client-header < $< > $@

.PHONY: all clean
