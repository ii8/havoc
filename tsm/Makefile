CFLAGS ?= -Wall -Wextra -Wno-unused-parameter -Wno-parentheses -Wno-format-overflow

OBJ=wcwidth.o shl-htable.o\
	tsm-render.o tsm-screen.o tsm-selection.o\
	tsm-unicode.o tsm-vte-charsets.o tsm-vte.o

libhtsm.a: $(OBJ)
	$(AR) $(ARFLAGS) $@ $^

clean:
	rm -f libhtsm.a $(OBJ)
