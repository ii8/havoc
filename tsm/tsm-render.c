/*
 * libtsm - Rendering
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Rendering
 * TSM does not depend on any graphics system or rendering libraries. Instead,
 * it provides iterators and ageing support so you can implement renderers
 * yourself.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "shl-llog.h"

#define LLOG_SUBSYSTEM "tsm-render"

static void to_rgb(struct tsm_screen *con, struct tsm_screen_attr *attr)
{
	int16_t code;

	code = attr->fccode;
	switch (code) {
	case TSM_COLOR_RGB:
		break;
	case TSM_COLOR_FOREGROUND:
		attr->fr = con->foreground[0];
		attr->fg = con->foreground[1];
		attr->fb = con->foreground[2];
		break;
	case TSM_COLOR_BACKGROUND:
		attr->fr = con->background[0];
		attr->fg = con->background[1];
		attr->fb = con->background[2];
		break;
	default:
		/* bold causes light colors */
		if (attr->bold && code < 8)
			code += 8;

		attr->fr = con->palette[code][0];
		attr->fg = con->palette[code][1];
		attr->fb = con->palette[code][2];
	}

	code = attr->bccode;
	switch (code) {
	case TSM_COLOR_RGB:
		break;
	case TSM_COLOR_FOREGROUND:
		attr->br = con->foreground[0];
		attr->bg = con->foreground[1];
		attr->bb = con->foreground[2];
		break;
	case TSM_COLOR_BACKGROUND:
		attr->br = con->background[0];
		attr->bg = con->background[1];
		attr->bb = con->background[2];
		break;
	default:
		attr->br = con->palette[code][0];
		attr->bg = con->palette[code][1];
		attr->bb = con->palette[code][2];
	}
}

bool screen_snapshot(struct tsm_screen *con)
{
	struct screen_snapshot *snapshot = &con->snapshot;
	struct cell *cells;
	size_t rows = con->size_y, cols = con->size_x;
	int x, y;
	int cur_x = con->cursor_x < con->size_x ? con->cursor_x : con->size_x - 1;
	int cur_y = con->cursor_y < con->size_y ? con->cursor_y : con->size_y - 1;

	if (snapshot->active)
		return true;
	if (con->size_x <= 0 || con->size_y <= 0 ||
	    rows > SIZE_MAX / sizeof(*snapshot->lines) ||
	    rows > SIZE_MAX / sizeof(*snapshot->view_lines) ||
	    cols > SIZE_MAX / rows / sizeof(*snapshot->cells))
		return false;
	/* Reuse the buffers until the grid is resized. */
	if (!snapshot->view_lines) {
		snapshot->lines = malloc(rows * sizeof(*snapshot->lines));
		snapshot->cells = malloc(rows * cols * sizeof(*snapshot->cells));
		snapshot->view_lines = malloc(rows * sizeof(*snapshot->view_lines));
		if (!snapshot->lines || !snapshot->cells || !snapshot->view_lines) {
			screen_free_snapshot(con);
			return false;
		}
	}
	snapshot->age = con->age_cnt;
	memcpy(snapshot->background, con->background, sizeof(snapshot->background));
	cells = snapshot->cells;
	for (y = 0; y < con->size_y; ++y) {
		snapshot->lines[y] = (struct line) {
			.size = con->size_x, .cells = cells, .age = con->lines[y]->age
		};
		snapshot->view_lines[y] = &snapshot->lines[y];
		memcpy(cells, con->lines[y]->cells, con->size_x * sizeof(*cells));
		/* Freeze colors and cursor; selection is applied when drawing. */
		for (x = 0; x < con->size_x; ++x) {
			to_rgb(con, &cells[x].attr);
			cells[x].attr.fccode = cells[x].attr.bccode = TSM_COLOR_RGB;
			if (con->flags & TSM_SCREEN_INVERSE)
				cells[x].attr.inverse = !cells[x].attr.inverse;
		}
		if (y == cur_y && !(con->flags & TSM_SCREEN_HIDE_CURSOR))
			cells[cur_x].attr.inverse = !cells[cur_x].attr.inverse;
		cells += con->size_x;
	}
	snapshot->active = true;
	con->view_lines = snapshot->view_lines;
	return true;
}

SHL_EXPORT
tsm_age_t tsm_screen_draw(struct tsm_screen *con, tsm_screen_draw_cb draw_cb,
			  void *data)
{
	int cur_x, cur_y;
	int i, j, k;
	struct line *iter, *line = NULL;
	struct cell *cell, empty;
	struct tsm_screen_attr attr;
	const uint32_t *ch;
	size_t len;
	bool in_sel = false, sel_start = false, sel_end = false;
	bool was_sel = false;
	bool frozen;
	tsm_age_t age;

	screen_cell_init(con, &empty, &con->def_attr);

	cur_x = con->cursor_x;
	if (con->cursor_x >= con->size_x)
		cur_x = con->size_x - 1;
	cur_y = con->cursor_y;
	if (con->cursor_y >= con->size_y)
		cur_y = con->size_y - 1;

	/* push each character into rendering pipeline */

	iter = con->sb_pos;
	k = 0;

	if (con->sel_active) {
		if (!con->sel_start.line && con->sel_start.y == SELECTION_TOP)
			in_sel = !in_sel;
		if (!con->sel_end.line && con->sel_end.y == SELECTION_TOP)
			in_sel = !in_sel;

		if (con->sel_start.line &&
		    (!iter || con->sel_start.line->sb_id < iter->sb_id))
			in_sel = !in_sel;
		if (con->sel_end.line &&
		    (!iter || con->sel_end.line->sb_id < iter->sb_id))
			in_sel = !in_sel;
	}

	for (i = 0; i < con->size_y; ++i) {
		frozen = !iter && con->snapshot.active;
		if (iter) {
			line = iter;
			iter = iter->next;
		} else {
			line = con->view_lines[k];
			k++;
		}

		if (con->sel_active) {
			if (con->sel_start.line == line ||
			    (!con->sel_start.line &&
			     con->sel_start.y == k - 1))
				sel_start = true;
			else
				sel_start = false;
			if (con->sel_end.line == line ||
			    (!con->sel_end.line &&
			     con->sel_end.y == k - 1))
				sel_end = true;
			else
				sel_end = false;

			was_sel = false;
		}

		for (j = 0; j < con->size_x; ++j) {
			if (j < line->size)
				cell = &line->cells[j];
			else
				cell = &empty;

			memcpy(&attr, &cell->attr, sizeof(attr));

			if (con->sel_active) {
				if (sel_start &&
				    j == con->sel_start.x) {
					was_sel = in_sel;
					in_sel = !in_sel;
				}
				if (sel_end &&
				    j == con->sel_end.x) {
					was_sel = in_sel;
					in_sel = !in_sel;
				}
			}

			if (!frozen && k == cur_y + 1 && j == cur_x &&
			    !(con->flags & TSM_SCREEN_HIDE_CURSOR))
				attr.inverse = !attr.inverse;

			/* TODO: do some more sophisticated inverse here. When
			 * INVERSE mode is set, we should instead just select
			 * inverse colors instead of switching background and
			 * foreground */
			if (!frozen && (con->flags & TSM_SCREEN_INVERSE))
				attr.inverse = !attr.inverse;

			if (in_sel || was_sel) {
				was_sel = false;
				attr.inverse = !attr.inverse;
			}

			if (con->age_reset) {
				age = 0;
			} else {
				age = cell->age;
				if (line->age > age)
					age = line->age;
				if (con->age > age)
					age = con->age;
			}

			ch = tsm_symbol_get(con->sym_table, &cell->ch, &len);
			if (cell->ch == 0 ||
			    cell->ch == ' ' ||
			    cell->ch == 0xA0)
				len = 0;
			to_rgb(con, &attr);
			draw_cb(con, cell->ch, ch, len, cell->width,
				j, i, &attr, age, data);
		}
	}

	if (con->age_reset) {
		con->age_reset = 0;
		return 0;
	} else {
		return con->age_cnt;
	}
}
