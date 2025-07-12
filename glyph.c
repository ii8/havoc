/* stb_truetype.h - v1.11 - public domain
 * originally authored from 2009-2015 by Sean Barrett / RAD Game Tools
 * https://github.com/nothings/stb
 * modified for use in havoc terminal emulator
 */

#include <inttypes.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "fallback.h"

#ifdef DEBUG_GLYPH
#include <assert.h>
#else
#define assert(x) (void)0
#endif

struct node {
	bool red;
	struct node *link[2];
	uint32_t ch;
	unsigned char *bitmap;
};

static struct node leaf = { false, { NULL, NULL }, 0, NULL };

struct font {
	unsigned char *data;
	size_t size;
	bool mmapped;

	int fontstart;

	int num_glyphs;
	short num_metrics;

	int loca, head, glyf, hhea, hmtx;
	int index_map;

	/* format needed to map from glyph index to glyph */
	int indextolocformat;

	int width, height;
	int ascent;
	float scale;

	struct node *cache;
};

static struct font font;

enum {
	VMOVE = 1,
	VLINE,
	VCURVE
};

struct vertex {
	short x, y, cx, cy;
	unsigned char type, padding;
};

struct bitmap {
	int w, h, stride;
	unsigned char *pixels;
};

/* http://developer.apple.com/textfonts/TTRefMan/RM06/Chap6name.html
 * http://www.microsoft.com/typography/otspec/name.htm */
#define STBTT_PLATFORM_ID_UNICODE 0
#define STBTT_PLATFORM_ID_MICROSOFT 3
#define STBTT_MS_EID_UNICODE_BMP 1
#define STBTT_MS_EID_UNICODE_FULL 10

#define read_byte(p) (*(uint8_t *)(p))
#define read_char(p) (*(int8_t *)(p))

static uint16_t read_ushort(const uint8_t *p)
{
	if (p < font.data || p + 1 > font.data + font.size - 1) {
		fprintf(stderr, "font file is corrupt\n");
		return 0;
	}

	return (p[0] << 8) + p[1];
}

static int16_t read_short(const uint8_t *p)
{
	if (p < font.data || p + 1 > font.data + font.size - 1) {
		fprintf(stderr, "font file is corrupt\n");
		return 0;
	}

	return (p[0] << 8) + p[1];
}

static uint32_t read_ulong(const uint8_t *p)
{
	if (p < font.data || p + 3 > font.data + font.size - 1) {
		fprintf(stderr, "font file is corrupt\n");
		return 0;
	}

	return p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static uint32_t find_table(uint8_t *data, uint32_t fontstart, const char *tag)
{
	int32_t i, num_tables = read_ushort(data + fontstart + 4);
	uint32_t tabledir = fontstart + 12;

	for (i = 0; i < num_tables; ++i) {
		uint8_t *loc = data + (tabledir + 16 * i);

		if (strncmp((char *)loc, tag, 4) == 0)
			return read_ulong(loc + 8);
	}

	return 0;
}

static int setup(struct font *f, int fontstart)
{
	uint32_t cmap, t;
	int32_t i, num_tables;
	uint8_t *data;

	data = f->data;
	f->fontstart = fontstart;

	cmap = find_table(data, fontstart, "cmap");
	f->loca = find_table(data, fontstart, "loca");
	f->head = find_table(data, fontstart, "head");
	f->glyf = find_table(data, fontstart, "glyf");
	f->hhea = find_table(data, fontstart, "hhea");
	f->hmtx = find_table(data, fontstart, "hmtx");
	if (!cmap || !f->loca || !f->head || !f->glyf || !f->hhea || !f->hmtx)
		return -1;

	t = find_table(data, fontstart, "maxp");
	if (t)
		f->num_glyphs = read_ushort(data + t + 4);
	else
		f->num_glyphs = 0xffff;

	/* find a cmap encoding table we understand *now* to avoid searching
	 * later. (todo: could make this installable)
	 * the same regardless of glyph. */
	num_tables = read_ushort(data + cmap + 2);
	f->index_map = 0;
	for (i = 0; i < num_tables; ++i) {
		uint32_t enc = cmap + 4 + 8 * i;

		/* find an encoding we understand */
		switch (read_ushort(data + enc)) {
		case STBTT_PLATFORM_ID_MICROSOFT:
			switch (read_ushort(data + enc + 2)) {
			case STBTT_MS_EID_UNICODE_BMP:
			case STBTT_MS_EID_UNICODE_FULL:
				f->index_map =
					cmap + read_ulong(data + enc + 4);
				break;
			}
			break;
		case STBTT_PLATFORM_ID_UNICODE:
			/* all encodingIDs are unicode */
			f->index_map = cmap + read_ulong(data + enc + 4);
			break;
		}
	}
	if (f->index_map == 0)
		return -1;

	f->indextolocformat = read_ushort(data + f->head + 50);
	return 0;
}

static int find_index(const struct font *f, uint32_t codepoint)
{
	uint8_t *data = f->data;
	uint32_t index_map = f->index_map;
	uint16_t format = read_ushort(data + index_map + 0);

	if (format == 0) { /* apple byte encoding */
		if (codepoint + 6 < read_ushort(data + index_map + 2))
			return read_byte(data + index_map + 6 + codepoint);
		return 0;
	} else if (format == 6) {
		uint32_t first = read_ushort(data + index_map + 6);
		uint32_t count = read_ushort(data + index_map + 8);
		if (codepoint >= first && codepoint < first + count)
			return read_ushort(data + index_map + 10
					   + (codepoint - first) * 2);
		return 0;
	} else if (format == 2) {
		return 0; /* TODO: high-byte mapping for jpn/cn/kr */
	} else if (format == 4) {
		/* standard mapping for windows fonts: binary search
		 * collection of ranges */
		uint16_t offset, start, last, item;
		uint8_t *idx;
		uint16_t segcount = read_ushort(data + index_map + 6) >> 1;
		uint16_t range = read_ushort(data + index_map + 8) >> 1;
		uint16_t selector = read_ushort(data + index_map + 10);
		uint16_t shift = read_ushort(data + index_map + 12) >> 1;

		/* do a binary search of the segments */
		uint32_t end_count = index_map + 14;
		uint32_t search = end_count;

		if (codepoint > 0xffff)
			return 0;

		/* they lie from end_count .. end_count + segcount
		 * but range is the nearest power of two, so... */
		if (codepoint >= read_ushort(data + search + shift * 2))
			search += shift * 2;

		/* now decrement to bias correctly to find smallest */
		search -= 2;
		while (selector) {
			uint16_t end;
			range >>= 1;
			end = read_ushort(data + search + range * 2);
			if (codepoint > end)
				search += range * 2;
			--selector;
		}

		item = (search + 2 - end_count) >> 1;
		idx = data + index_map + 14;
		start = read_ushort(idx + segcount * 2 + 2 + 2 * item);
		last = read_ushort(data + end_count + 2 * item);
		if (codepoint < start || codepoint > last)
			return 0;

		offset = read_ushort(idx + segcount * 6 + 2 + 2 * item);
		if (offset == 0)
			return codepoint + read_short(idx + segcount * 4
						      + 2 + 2 * item);

		return read_ushort(data + offset + (codepoint - start) * 2
				   + index_map + 14
				   + segcount * 6 + 2 + 2 * item);
	} else if (format == 12 || format == 13) {
		uint32_t ngroups = read_ulong(data + index_map + 12);
		int32_t low = 0, high = (int32_t)ngroups;
		/* Binary search the right group. */
		while (low < high) {
			/* rounds down, so low <= mid < high */
			int32_t mid = low + ((high - low) >> 1);
			uint8_t *a = data + index_map + 16 + mid * 12;
			uint32_t start_char = read_ulong(a);
			uint32_t end_char = read_ulong(a + 4);

			if (codepoint < start_char) {
				high = mid;
			} else if (codepoint > end_char) {
				low = mid + 1;
			} else {
				uint32_t startg = read_ulong(a + 8);
				if (format == 12)
					return startg + codepoint - start_char;
				else /* format == 13 */
					return startg;
			}
		}
		return 0; /* not found */
	}
	/* TODO */
	fputs("glyph: TODO format\n", stderr);
	return 0;
}

static void vinit(struct vertex *v, uint8_t type, int32_t x, int32_t y,
		  int32_t cx, int32_t cy)
{
	v->type = type;
	v->x = (int16_t)x;
	v->y = (int16_t)y;
	v->cx = (int16_t)cx;
	v->cy = (int16_t)cy;
}

static int glyph_offset(const struct font *f, int glyph_index)
{
	int g1, g2;
	uint8_t *loc = f->data + f->loca;

	if (glyph_index >= f->num_glyphs)
		return -1; /* glyph index out of range */
	if (f->indextolocformat >= 2)
		return -1; /* unknown index->glyph map format */

	if (f->indextolocformat == 0) {
		g1 = f->glyf + read_ushort(loc + glyph_index * 2) * 2;
		g2 = f->glyf + read_ushort(loc + glyph_index * 2 + 2) * 2;
	} else {
		g1 = f->glyf + read_ulong(loc + glyph_index * 4);
		g2 = f->glyf + read_ulong(loc + glyph_index * 4 + 4);
	}

	return g1 == g2 ? -1 : g1; /* if length is 0, return -1 */
}

static int close_shape(struct vertex *vertices, int n,
		       int was_off, int start_off, int32_t sx, int32_t sy,
		       int32_t scx, int32_t scy, int32_t cx, int32_t cy)
{
	if (start_off) {
		if (was_off)
			vinit(&vertices[n++], VCURVE, (cx + scx) >> 1,
			      (cy + scy) >> 1, cx, cy);
		vinit(&vertices[n++], VCURVE, sx, sy, scx, scy);
	} else {
		if (was_off)
			vinit(&vertices[n++], VCURVE, sx, sy, cx, cy);
		else
			vinit(&vertices[n++], VLINE, sx, sy, 0, 0);
	}
	return n;
}

/* returns number of vertices and fills *pvertices with the pointer to them
 * these are expressed in unscaled coordinates
 *
 * The shape is a series of countours. Each one starts with
 * a VMOVE, then consists of a series of mixed VLINE and VCURVE segments.
 * A lineto draws a line from previous endpoint to its x,y; a curveto
 * draws a quadratic bezier from previous endpoint to
 * its x,y, using cx,cy as the bezier control point. */
static int glyph_shape(const struct font *f, int glyph_index,
		       struct vertex **pvertices)
{
	int16_t ncontours;
	uint8_t *contour_ends;
	struct vertex *vertices = 0;
	int vcount = 0;
	int g = glyph_offset(f, glyph_index);

	*pvertices = NULL;
	if (g < 0)
		return 0;

	ncontours = read_short(f->data + g);
	if (ncontours > 0) {
		uint8_t flags = 0, flagcount;
		int32_t ins, i, j = 0, m, n;
		int32_t next_move, was_off = 0, off, start_off = 0;
		int32_t x, y, cx, cy, sx, sy, scx, scy;
		uint8_t *points;

		contour_ends = (f->data + g + 10);
		ins = read_ushort(f->data + g + 10 + ncontours * 2);
		points = f->data + g + 10 + ncontours * 2 + 2 + ins;

		n = 1 + read_ushort(contour_ends + ncontours * 2 - 2);

		/* a loose bound on how many vertices we might need */
		m = n + 2 * ncontours;
		vertices = malloc(m * sizeof(vertices[0]));
		if (vertices == NULL)
			return 0;

		next_move = 0;
		flagcount = 0;

		/* in first pass, we load uninterpreted data into the
		 * allocated array above, shifted to the end of the array so
		 * we won't overwrite it when we create our final data
		 * starting from the front */

		/* starting offset for uninterpreted data, regardless
		 * of how m ends up being calculated */
		off = m - n;

		/* load flags */
		for (i = 0; i < n; ++i) {
			if (flagcount == 0) {
				flags = *points++;
				if (flags & 8)
					flagcount = *points++;
			} else {
				--flagcount;
			}
			vertices[off + i].type = flags;
		}

		/* load x coordinates */
		x = 0;
		for (i = 0; i < n; ++i) {
			flags = vertices[off + i].type;
			if (flags & 2) {
				int16_t dx = *points++;
				x += (flags & 16) ? dx : -dx; /* ??? */
			} else {
				if (!(flags & 16)) {
					x = x + (int16_t)(points[0] * 256
							  + points[1]);
					points += 2;
				}
			}
			vertices[off + i].x = (int16_t) x;
		}

		/* now load y coordinates */
		y = 0;
		for (i = 0; i < n; ++i) {
			flags = vertices[off + i].type;
			if (flags & 4) {
				int16_t dy = *points++;
				y += (flags & 32) ? dy : -dy; /* ??? */
			} else {
				if (!(flags & 32)) {
					y = y + (int16_t) (points[0] * 256
							   + points[1]);
					points += 2;
				}
			}
			vertices[off + i].y = (int16_t) y;
		}

		/* now convert them to our format */
		vcount = 0;
		sx = sy = cx = cy = scx = scy = 0;
		for (i = 0; i < n; ++i) {
			flags = vertices[off + i].type;
			x = (int16_t) vertices[off + i].x;
			y = (int16_t) vertices[off + i].y;

			if (next_move == i) {
				if (i != 0)
					vcount = close_shape(vertices,
							     vcount, was_off,
							     start_off, sx,
							     sy, scx, scy, cx,
							     cy);

				/* now start the new one */
				start_off = !(flags & 1);
				/* if we start off with an off-curve point,
				 * then when we need to find a point on the
				 * curve where we can start, and we need to
				 * save some state for when we wraparound. */
				if (start_off) {
					scx = x;
					scy = y;
					if (!(vertices[off + i + 1].type & 1)) {
						/* next point is also a curve point, so interpolate an on-point curve */
						sx = (x + (int32_t) vertices[off + i + 1].x) >> 1;
						sy = (y + (int32_t) vertices[off + i + 1].y) >> 1;
					} else {
						/* otherwise just use the next point as our start point */
						sx = (int32_t) vertices[off + i + 1].x;
						sy = (int32_t) vertices[off + i + 1].y;
						++i; /* we're using point i+1 as the starting point, so skip it */
					}
				} else {
					sx = x;
					sy = y;
				}
				vinit(&vertices[vcount++], VMOVE, sx, sy, 0, 0);
				was_off = 0;
				next_move = read_ushort(contour_ends + j * 2);
				next_move += 1;
				++j;
			} else {
				if (!(flags & 1)) { /* if it's a curve */
					if (was_off) /* two off-curve control points in a row means interpolate an on-curve midpoint */
						vinit(&vertices [vcount++], VCURVE, (cx + x) >> 1, (cy + y) >> 1, cx, cy);
					cx = x;
					cy = y;
					was_off = 1;
				} else {
					if (was_off)
						vinit(&vertices [vcount++], VCURVE, x, y, cx, cy);
					else
						vinit(&vertices [vcount++], VLINE, x, y, 0, 0);
					was_off = 0;
				}
			}
		}
		vcount = close_shape(vertices, vcount, was_off, start_off,
				     sx, sy, scx, scy, cx, cy);
	} else if (ncontours < 0) {
		/* Compound shapes */
		uint16_t flags = (1 << 5);
		uint8_t *comp = f->data + g + 10;
		vcount = 0;
		vertices = 0;
		while (flags & (1 << 5)) {
			uint16_t gidx;
			int comp_num_verts = 0, i;
			struct vertex *comp_verts = 0, *tmp = 0;
			float mtx[6] = { 1, 0, 0, 1, 0, 0 }, m, n;

			flags = read_short(comp);
			comp += 2;
			gidx = read_short(comp);
			comp += 2;

			if (flags & 2) { /* XY values */
				if (flags & 1) { /* shorts */
					mtx[4] = read_short(comp);
					comp += 2;
					mtx[5] = read_short(comp);
					comp += 2;
				} else {
					mtx[4] = read_char(comp);
					comp += 1;
					mtx[5] = read_char(comp);
					comp += 1;
				}
			} else {
				/* TODO handle matching point */
			}

			if (flags & 1 << 3) {
				/* WE_HAVE_A_SCALE */
				mtx[0] = mtx[3] = read_short(comp) / 16384.0f;
				comp += 2;
				mtx[1] = mtx[2] = 0;
			} else if (flags & 1 << 6) {
				/* WE_HAVE_AN_X_AND_YSCALE */
				mtx[0] = read_short(comp) / 16384.0f;
				comp += 2;
				mtx[1] = mtx[2] = 0;
				mtx[3] = read_short(comp) / 16384.0f;
				comp += 2;
			} else if (flags & 1 << 7) {
				/* WE_HAVE_A_TWO_BY_TWO */
				mtx[0] = read_short(comp) / 16384.0f;
				comp += 2;
				mtx[1] = read_short(comp) / 16384.0f;
				comp += 2;
				mtx[2] = read_short(comp) / 16384.0f;
				comp += 2;
				mtx[3] = read_short(comp) / 16384.0f;
				comp += 2;
			}

			/* Find transformation scales */
			m = (float)sqrt(mtx[0] * mtx[0] + mtx[1] * mtx[1]);
			n = (float)sqrt(mtx[2] * mtx[2] + mtx[3] * mtx[3]);

			/* Get indexed glyph */
			comp_num_verts = glyph_shape(f, gidx, &comp_verts);
			if (comp_num_verts <= 0)
				continue;

			/* Transform vertices */
			for (i = 0; i < comp_num_verts; ++i) {
				struct vertex *v = &comp_verts[i];
				short x, y;
				x = v->x;
				y = v->y;
				v->x = m * (mtx[0] * x + mtx[2] * y + mtx[4]);
				v->y = n * (mtx[1] * x + mtx[3] * y + mtx[5]);
				x = v->cx;
				y = v->cy;
				v->cx = m * (mtx[0] * x + mtx[2] * y + mtx[4]);
				v->cy = n * (mtx[1] * x + mtx[3] * y + mtx[5]);
			}

			/* Append vertices */
			tmp = malloc((vcount + comp_num_verts)
				      * sizeof(struct vertex));
			if (tmp == NULL) {
				if (vertices)
					free(vertices);
				if (comp_verts)
					free(comp_verts);
				return 0;
			}

			if (vcount > 0)
				memcpy(tmp, vertices,
				       vcount * sizeof(struct vertex));
			memcpy(tmp + vcount, comp_verts,
			       comp_num_verts * sizeof(struct vertex));
			if (vertices)
				free(vertices);
			vertices = tmp;
			free(comp_verts);
			vcount += comp_num_verts;
		}
	} else {
		/* ncontours == 0, do nothing */
	}

	*pvertices = vertices;
	return vcount;
}

static int get_ascent(struct font *f)
{
	return read_short(f->data + f->hhea + 4);
}

static int get_descent(struct font *f)
{
	return read_short(f->data + f->hhea + 6);
}

static int get_linegap(struct font *f)
{
	return read_short(f->data + f->hhea + 8);
}

static void get_glyph_origin(struct font *f, int glyph, int *x, int *y)
{
	int g = glyph_offset(f, glyph);

	if (g < 0) {
		*x = 0, *y = 0;
		return;
	}

	*x = floor(read_short(f->data + g + 2) * f->scale);
	*y = floor(-read_short(f->data + g + 8) * f->scale);
}

struct hheap_chunk {
	struct hheap_chunk *next;
};

struct hheap {
	struct hheap_chunk *head;
	void *first_free;
	int num_in_head_chunk;
} hheap;

static void *hheap_alloc(struct hheap *hh, size_t size)
{
	if (hh->first_free) {
		void *p = hh->first_free;
		hh->first_free = *(void **)p;
		return p;
	} else {
		if (hh->num_in_head_chunk == 0) {
			int count = (size < 32 ? 2000 : size < 128 ? 800 : 100);
			struct hheap_chunk *c = malloc(sizeof(*c) + size * count);
			if (c == NULL)
				return NULL;
			c->next = hh->head;
			hh->head = c;
			hh->num_in_head_chunk = count;
		}
		--hh->num_in_head_chunk;
		return (char *)(hh->head) + sizeof(struct hheap_chunk) + size * hh->num_in_head_chunk;
	}
}

static void hheap_free(struct hheap *hh, void *p)
{
	*(void **)p = hh->first_free;
	hh->first_free = p;
}

static void hheap_cleanup(struct hheap *hh)
{
	struct hheap_chunk *c = hh->head;
	while (c) {
		struct hheap_chunk *n = c->next;
		free(c);
		c = n;
	}
}

struct edge {
	float x0, y0, x1, y1;
	int invert;
};

struct active_edge {
	struct active_edge *next;
	float fx, fdx, fdy;
	float direction;
	float sy;
	float ey;
};

static struct active_edge *new_active(struct hheap *hh, struct edge *e,
				      int off_x, float start_point)
{
	struct active_edge *z = hheap_alloc(hh, sizeof(*z));
	float dxdy = (e->x1 - e->x0) / (e->y1 - e->y0);

	if (!z)
		return z;
	z->fdx = dxdy;
	z->fdy = dxdy != 0.0f ? (1.0f / dxdy) : 0.0f;
	z->fx = e->x0 + dxdy * (start_point - e->y0);
	z->fx -= off_x;
	z->direction = e->invert ? 1.0f : -1.0f;
	z->sy = e->y0;
	z->ey = e->y1;
	z->next = 0;
	return z;
}

/* the edge passed in here does not cross the vertical line at x or
 * the vertical line at x+1 (i.e. it has already been clipped to those) */
static void handle_clipped_edge(float *scanline, int x, struct active_edge *e,
				float x0, float y0, float x1, float y1)
{
	if (y0 == y1)
		return;
	assert(y0 < y1);
	assert(e->sy <= e->ey);
	if (y0 > e->ey)
		return;
	if (y1 < e->sy)
		return;
	if (y0 < e->sy) {
		x0 += (x1 - x0) * (e->sy - y0) / (y1 - y0);
		y0 = e->sy;
	}
	if (y1 > e->ey) {
		x1 += (x1 - x0) * (e->ey - y1) / (y1 - y0);
		y1 = e->ey;
	}

	if (x0 == x)
		assert(x1 <= x + 1);
	else if (x0 == x + 1)
		assert(x1 >= x);
	else if (x0 <= x)
		assert(x1 <= x);
	else if (x0 >= x + 1)
		assert(x1 >= x + 1);
	else
		assert(x1 >= x && x1 <= x + 1);

	if (x0 <= x && x1 <= x) {
		scanline[x] += e->direction * (y1 - y0);
	} else if (x0 >= x + 1 && x1 >= x + 1) {
	} else {
		float coverage = 1 - ((x0 - x) + (x1 - x)) / 2;
		assert(x0 >= x && x0 <= x + 1 && x1 >= x && x1 <= x + 1);
		scanline[x] += e->direction * (y1 - y0) * coverage;
	}
}


static float sized_trapezoid_area(float height, float top_width, float bottom_width)
{
   assert(top_width >= 0);
   assert(bottom_width >= 0);
   return (top_width + bottom_width) / 2.0f * height;
}

static float position_trapezoid_area(float height, float tx0, float tx1, float bx0, float bx1)
{
   return sized_trapezoid_area(height, tx1 - tx0, bx1 - bx0);
}

static float sized_triangle_area(float height, float width)
{
   return height * width / 2;
}

static void fill_active_edges_new(float *scanline, float *scanline_fill,
				  int len, struct active_edge *e, float y_top)
{
	float y_bottom = y_top + 1;

	while (e) {
		/* brute force every pixel
		 * compute intersection points with top & bottom */
		assert(e->ey >= y_top);

		if (e->fdx == 0) {
			float x0 = e->fx;
			if (x0 < len) {
				if (x0 >= 0) {
					handle_clipped_edge(scanline, (int)x0, e, x0, y_top, x0, y_bottom);
					handle_clipped_edge(scanline_fill - 1, (int)x0 + 1, e, x0, y_top, x0, y_bottom);
				} else {
					handle_clipped_edge(scanline_fill - 1, 0, e, x0, y_top, x0, y_bottom);
				}
			}
		} else {
			float x0 = e->fx;
			float dx = e->fdx;
			float xb = x0 + dx;
			float x_top, x_bottom;
			float sy0, sy1;
			float dy = e->fdy;
			assert(e->sy <= y_bottom && e->ey >= y_top);

			/* compute endpoints of line segment clipped to this scanline (if the
			 * line segment starts on this scanline. x0 is the intersection of the
			 * line with y_top, but that may be off the line segment. */
			if (e->sy > y_top) {
				x_top = x0 + dx * (e->sy - y_top);
				sy0 = e->sy;
			} else {
				x_top = x0;
				sy0 = y_top;
			}
			if (e->ey < y_bottom) {
				x_bottom = x0 + dx * (e->ey - y_top);
				sy1 = e->ey;
			} else {
				x_bottom = xb;
				sy1 = y_bottom;
			}

			if (x_top >= 0 && x_bottom >= 0 && x_top < len
			    && x_bottom < len) {
				// from here on, we don't have to range check x values

				if ((int)x_top == (int)x_bottom) {
					float height;
					// simple case, only spans one pixel
					int x = (int)x_top;
					height = (sy1 - sy0) * e->direction;
					assert(x >= 0 && x < len);
					scanline[x] += position_trapezoid_area(height, x_top, x+1.0f, x_bottom, x+1.0f);
					scanline_fill[x] += height; /* everything right of this pixel is filled */
				} else {
					int x, x1, x2;
					float y_crossing, y_final, step, sign, area;
					/* covers 2+ pixels */
					if (x_top > x_bottom) {
						/* flip scanline vertically
						 * signed area is the same */
						float t;
						sy0 = y_bottom - (sy0 - y_top);
						sy1 = y_bottom - (sy1 - y_top);
						t = sy0, sy0 = sy1, sy1 = t;
						t = x_bottom, x_bottom = x_top, x_top = t;
						dx = -dx;
						dy = -dy;
						t = x0, x0 = xb, xb = t;
					}
					assert(dy >= 0);
					assert(dx >= 0);

					x1 = (int)x_top;
					x2 = (int)x_bottom;
					// compute intersection with y axis at x1+1
					y_crossing = y_top + dy * (x1+1 - x0);
					// compute intersection with y axis at x2
					y_final = y_top + dy * (x2 - x0); 

					// if x2 is right at the right edge of x1, y_crossing can blow up, github #1057
					if (y_crossing > y_bottom)
						y_crossing = y_bottom;

					sign = e->direction;
					// area of the rectangle covered from sy0..y_crossing
					area = sign * (y_crossing - sy0);
					// area of the triangle (x_top,sy0), (x1+1,sy0), (x1+1,y_crossing)
					scanline[x1] += sized_triangle_area(area, x1+1 - x_top);

					// check if final y_crossing is blown up; no test case for this
					if (y_final > y_bottom) {
						y_final = y_bottom;
						dy = (y_final - y_crossing ) / (x2 - (x1+1)); // if denom=0, y_final = y_crossing, so y_final <= y_bottom
					}

					step = sign * dy * 1; // dy is dy/dx, change in y for every 1 change in x, which is also how much pixel area changes for each step in x
					for (x = x1 + 1; x < x2; ++x) {
						scanline[x] += area + step/2; // area of trapezoid is 1*step/2
						area += step;
					}

					assert(fabs(area) <= 1.01f);
					assert(sy1 > y_final-0.01f);

					// area covered in the last pixel is the rectangle from all the pixels to the left,
					// plus the trapezoid filled by the line segment in this pixel all the way to the right edge
					scanline[x2] += area + sign * position_trapezoid_area(sy1-y_final, (float) x2, x2+1.0f, x_bottom, x2+1.0f);
					// the rest of the line is filled based on the total height of the line segment in this pixel
					scanline_fill[x2] += sign * (sy1 - sy0);
				}
			} else {
				/* if edge goes outside of box we're drawing, we require
				 * clipping logic. since this does not match the intended use
				 * of this library, we use a different, very slow brute
				 * force implementation */
				int x;
				for (x = 0; x < len; ++x) {
					/* cases:
					 *
					 * there can be up to two intersections with the pixel. any intersection
					 * with left or right edges can be handled by splitting into two (or three)
					 * regions. intersections with top & bottom do not necessitate case-wise logic.
					 *
					 * the old way of doing this found the intersections with the left & right edges,
					 * then used some simple logic to produce up to three segments in sorted order
					 * from top-to-bottom. however, this had a problem: if an x edge was epsilon
					 * across the x border, then the corresponding y position might not be distinct
					 * from the other y segment, and it might ignored as an empty segment. to avoid
					 * that, we need to explicitly produce segments based on x positions. */

					/* rename variables to clear pairs */
					float y0 = y_top;
					float x1 = (float)(x);
					float x2 = (float)(x + 1);
					float x3 = xb;
					float y3 = y_bottom;
					float y1, y2;

					y1 = (x - x0) / dx + y_top;
					y2 = (x + 1 - x0) / dx + y_top;

					if (x0 < x1 && x3 > x2) { /* three segments descending down-right */
						handle_clipped_edge(scanline, x, e, x0, y0, x1, y1);
						handle_clipped_edge(scanline, x, e, x1, y1, x2, y2);
						handle_clipped_edge(scanline, x, e, x2, y2, x3, y3);
					} else if (x3 < x1 && x0 > x2) { /* three segments descending down-left */
						handle_clipped_edge(scanline, x, e, x0, y0, x2, y2);
						handle_clipped_edge(scanline, x, e, x2, y2, x1, y1);
						handle_clipped_edge(scanline, x, e, x1, y1, x3, y3);
					} else if (x0 < x1 && x3 > x1) { /* two segments across x, down-right */
						handle_clipped_edge(scanline, x, e, x0, y0, x1, y1);
						handle_clipped_edge(scanline, x, e, x1, y1, x3, y3);
					} else if (x3 < x1 && x0 > x1) { /* two segments across x, down-left */
						handle_clipped_edge(scanline, x, e, x0, y0, x1, y1);
						handle_clipped_edge(scanline, x, e, x1, y1, x3, y3);
					} else if (x0 < x2 && x3 > x2) { /* two segments across x+1, down-right */
						handle_clipped_edge(scanline, x, e, x0, y0, x2, y2);
						handle_clipped_edge(scanline, x, e, x2, y2, x3, y3);
					} else if (x3 < x2 && x0 > x2) { /* two segments across x+1, down-left */
						handle_clipped_edge(scanline, x, e, x0, y0, x2, y2);
						handle_clipped_edge(scanline, x, e, x2, y2, x3, y3);
					} else { /* one segment */
						handle_clipped_edge(scanline, x, e, x0, y0, x3, y3);
					}
				}
			}
		}
		e = e->next;
	}
}

/* directly AA rasterize edges w/o supersampling */
static void rasterize_sorted_edges(struct bitmap *result, struct edge *e,
				   int n, int vsubsample, int off_x, int off_y)
{
	struct hheap hh = { 0, 0, 0 };
	struct active_edge *active = NULL;
	int y, j = 0, i;
	float scanline_data[129], *scanline, *scanline2;

	if (result->w > 64)
		scanline = malloc((result->w * 2 + 1) * sizeof(float));
	else
		scanline = scanline_data;

	scanline2 = scanline + result->w;

	y = off_y;
	e[n].y0 = (float)(off_y + result->h) + 1;

	while (j < result->h) {
		/* find center of pixel for this scanline */
		float scan_y_top = y + 0.0f;
		float scan_y_bottom = y + 1.0f;
		struct active_edge **step = &active;

		memset(scanline, 0, result->w * sizeof(scanline[0]));
		memset(scanline2, 0, (result->w + 1) * sizeof(scanline[0]));

		/* update all active edges; */
		/* remove all active edges that terminate before the top
		 * of this scanline */
		while (*step) {
			struct active_edge *z = *step;
			if (z->ey <= scan_y_top) {
				*step = z->next;
				assert(z->direction);
				z->direction = 0;
				hheap_free(&hh, z);
			} else {
				step = &((*step)->next);
			}
		}

		/* insert all edges that start before the bottom
		 * of this scanline */
		while (e->y0 <= scan_y_bottom) {
			if (e->y0 != e->y1) {
				struct active_edge *z = new_active(&hh, e,
								   off_x,
								   scan_y_top);
				if (z != NULL) {
					if (j == 0 && off_y != 0) {
						if (z->ey < scan_y_top) {
							z->ey = scan_y_top;
						}
					}
					assert(z->ey >= scan_y_top);
					/* insert at front */
					z->next = active;
					active = z;
				}
			}
			++e;
		}

		/* now process all active edges */
		if (active)
			fill_active_edges_new(scanline, scanline2 + 1,
					      result->w, active, scan_y_top);

		{
			float sum = 0;
			for (i = 0; i < result->w; ++i) {
				float k;
				int m;
				sum += scanline2[i];
				k = scanline[i] + sum;
				k = (float)fabs(k) * 255 + 0.5f;
				m = (int)k;
				if (m > 255)
					m = 255;
				result->pixels[j * result->stride + i] =
				    (unsigned char)m;
			}
		}
		/* advance all the edges */
		step = &active;
		while (*step) {
			struct active_edge *z = *step;
			/* advance to position for current scanline */
			z->fx += z->fdx;
			step = &((*step)->next); /* advance through list */
		}

		++y;
		++j;
	}

	hheap_cleanup(&hh);

	if (scanline != scanline_data)
		free(scanline);
}

#define cmp(a, b) ((a)->y0 < (b)->y0)

static void sort_edges_ins_sort(struct edge *p, int n)
{
	int i, j;
	for (i = 1; i < n; ++i) {
		struct edge t = p[i], *a = &t;
		j = i;
		while (j > 0) {
			struct edge *b = &p[j - 1];
			int c = cmp(a, b);
			if (!c)
				break;
			p[j] = p[j - 1];
			--j;
		}
		if (i != j)
			p[j] = t;
	}
}

static void sort_edges_quicksort(struct edge *p, int n)
{
	/* threshhold for transitioning to insertion sort */
	while (n > 12) {
		struct edge t;
		int c01, c12, c, m, i, j;

		/* compute median of three */
		m = n >> 1;
		c01 = cmp(&p[0], &p[m]);
		c12 = cmp(&p[m], &p[n - 1]);
		/* if 0 >= mid >= end, or 0 < mid < end, then use mid */
		if (c01 != c12) {
			/* otherwise, we'll need to swap something else to middle */
			int z;
			c = cmp(&p[0], &p[n - 1]);
			/* 0>mid && mid<n:  0>n => n; 0<n => 0 */
			/* 0<mid && mid>n:  0>n => 0; 0<n => n */
			z = (c == c12) ? 0 : n - 1;
			t = p[z];
			p[z] = p[m];
			p[m] = t;
		}
		/* now p[m] is the median-of-three */
		/* swap it to the beginning so it won't move around */
		t = p[0];
		p[0] = p[m];
		p[m] = t;

		/* partition loop */
		i = 1;
		j = n - 1;
		for (;;) {
			/* handling of equality is crucial here */
			/* for sentinels & efficiency with duplicates */
			for (;; ++i) {
				if (!cmp(&p[i], &p[0]))
					break;
			}
			for (;; --j) {
				if (!cmp(&p[0], &p[j]))
					break;
			}
			/* make sure we haven't crossed */
			if (i >= j)
				break;
			t = p[i];
			p[i] = p[j];
			p[j] = t;

			++i;
			--j;
		}
		/* recurse on smaller side, iterate on larger */
		if (j < (n - i)) {
			sort_edges_quicksort(p, j);
			p = p + i;
			n = n - i;
		} else {
			sort_edges_quicksort(p + i, n - i);
			n = j;
		}
	}
}

static void sort_edges(struct edge * p, int n)
{
	sort_edges_quicksort(p, n);
	sort_edges_ins_sort(p, n);
}

struct point {
	float x, y;
};

static void rasterize(struct bitmap *result, struct point *pts, int *wcount,
		      int windings, float scale_x, float scale_y,
		      float shift_x, float shift_y, int off_x, int off_y,
		      int invert)
{
	float yscale_inv = invert ? -scale_y : scale_y;
	struct edge *e;
	int n, i, j, k, m;
	int vsubsample = 1;
	/* vsubsample should divide 255 evenly;
	 * otherwise we won't reach full opacity */

	/* now we have to blow out the windings into explicit edge lists */
	n = 0;
	for (i = 0; i < windings; ++i)
		n += wcount[i];

	e = malloc(sizeof(*e) * (n + 1)); /* inc sentinel */
	if (e == 0)
		return;
	n = 0;

	m = 0;
	for (i = 0; i < windings; ++i) {
		struct point *p = pts + m;
		m += wcount[i];
		j = wcount[i] - 1;
		for (k = 0; k < wcount[i]; j = k++) {
			int a = k, b = j;
			/* skip the edge if horizontal */
			if (p[j].y == p[k].y)
				continue;
			// add edge from j to k to the list
			e[n].invert = 0;
			if (invert ? p[j].y > p[k].y : p[j].y < p[k].y) {
				e[n].invert = 1;
				a = j, b = k;
			}
			e[n].x0 = p[a].x * scale_x + shift_x;
			e[n].y0 = (p[a].y * yscale_inv + shift_y) * vsubsample;
			e[n].x1 = p[b].x * scale_x + shift_x;
			e[n].y1 = (p[b].y * yscale_inv + shift_y) * vsubsample;
			++n;
		}
	}

	/* now sort the edges by their highest point (should snap to
	 * integer, and then by x) */
	sort_edges(e, n);

	/* now, traverse the scanlines and find the intersections on each
	 * scanline, use xor winding rule */
	rasterize_sorted_edges(result, e, n, vsubsample, off_x, off_y);

	free(e);
}

static void add_point(struct point *points, int n, float x, float y)
{
	if (!points)
		return;	/* during first pass, it's unallocated */
	points[n].x = x;
	points[n].y = y;
}

/* tesselate until threshhold p is happy...
 * @TODO warped to compensate for non-linear stretching */
static int tesselate_curve(struct point *points, int *num_points, float x0,
			   float y0, float x1, float y1, float x2, float y2,
			   float objspace_flatness_squared, int n)
{
	/* midpoint */
	float mx = (x0 + 2 * x1 + x2) / 4;
	float my = (y0 + 2 * y1 + y2) / 4;
	/* versus directly drawn line */
	float dx = (x0 + x2) / 2 - mx;
	float dy = (y0 + y2) / 2 - my;

	if (n > 16) /* 65536 segments on one curve better be enough! */
		return 1;
	/* half-pixel error allowed... need to be smaller if AA */
	if (dx * dx + dy * dy > objspace_flatness_squared) {
		tesselate_curve(points, num_points, x0, y0, (x0 + x1) / 2.0f,
				(y0 + y1) / 2.0f, mx, my,
				objspace_flatness_squared, n + 1);
		tesselate_curve(points, num_points, mx, my, (x1 + x2) / 2.0f,
				(y1 + y2) / 2.0f, x2, y2,
				objspace_flatness_squared, n + 1);
	} else {
		add_point(points, *num_points, x2, y2);
		*num_points = *num_points + 1;
	}
	return 1;
}

/* returns number of contours */
static struct point *flatten(struct vertex *vertices, int num_verts,
			     float objspace_flatness, int **contour_lengths,
			     int *num_contours)
{
	struct point *points = 0;
	int num_points = 0;
	float objspace_flatness_pow2 = objspace_flatness * objspace_flatness;
	int i, n = 0, start = 0, pass;

	/* count how many "moves" there are to get the contour count */
	for (i = 0; i < num_verts; ++i)
		if (vertices[i].type == VMOVE)
			++n;
	*num_contours = n;
	if (n == 0)
		return 0;

	*contour_lengths = malloc(sizeof(**contour_lengths) * n);
	if (*contour_lengths == 0) {
		*num_contours = 0;
		return 0;
	}
	/* make two passes through the points so we don't need to realloc */
	for (pass = 0; pass < 2; ++pass) {
		float x = 0, y = 0;
		if (pass == 1) {
			points = malloc(num_points * sizeof(points[0]));
			if (points == NULL)
				goto error;
		}
		num_points = 0;
		n = -1;
		for (i = 0; i < num_verts; ++i) {
			switch (vertices[i].type) {
				case VMOVE:
					// start the next contour
					if (n >= 0)
						(*contour_lengths)[n] = num_points - start;
					++n;
					start = num_points;

					x = vertices[i].x, y = vertices[i].y;
					add_point(points, num_points++, x, y);
					break;
				case VLINE:
					x = vertices[i].x, y = vertices[i].y;
					add_point(points, num_points++, x, y);
					break;
				case VCURVE:
					tesselate_curve(points, &num_points,
							x, y, vertices[i].cx,
							vertices[i].cy,
							vertices[i].x,
							vertices[i].y,
							objspace_flatness_pow2,
							0);
					x = vertices[i].x, y = vertices[i].y;
					break;
			}
		}
		(*contour_lengths)[n] = num_points - start;
	}

	return points;
 error:
	free(points);
	free(*contour_lengths);
	*contour_lengths = 0;
	*num_contours = 0;
	return NULL;
}

static void render(struct bitmap *result, float flatness_in_pixels,
		   struct vertex * vertices, int num_verts,
		   float scale_x, float scale_y, float shift_x,
		   float shift_y, int x_off, int y_off, int invert)
{
	float scale = scale_x > scale_y ? scale_y : scale_x;
	int winding_count, *winding_lengths;
	struct point *windings = flatten(vertices, num_verts,
					 flatness_in_pixels / scale,
					 &winding_lengths, &winding_count);
	if (windings) {
		rasterize(result, windings, winding_lengths, winding_count,
			  scale_x, scale_y, shift_x, shift_y, x_off, y_off,
			  invert);
		free(winding_lengths);
		free(windings);
	}
}

static short get_bearing(struct font *f, int glyph)
{
	if (glyph < f->num_metrics)
		return read_short(f->data + f->hmtx + 4 * glyph + 2);
	else
		return read_short(f->data + f->hmtx + 4 * f->num_metrics
				  + 2 * (glyph - f->num_metrics));
}

static void flip(struct node *node)
{
	node->red = true;
	node->link[0]->red = false;
	node->link[1]->red = false;
}

static struct node *rotate(struct node *node, int dir)
{
	struct node *tmp = node->link[!dir];

	node->link[!dir] = tmp->link[dir];
	tmp->link[dir] = node;

	tmp->red = false;
	node->red = true;;

	return tmp;
}

static struct node *rotate2(struct node *node, int dir)
{
	node->link[!dir] = rotate(node->link[!dir], !dir);

	return rotate(node, dir);
}

static struct node *new_entry(uint32_t ch, bool red, unsigned char *bitmap)
{
	struct node *n;

	n = malloc(sizeof *n);
	n->link[0] = n->link[1] = &leaf;
	n->red = red;
	n->ch = ch;
	n->bitmap = bitmap;
	return n;
}

static void cache(struct node **root, uint32_t ch, unsigned char *bitmap)
{
	struct node fake = { false, { NULL, *root }, 0, NULL };
	struct node *i = *root;
	struct node *p, *g, *gg;
	int dir = 0, last;

	/* Initializing `last` is not actually necessary, but it stops the
	 * compiler crying */
	last = 0;

	if (*root == &leaf) {
		*root = new_entry(ch, false, bitmap);
		return;
	}

	gg = &fake;
	p = g = &leaf;

	for (; ; ) {
		if (i == &leaf)
			p->link[dir] = i = new_entry(ch, true, bitmap);
		else if (i->link[0]->red && i->link[1]->red)
			flip(i);

		if (i->red && p->red) {
			int d = gg->link[1] == g;

			if (i == p->link[last])
				gg->link[d] = rotate(g, !last);
			else
				gg->link[d] = rotate2(g, !last);
		}

		if (ch == i->ch)
			break;

		last = dir;
		dir = i->ch < ch;

		if (g != &leaf)
			gg = g;
		g = p;
		p = i;
		i = i->link[dir];
	}
	*root = fake.link[1];
	(*root)->red = false;
}

static void delete_cache(struct node *n)
{
	if (n == &leaf)
		return;

	delete_cache(n->link[0]);
	delete_cache(n->link[1]);

	free(n->bitmap);
	free(n);
}

static unsigned char *lookup(struct node *n, uint32_t ch)
{
	while (n) {
		if (n->ch == ch) {
			return n->bitmap;
		}
		n = n->link[n->ch < ch];
	}

	return NULL;
}

unsigned char *new_glyph(uint32_t id, uint32_t c, int cwidth)
{
	struct vertex *vertices;
	int xmin, ymin;
	int glyph = find_index(&font, c);
	float leftb = get_bearing(&font, glyph) * font.scale;
	int vcount = glyph_shape(&font, glyph, &vertices);
	struct bitmap bm = {
		font.width * cwidth,
		font.height,
		font.width * cwidth,
		NULL
	};

	bm.pixels = calloc(1, bm.w * bm.h);

	get_glyph_origin(&font, glyph, &xmin, &ymin);
	render(&bm, 0.35f, vertices, vcount, font.scale, font.scale,
	       leftb, font.ascent + ymin, xmin, ymin, 1);

	free(vertices);

	cache(&font.cache, id, bm.pixels);
	return bm.pixels;
}

unsigned char *get_glyph(uint32_t id, uint32_t c, int cwidth)
{
	unsigned char *buf = lookup(font.cache, id);

	if (buf)
		return buf;
	else
		return new_glyph(id, c, cwidth);
}

static int get_width(struct font *f)
{
	int i = find_index(f, 'W');
	short advance;

	if (i < f->num_metrics) {
		advance = read_short(f->data + f->hmtx + 4 * i);
	} else {
		advance = read_short(f->data + f->hmtx);
	}

	return advance;
}

static void open_font(char *path)
{
	int fd;
	struct stat st;

	if (path == NULL || *path == '\0')
		goto fb;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "could not open font file: %s\n",
			strerror(errno));
		goto err;
	}

	if (fstat(fd, &st) < 0) {
		fprintf(stderr, "could not fstat font file: %s\n",
			strerror(errno));
		close(fd);
		goto err;
	}

	font.size = st.st_size;
	font.data = mmap(NULL, font.size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (font.data == MAP_FAILED) {
		fprintf(stderr, "could not mmap font file: %s\n",
			strerror(errno));
		goto err;
	}
	font.mmapped = true;
	return;

err:
	fprintf(stderr, "using fallback font\n");
fb:
	font.size = sizeof(fallback);
	font.data = &fallback[0];
	font.mmapped = false;
}

static void close_font(void)
{
	if (font.mmapped)
		munmap(font.data, font.size);
}

int font_init(int size, char *path, int *w, int *h)
{
	int descent, linegap;

	open_font(path);

	if (setup(&font, 0) < 0) {
		close_font();
		return -1;
	}

	font.num_metrics = read_ushort(font.data + font.hhea + 34);
	font.cache = &leaf;

	font.ascent = get_ascent(&font);
	descent = get_descent(&font);
	linegap = get_linegap(&font);
	font.scale = (float)size / (font.ascent - descent);

	font.height = font.ascent - descent + linegap;
	font.width = get_width(&font);

	font.ascent = floor(font.scale * font.ascent);
	font.width = ceil(font.scale * font.width);
	font.height = ceil(font.scale * font.height);

	*w = font.width;
	*h = font.height;

	return 0;
}

void font_deinit(void)
{
	delete_cache(font.cache);
	close_font();
}
