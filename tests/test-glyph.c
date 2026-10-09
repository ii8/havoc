#ifdef NDEBUG
#undef NDEBUG
#endif
#define DEBUG_GLYPH
#include "../glyph.c"

static void check_borders(int size, int columns)
{
 static const uint32_t borders[] = {
  0x2500, 0x2502, 0x250c, 0x2510, 0x2514, 0x2518,
  0x251c, 0x2524, 0x252c, 0x2534, 0x253c
 };
 int w, h;
 font_scale(size, &w, &h);
 w *= columns;
 for (unsigned int i = 0; i < sizeof(borders) / sizeof(borders[0]); ++i) {
  unsigned char *pixels;
  int count = 0;
  assert(find_index(&font, borders[i]) == 0);
  pixels = get_glyph(borders[i], borders[i], columns);
  assert(pixels);
  assert(get_glyph(borders[i], borders[i], columns) == pixels);
  for (int p = 0; p < w * h; ++p) {
   assert(pixels[p] == 0 || pixels[p] == 255);
   count += pixels[p] != 0;
  }
  assert(count > 0);
 }
 /* Straight lines reach both cell edges, including at small/large scales. */
 unsigned char *horizontal = get_glyph(0x2500, 0x2500, columns);
 unsigned char *vertical = get_glyph(0x2502, 0x2502, columns);
 for (int x = 0; x < w; ++x)
  assert(horizontal[(h / 2) * w + x] == 255);
 for (int y = 0; y < h; ++y)
  assert(vertical[y * w + w / 2] == 255);
 /* The upper-left corner connects right/down without extra arms. */
 unsigned char *corner = get_glyph(0x250c, 0x250c, columns);
 assert(corner[(h / 2) * w + w - 1] == 255);
 assert(corner[(h - 1) * w + w / 2] == 255);
 assert(corner[(h / 2) * w] == 0);
 assert(corner[w / 2] == 0);
 /* The cross reaches all four edges. */
 unsigned char *cross = get_glyph(0x253c, 0x253c, columns);
 assert(cross[(h / 2) * w] == 255);
 assert(cross[(h / 2) * w + w - 1] == 255);
 assert(cross[w / 2] == 255);
 assert(cross[(h - 1) * w + w / 2] == 255);
 /* Space must remain blank. */
 unsigned char *space = get_glyph(' ', ' ', columns);
 for (int p = 0; p < w * h; ++p)
  assert(space[p] == 0);
 /* Other missing characters still use the font's .notdef glyph. */
 assert(find_index(&font, 0x10ffff) == 0);
 unsigned char *missing = get_glyph(0x10ffff, 0x10ffff, columns);
 unsigned char *notdef = get_glyph(0, 0, columns);
 assert(memcmp(missing, notdef, w * h) == 0);
}

int main(int argc, char **argv)
{
 int sizes[] = {6, 18, 32, 96, 300};
 assert(font_init(argc > 1 ? argv[1] : "") == 0);
 for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
  check_borders(sizes[i], 1);
  check_borders(sizes[i], 2);
 }
 font_deinit();
 puts("PASS: 11 missing borders, 5 sizes, 2 widths, cell connections, cache and space");
 return 0;
}
