#ifndef HAVOC_GLYPH_H
#define HAVOC_GLYPH_H

#include <stdbool.h>
#include <stdint.h>

enum font_style {
	FONT_REGULAR, FONT_BOLD, FONT_ITALIC, FONT_BOLD_ITALIC, FONT_COUNT
};

struct font;

struct font *font_init(const char *path);
void font_scale(struct font *font, int size, const struct font *base);
void font_size(const struct font *font, int *width, int *height);
bool font_has_glyph(const struct font *font, uint32_t character);
void font_deinit(struct font *font);
unsigned char *get_glyph(struct font *font, uint32_t id,
                         uint32_t character, int cell_width);

#endif
