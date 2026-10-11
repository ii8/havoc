/* CFF/Type 2 outline reader, adapted from stb_truetype v1.26.
 * Sean Barrett, Dougall Johnson and contributors, public domain (Unlicense).
 * https://github.com/nothings/stb/blob/master/stb_truetype.h
 * Only outline decoding is included; rasterization stays in glyph.c.
 */

struct cff_buf {
	const uint8_t *data;
	int cursor, size;
	bool failed;
};

struct cff_font {
	struct cff_buf cff, charstrings, gsubrs, subrs, fontdicts, fdselect;
};

static uint8_t cff_buf_get8(struct cff_buf *b)
{
	if (b->cursor >= b->size) {
		b->failed = true;
		return 0;
	}
	return b->data[b->cursor++];
}

static uint8_t cff_buf_peek8(struct cff_buf *b)
{
	if (b->cursor >= b->size)
		return 0;
	return b->data[b->cursor];
}

static void cff_buf_seek(struct cff_buf *b, int64_t o)
{
	if (o > b->size || o < 0) {
		b->failed = true;
		b->cursor = b->size;
	} else b->cursor = o;
}

static void cff_buf_skip(struct cff_buf *b, int64_t o)
{
	cff_buf_seek(b, (int64_t)b->cursor + o);
}

static uint32_t cff_buf_get(struct cff_buf *b, int n)
{
	uint32_t v = 0;
	int i;
	if (n < 1 || n > 4) { b->failed = true; return 0; }
	for (i = 0; i < n; i++)
		v = (v << 8) | cff_buf_get8(b);
	return v;
}

static struct cff_buf cff_new_buf(const void *p, size_t size)
{
	struct cff_buf r;
	if (size >= 0x40000000) size = 0;
	r.data = p;
	r.size = (int) size;
	r.cursor = 0;
	r.failed = false;
	return r;
}

#define cff_buf_get16(b)  cff_buf_get((b), 2)
#define cff_buf_get32(b)  cff_buf_get((b), 4)

static struct cff_buf cff_buf_range(const struct cff_buf *b, int64_t o, int64_t s)
{
	struct cff_buf r = cff_new_buf(NULL, 0);
	if (o < 0 || s < 0 || o > b->size || s > b->size - o || b->failed) {
		r.failed = true; return r;
	}
	r.data = b->data + o;
	r.size = s;
	return r;
}

static struct cff_buf cff_get_index(struct cff_buf *b)
{
	int count, start, offsize;
	start = b->cursor;
	count = cff_buf_get16(b);
	if (count) {
		offsize = cff_buf_get8(b);
		if (offsize < 1 || offsize > 4) {
			b->failed = true;
			return cff_new_buf(NULL, 0);
		}
		cff_buf_skip(b, offsize * count);
		cff_buf_skip(b, (int64_t)cff_buf_get(b, offsize) - 1);
	}
	return cff_buf_range(b, start, b->cursor - start);
}

static uint32_t cff_int(struct cff_buf *b)
{
	int b0 = cff_buf_get8(b);
	if (b0 >= 32 && b0 <= 246)       return b0 - 139;
	else if (b0 >= 247 && b0 <= 250) return (b0 - 247)*256 + cff_buf_get8(b) + 108;
	else if (b0 >= 251 && b0 <= 254) return -(b0 - 251)*256 - cff_buf_get8(b) - 108;
	else if (b0 == 28)               return cff_buf_get16(b);
	else if (b0 == 29)               return cff_buf_get32(b);
	b->failed = true;
	return 0;
}

static void cff_skip_operand(struct cff_buf *b) {
	int v, b0 = cff_buf_peek8(b);
	assert(b0 >= 28);
	if (b0 == 30) {
		cff_buf_skip(b, 1);
		while (b->cursor < b->size) {
			v = cff_buf_get8(b);
			if ((v & 0xF) == 0xF || (v >> 4) == 0xF)
				break;
		}
	} else {
		cff_int(b);
	}
}

static struct cff_buf cff_dict_get(struct cff_buf *b, int key)
{
	cff_buf_seek(b, 0);
	while (!b->failed && b->cursor < b->size) {
		int start = b->cursor, end, op;
		while (!b->failed && cff_buf_peek8(b) >= 28)
			cff_skip_operand(b);
		end = b->cursor;
		op = cff_buf_get8(b);
		if (op == 12)  op = cff_buf_get8(b) | 0x100;
		if (op == key) return cff_buf_range(b, start, end-start);
	}
	return cff_buf_range(b, 0, 0);
}

static void cff_dict_get_ints(struct cff_buf *b, int key, int outcount, uint32_t *out)
{
	int i;
	struct cff_buf operands = cff_dict_get(b, key);
	for (i = 0; i < outcount && operands.cursor < operands.size; i++)
		out[i] = cff_int(&operands);
}

static int cff_index_count(struct cff_buf *b)
{
	cff_buf_seek(b, 0);
	return cff_buf_get16(b);
}

static struct cff_buf cff_index_get(struct cff_buf b, int i)
{
	int count, offsize;
	uint32_t start, end;
	cff_buf_seek(&b, 0);
	count = cff_buf_get16(&b);
	offsize = cff_buf_get8(&b);
	if (i < 0 || i >= count || offsize < 1 || offsize > 4)
		return cff_new_buf(NULL, 0);
	cff_buf_skip(&b, i*offsize);
	start = cff_buf_get(&b, offsize);
	end = cff_buf_get(&b, offsize);
	return cff_buf_range(&b, 2 + (int64_t)(count + 1) * offsize + start,
	                     (int64_t)end - start);
}

static struct cff_buf cff_get_subrs(struct cff_buf cff, struct cff_buf fontdict)
{
	uint32_t subrsoff = 0, private_loc[2] = { 0, 0 };
	struct cff_buf pdict;
	cff_dict_get_ints(&fontdict, 18, 2, private_loc);
	if (!private_loc[1] || !private_loc[0]) return cff_new_buf(NULL, 0);
	pdict = cff_buf_range(&cff, private_loc[1], private_loc[0]);
	cff_dict_get_ints(&pdict, 19, 1, &subrsoff);
	if (!subrsoff) return cff_new_buf(NULL, 0);
	cff_buf_seek(&cff, (int64_t)private_loc[1] + subrsoff);
	return cff_get_index(&cff);
}

static int cff_init(struct cff_font *info, const void *data, size_t size)
{
	struct cff_buf b, topdict, topdictidx;
	uint32_t cstype = 2, charstrings = 0, fdarrayoff = 0, fdselectoff = 0;

	info->fontdicts = cff_new_buf(NULL, 0);
	info->fdselect = cff_new_buf(NULL, 0);

	info->cff = cff_new_buf(data, size);
	b = info->cff;

	if (size < 4 || cff_buf_get8(&b) != 1) return 0;
	cff_buf_seek(&b, 0);

	// read the header
	cff_buf_skip(&b, 2);
	cff_buf_seek(&b, cff_buf_get8(&b)); // hdrsize

	/* OpenType CFF tables contain one font. */
	cff_get_index(&b);  // name INDEX
	topdictidx = cff_get_index(&b);
	topdict = cff_index_get(topdictidx, 0);
	cff_get_index(&b);  // string INDEX
	info->gsubrs = cff_get_index(&b);

	cff_dict_get_ints(&topdict, 17, 1, &charstrings);
	cff_dict_get_ints(&topdict, 0x100 | 6, 1, &cstype);
	cff_dict_get_ints(&topdict, 0x100 | 36, 1, &fdarrayoff);
	cff_dict_get_ints(&topdict, 0x100 | 37, 1, &fdselectoff);
	info->subrs = cff_get_subrs(b, topdict);

	// we only support Type 2 charstrings
	if (cstype != 2) return 0;
	if (charstrings == 0) return 0;

	if (fdarrayoff) {
		// looks like a CID font
		if (!fdselectoff) return 0;
		cff_buf_seek(&b, fdarrayoff);
		info->fontdicts = cff_get_index(&b);
		info->fdselect = cff_buf_range(&b, fdselectoff, b.size-fdselectoff);
	}

	cff_buf_seek(&b, charstrings);
	info->charstrings = cff_get_index(&b);
	return !b.failed && info->charstrings.size != 0;
}
typedef struct
{
	int bounds;
	int started;
	float first_x, first_y;
	float x, y;
	int32_t min_x, max_x, min_y, max_y;

	struct vertex *pvertices;
	int num_vertices;
} cff_context;

#define CFF_CTX_INIT(bounds) {bounds,0, 0,0, 0,0, 0,0,0,0, NULL, 0}

static void cff_track_vertex(cff_context *c, int32_t x, int32_t y)
{
	if (x > c->max_x || !c->started) c->max_x = x;
	if (y > c->max_y || !c->started) c->max_y = y;
	if (x < c->min_x || !c->started) c->min_x = x;
	if (y < c->min_y || !c->started) c->min_y = y;
	c->started = 1;
}

static void cff_csctx_v(cff_context *c, uint8_t type,
                        int32_t x, int32_t y, int32_t cx, int32_t cy,
                        int32_t cx1, int32_t cy1)
{
	if (c->bounds) {
		cff_track_vertex(c, x, y);
		if (type == VCUBIC) {
			cff_track_vertex(c, cx, cy);
			cff_track_vertex(c, cx1, cy1);
		}
	} else {
		vinit(&c->pvertices[c->num_vertices], type, x, y, cx, cy);
		c->pvertices[c->num_vertices].cx1 = (int16_t) cx1;
		c->pvertices[c->num_vertices].cy1 = (int16_t) cy1;
	}
	c->num_vertices++;
}

static void cff_csctx_close_shape(cff_context *ctx)
{
	if (ctx->first_x != ctx->x || ctx->first_y != ctx->y)
		cff_csctx_v(ctx, VLINE, (int)ctx->first_x, (int)ctx->first_y, 0, 0, 0, 0);
}

static void cff_csctx_rmove_to(cff_context *ctx, float dx, float dy)
{
	cff_csctx_close_shape(ctx);
	ctx->first_x = ctx->x = ctx->x + dx;
	ctx->first_y = ctx->y = ctx->y + dy;
	cff_csctx_v(ctx, VMOVE, (int)ctx->x, (int)ctx->y, 0, 0, 0, 0);
}

static void cff_csctx_rline_to(cff_context *ctx, float dx, float dy)
{
	ctx->x += dx;
	ctx->y += dy;
	cff_csctx_v(ctx, VLINE, (int)ctx->x, (int)ctx->y, 0, 0, 0, 0);
}

static void cff_csctx_rccurve_to(cff_context *ctx, float dx1, float dy1,
                                float dx2, float dy2, float dx3, float dy3)
{
	float cx1 = ctx->x + dx1;
	float cy1 = ctx->y + dy1;
	float cx2 = cx1 + dx2;
	float cy2 = cy1 + dy2;
	ctx->x = cx2 + dx3;
	ctx->y = cy2 + dy3;
	cff_csctx_v(ctx, VCUBIC, (int)ctx->x, (int)ctx->y, (int)cx1, (int)cy1, (int)cx2, (int)cy2);
}

static struct cff_buf cff_get_subr(struct cff_buf idx, int n)
{
	int count = cff_index_count(&idx);
	int bias = 107;
	if (count >= 33900)
		bias = 32768;
	else if (count >= 1240)
		bias = 1131;
	n += bias;
	if (n < 0 || n >= count)
		return cff_new_buf(NULL, 0);
	return cff_index_get(idx, n);
}

static struct cff_buf cff_cid_get_glyph_subrs(const struct cff_font *info, int glyph_index)
{
	struct cff_buf fdselect = info->fdselect;
	int nranges, start, end, v, fmt, fdselector = -1, i;

	cff_buf_seek(&fdselect, 0);
	fmt = cff_buf_get8(&fdselect);
	if (fmt == 0) {
				cff_buf_skip(&fdselect, glyph_index);
		fdselector = cff_buf_get8(&fdselect);
	} else if (fmt == 3) {
		nranges = cff_buf_get16(&fdselect);
		start = cff_buf_get16(&fdselect);
		for (i = 0; i < nranges; i++) {
			v = cff_buf_get8(&fdselect);
			end = cff_buf_get16(&fdselect);
			if (glyph_index >= start && glyph_index < end) {
				fdselector = v;
				break;
			}
			start = end;
		}
	}
	if (fdselect.failed || fdselector == -1) return cff_new_buf(NULL, 0);
	return cff_get_subrs(info->cff, cff_index_get(info->fontdicts, fdselector));
}

static int cff_run_charstring(const struct cff_font *info, int glyph_index, cff_context *c)
{
	int in_header = 1, maskbits = 0, subr_stack_height = 0, sp = 0, v, i, b0;
	int has_subrs = 0, clear_stack;
	float s[48];
	struct cff_buf subr_stack[10], subrs = info->subrs, b;
	float f;
	unsigned steps = 0;

#define CFF_ERROR(s) (0)

	// this currently ignores the initial width value, which isn't needed if we have hmtx
	b = cff_index_get(info->charstrings, glyph_index);
	while (!b.failed && b.cursor < b.size) {
		/* Bound cyclic subroutines and excessive outlines. */
		if (++steps > 100000 || c->num_vertices > 65536) return 0;
		i = 0;
		clear_stack = 1;
		b0 = cff_buf_get8(&b);
		switch (b0) {
		case 0x13: // hintmask
		case 0x14: // cntrmask
			if (in_header)
				maskbits += (sp / 2); // implicit "vstem"
			in_header = 0;
			cff_buf_skip(&b, (maskbits + 7) / 8);
			break;

		case 0x01: // hstem
		case 0x03: // vstem
		case 0x12: // hstemhm
		case 0x17: // vstemhm
			maskbits += (sp / 2);
			break;

		case 0x15: // rmoveto
			in_header = 0;
			if (sp < 2) return CFF_ERROR("rmoveto stack");
			cff_csctx_rmove_to(c, s[sp-2], s[sp-1]);
			break;
		case 0x04: // vmoveto
			in_header = 0;
			if (sp < 1) return CFF_ERROR("vmoveto stack");
			cff_csctx_rmove_to(c, 0, s[sp-1]);
			break;
		case 0x16: // hmoveto
			in_header = 0;
			if (sp < 1) return CFF_ERROR("hmoveto stack");
			cff_csctx_rmove_to(c, s[sp-1], 0);
			break;

		case 0x05: // rlineto
			if (sp < 2) return CFF_ERROR("rlineto stack");
			for (; i + 1 < sp; i += 2)
				cff_csctx_rline_to(c, s[i], s[i+1]);
			break;

		// hlineto/vlineto and vhcurveto/hvcurveto alternate horizontal and vertical
		// starting from a different place.

		case 0x07: // vlineto
			if (sp < 1) return CFF_ERROR("vlineto stack");
			goto vlineto;
		case 0x06: // hlineto
			if (sp < 1) return CFF_ERROR("hlineto stack");
			for (;;) {
				if (i >= sp) break;
				cff_csctx_rline_to(c, s[i], 0);
				i++;
		vlineto:
				if (i >= sp) break;
				cff_csctx_rline_to(c, 0, s[i]);
				i++;
			}
			break;

		case 0x1F: // hvcurveto
			if (sp < 4) return CFF_ERROR("hvcurveto stack");
			goto hvcurveto;
		case 0x1E: // vhcurveto
			if (sp < 4) return CFF_ERROR("vhcurveto stack");
			for (;;) {
				if (i + 3 >= sp) break;
				cff_csctx_rccurve_to(c, 0, s[i], s[i+1], s[i+2], s[i+3], (sp - i == 5) ? s[i + 4] : 0.0f);
				i += 4;
		hvcurveto:
				if (i + 3 >= sp) break;
				cff_csctx_rccurve_to(c, s[i], 0, s[i+1], s[i+2], (sp - i == 5) ? s[i+4] : 0.0f, s[i+3]);
				i += 4;
			}
			break;

		case 0x08: // rrcurveto
			if (sp < 6) return CFF_ERROR("rcurveline stack");
			for (; i + 5 < sp; i += 6)
				cff_csctx_rccurve_to(c, s[i], s[i+1], s[i+2], s[i+3], s[i+4], s[i+5]);
			break;

		case 0x18: // rcurveline
			if (sp < 8) return CFF_ERROR("rcurveline stack");
			for (; i + 5 < sp - 2; i += 6)
				cff_csctx_rccurve_to(c, s[i], s[i+1], s[i+2], s[i+3], s[i+4], s[i+5]);
			if (i + 1 >= sp) return CFF_ERROR("rcurveline stack");
			cff_csctx_rline_to(c, s[i], s[i+1]);
			break;

		case 0x19: // rlinecurve
			if (sp < 8) return CFF_ERROR("rlinecurve stack");
			for (; i + 1 < sp - 6; i += 2)
				cff_csctx_rline_to(c, s[i], s[i+1]);
			if (i + 5 >= sp) return CFF_ERROR("rlinecurve stack");
			cff_csctx_rccurve_to(c, s[i], s[i+1], s[i+2], s[i+3], s[i+4], s[i+5]);
			break;

		case 0x1A: // vvcurveto
		case 0x1B: // hhcurveto
			if (sp < 4) return CFF_ERROR("(vv|hh)curveto stack");
			f = 0.0;
			if (sp & 1) { f = s[i]; i++; }
			for (; i + 3 < sp; i += 4) {
				if (b0 == 0x1B)
					cff_csctx_rccurve_to(c, s[i], f, s[i+1], s[i+2], s[i+3], 0.0);
				else
					cff_csctx_rccurve_to(c, f, s[i], s[i+1], s[i+2], 0.0, s[i+3]);
				f = 0.0;
			}
			break;

		case 0x0A: // callsubr
			if (!has_subrs) {
				if (info->fdselect.size)
					subrs = cff_cid_get_glyph_subrs(info, glyph_index);
				has_subrs = 1;
			}
			// FALLTHROUGH
		case 0x1D: // callgsubr
			if (sp < 1) return CFF_ERROR("call(g|)subr stack");
			v = (int) s[--sp];
			if (subr_stack_height >= 10) return CFF_ERROR("recursion limit");
			subr_stack[subr_stack_height++] = b;
			b = cff_get_subr(b0 == 0x0A ? subrs : info->gsubrs, v);
			if (b.size == 0) return CFF_ERROR("subr not found");
			b.cursor = 0;
			clear_stack = 0;
			break;

		case 0x0B: // return
			if (subr_stack_height <= 0) return CFF_ERROR("return outside subr");
			b = subr_stack[--subr_stack_height];
			clear_stack = 0;
			break;

		case 0x0E: // endchar
			cff_csctx_close_shape(c);
			return !b.failed;

		case 0x0C: { // two-byte escape
			float dx1, dx2, dx3, dx4, dx5, dx6, dy1, dy2, dy3, dy4, dy5, dy6;
			float dx, dy;
			int b1 = cff_buf_get8(&b);
			switch (b1) {
			// @TODO These "flex" implementations ignore the flex-depth and resolution,
			// and always draw beziers.
			case 0x22: // hflex
				if (sp < 7) return CFF_ERROR("hflex stack");
				dx1 = s[0];
				dx2 = s[1];
				dy2 = s[2];
				dx3 = s[3];
				dx4 = s[4];
				dx5 = s[5];
				dx6 = s[6];
				cff_csctx_rccurve_to(c, dx1, 0, dx2, dy2, dx3, 0);
				cff_csctx_rccurve_to(c, dx4, 0, dx5, -dy2, dx6, 0);
				break;

			case 0x23: // flex
				if (sp < 13) return CFF_ERROR("flex stack");
				dx1 = s[0];
				dy1 = s[1];
				dx2 = s[2];
				dy2 = s[3];
				dx3 = s[4];
				dy3 = s[5];
				dx4 = s[6];
				dy4 = s[7];
				dx5 = s[8];
				dy5 = s[9];
				dx6 = s[10];
				dy6 = s[11];
				//fd is s[12]
				cff_csctx_rccurve_to(c, dx1, dy1, dx2, dy2, dx3, dy3);
				cff_csctx_rccurve_to(c, dx4, dy4, dx5, dy5, dx6, dy6);
				break;

			case 0x24: // hflex1
				if (sp < 9) return CFF_ERROR("hflex1 stack");
				dx1 = s[0];
				dy1 = s[1];
				dx2 = s[2];
				dy2 = s[3];
				dx3 = s[4];
				dx4 = s[5];
				dx5 = s[6];
				dy5 = s[7];
				dx6 = s[8];
				cff_csctx_rccurve_to(c, dx1, dy1, dx2, dy2, dx3, 0);
				cff_csctx_rccurve_to(c, dx4, 0, dx5, dy5, dx6, -(dy1+dy2+dy5));
				break;

			case 0x25: // flex1
				if (sp < 11) return CFF_ERROR("flex1 stack");
				dx1 = s[0];
				dy1 = s[1];
				dx2 = s[2];
				dy2 = s[3];
				dx3 = s[4];
				dy3 = s[5];
				dx4 = s[6];
				dy4 = s[7];
				dx5 = s[8];
				dy5 = s[9];
				dx6 = dy6 = s[10];
				dx = dx1+dx2+dx3+dx4+dx5;
				dy = dy1+dy2+dy3+dy4+dy5;
				if (fabs(dx) > fabs(dy))
					dy6 = -dy;
				else
					dx6 = -dx;
				cff_csctx_rccurve_to(c, dx1, dy1, dx2, dy2, dx3, dy3);
				cff_csctx_rccurve_to(c, dx4, dy4, dx5, dy5, dx6, dy6);
				break;

			default:
				return CFF_ERROR("unimplemented");
			}
		} break;

		default:
			if (b0 != 255 && b0 != 28 && b0 < 32)
				return CFF_ERROR("reserved operator");

			// push immediate
			if (b0 == 255) {
				f = (float)(int32_t)cff_buf_get32(&b) / 0x10000;
			} else {
				cff_buf_skip(&b, -1);
				f = (float)(int16_t)cff_int(&b);
			}
			if (sp >= 48) return CFF_ERROR("push stack overflow");
			s[sp++] = f;
			clear_stack = 0;
			break;
		}
		if (clear_stack) sp = 0;
	}
	return CFF_ERROR("no endchar");

#undef CFF_ERROR
}

static int cff_shape(const struct cff_font *info, int glyph_index,
                     struct vertex **pvertices)
{
	// runs the charstring twice, once to count and once to output (to avoid realloc)
	cff_context count_ctx = CFF_CTX_INIT(1);
	cff_context output_ctx = CFF_CTX_INIT(0);
	*pvertices = NULL;
	if (cff_run_charstring(info, glyph_index, &count_ctx)) {
		*pvertices = malloc(count_ctx.num_vertices * sizeof(struct vertex));
		if (!*pvertices) return 0;
		output_ctx.pvertices = *pvertices;
		if (cff_run_charstring(info, glyph_index, &output_ctx)) {
			assert(output_ctx.num_vertices == count_ctx.num_vertices);
			return output_ctx.num_vertices;
		}
	}
	free(*pvertices);
	*pvertices = NULL;
	return 0;
}

static int cff_bounds(const struct cff_font *info, int glyph_index,
                      int *x0, int *y0, int *x1, int *y1)
{
	cff_context c = CFF_CTX_INIT(1);
	int r = cff_run_charstring(info, glyph_index, &c);
	if (x0)  *x0 = r ? c.min_x : 0;
	if (y0)  *y0 = r ? c.min_y : 0;
	if (x1)  *x1 = r ? c.max_x : 0;
	if (y1)  *y1 = r ? c.max_y : 0;
	return r ? c.num_vertices : 0;
}
