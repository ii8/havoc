
CC=cc
SCANNER=wayland-scanner
PROTDIR=$(shell pkg-config --variable=datarootdir wayland-protocols)/wayland-protocols

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

xdg-shell.c: $(PROTDIR)/stable/xdg-shell/xdg-shell.xml
	$(SCANNER) code < $< > $@

xdg-shell.h: $(PROTDIR)/stable/xdg-shell/xdg-shell.xml
	$(SCANNER) client-header < $< > $@

.PHONY: all clean
