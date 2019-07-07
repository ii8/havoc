#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <ctype.h>

#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <pty.h>

#include <xkbcommon/xkbcommon-compose.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include "tsm/libtsm.h"
#include "xdg-shell.h"
#include "gtk-primary-selection.h"

typedef unsigned int uint;
typedef unsigned char uchr;

unsigned char *get_glyph(uint32_t, uint32_t, uint);
int font_init(int, char *, int *, int *);
void font_deinit(void);

static struct {
	bool die;
	bool configured;
	bool need_redraw;
	bool can_redraw;
	int resize;

	int fd, master_fd;

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *cp;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;

	struct wl_surface *surf;
	struct xdg_surface *xdgsurf;
	struct xdg_toplevel *toplvl;

	struct buffer {
		struct wl_buffer *b;
		void *data;
		int size;
		bool busy;
		tsm_age_t age;
	} buf[2];
	struct wl_callback *cb;

	int col, row;
	int cwidth, cheight;
	size_t width, height;
	size_t confheight, confwidth;
	struct {
		size_t top, left;
	} margin;

	struct tsm_screen *screen;
	struct tsm_vte *vte;
	enum tsm_vte_modifier mods;

	struct xkb_context *xkb_ctx;
	struct xkb_state *xkb_state;
	struct xkb_keymap *xkb_keymap;
	struct xkb_compose_table *xkb_compose_table;
	struct xkb_compose_state *xkb_compose_state;
	xkb_mod_index_t xkb_alt;
	xkb_mod_index_t xkb_ctrl;
	xkb_mod_index_t xkb_shift;

	struct wl_keyboard *kbd;
	struct wl_pointer *ptr;
	int select;

	struct {
		int fd;
		xkb_keysym_t sym;
		uint32_t unicode;
		struct itimerspec its;
	} repeat;

	wl_fixed_t ptr_x, ptr_y;

	bool has_argb;

	struct gtk_primary_selection_device_manager *ps_dm;
	struct gtk_primary_selection_device *ps_d;

	struct {
		struct gtk_primary_selection_source *source;
		char *data;
	} copy;

	struct {
		struct gtk_primary_selection_offer *offer;
		bool acceptable;

		int fd[2];
		char buf[200];
		size_t len;
		bool active;
	} paste;

	struct {
		bool linger;
		char *config;
		char *display;
	} opt;

	struct {
		char shell[32];
		int col, row;
		uint scrollback;
		bool margin;
		uchr opacity;
		int font_size;
		char font_path[512];
		uint8_t colors[TSM_COLOR_NUM][3];
	} cfg;
} term = {
	.cfg.shell = "/bin/sh",
	.cfg.col = 80,
	.cfg.row = 24,
	.cfg.scrollback = 0,
	.cfg.margin = false,
	.cfg.opacity = 0xff,
	.cfg.font_size = 18,
	.cfg.font_path = "",
	.cfg.colors = {
		[TSM_COLOR_BLACK]         = {   0,   0,   0 },
		[TSM_COLOR_RED]           = { 205,   0,   0 },
		[TSM_COLOR_GREEN]         = {   0, 205,   0 },
		[TSM_COLOR_YELLOW]        = { 205, 205,   0 },
		[TSM_COLOR_BLUE]          = {   0,   0, 238 },
		[TSM_COLOR_MAGENTA]       = { 205,   0, 205 },
		[TSM_COLOR_CYAN]          = {   0, 205, 205 },
		[TSM_COLOR_LIGHT_GREY]    = { 229, 229, 229 },
		[TSM_COLOR_DARK_GREY]     = { 127, 127, 127 },
		[TSM_COLOR_LIGHT_RED]     = { 255,   0,   0 },
		[TSM_COLOR_LIGHT_GREEN]   = {   0, 255,   0 },
		[TSM_COLOR_LIGHT_YELLOW]  = { 255, 255,   0 },
		[TSM_COLOR_LIGHT_BLUE]    = {  92,  92, 255 },
		[TSM_COLOR_LIGHT_MAGENTA] = { 255,   0, 255 },
		[TSM_COLOR_LIGHT_CYAN]    = {   0, 255, 255 },
		[TSM_COLOR_WHITE]         = { 255, 255, 255 },
		[TSM_COLOR_FOREGROUND]    = { 229, 229, 229 },
		[TSM_COLOR_BACKGROUND]    = {   0,   0,   0 },
	}
};

static const char *sev2str_table[] = {
	"FATAL",
	"ALERT",
	"CRITICAL",
	"ERROR",
	"WARNING",
	"NOTICE",
	"INFO",
	"DEBUG"
};

static const char *sev2str(uint sev)
{
	if (sev > 7)
		return "UNKNOWN";

	return sev2str_table[sev];
}

static void log_tsm(void *data,
		    const char *file,
		    int line,
		    const char *fn,
		    const char *subs,
		    uint sev,
		    const char *format,
		    va_list args)
{
	fprintf(stderr, "%s: %s: ", sev2str(sev), subs);
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
}

static void wcb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	if (term.master_fd >= 0 && write(term.master_fd, u8, len) < 0) {
		fprintf(stderr, "could not write to pty master: %m\n");
		abort();
	}
}

static void handle_display(int ev)
{
	if (ev & EPOLLHUP) {
		term.die = true;
	} else if (ev & EPOLLIN) {
		if (wl_display_dispatch(term.display) < 0) {
			fprintf(stderr, "could not dispatch events: %m\n");
			abort();
		}
	}
}

static void handle_tty(int ev)
{
	char data[256];
	int len;

	if (ev & EPOLLIN) {
		term.need_redraw = true;
		len = read(term.master_fd, data, sizeof(data));
		assert(len);
		if (len < 0) {
			fprintf(stderr, "could not read from pty: %m\n");
			abort();
		} else {
			tsm_vte_input(term.vte, data, len);
		}
	} else if (ev & EPOLLHUP) {
		epoll_ctl(term.fd, EPOLL_CTL_DEL, term.master_fd, NULL);
		close(term.master_fd);
		term.master_fd = -1;
		if (!term.opt.linger)
			term.die = true;
	}
}

static void handle_repeat(int ev)
{
	uint64_t exp;

	if (read(term.repeat.fd, &exp, sizeof exp) < 0)
		return;

	tsm_vte_handle_keyboard(term.vte, term.repeat.sym, 0, term.mods,
				term.repeat.unicode);
}

#define REPLACEMENT_CHAR 0x0000fffd

static int utf8_to_utf32(char const **utf8, size_t *len, uint32_t *r)
{
	static unsigned char const tail_len[128] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0
	};
	unsigned char tail, c = *(*utf8)++;

	--(*len);
	if (c < 0x80) {
		*r = c;
		return 0;
	}

	tail = tail_len[c - 0x80];

	if (!tail) {
		/* middle of a character or tail too long */
		*r = REPLACEMENT_CHAR;
		return 0;
	} else if (tail > *len) {
		/* need more input for a complete character */
		++(*len), --(*utf8);
		return 1;
	}

	/* remove length specification bits */
	*r = c & 0x3f >> tail;

	while (tail--) {
		c = *(*utf8)++;
		--(*len);
		if ((c & 0xc0) != 0x80) {
			*r = REPLACEMENT_CHAR;
			++(*len), --(*utf8);
			return 0;
		}
		*r = (*r << 6) + (c & 0x3f);
	}
	return 0;
}

static void end_paste(void)
{
	epoll_ctl(term.fd, EPOLL_CTL_DEL, term.paste.fd[0], NULL);
	close(term.paste.fd[0]);
	term.paste.len = 0;
	term.paste.active = false;
}

static void handle_paste(int ev)
{
	if (ev & EPOLLIN) {
		uint32_t code;
		ssize_t len;
		char const *p = &term.paste.buf[0];

		len = read(term.paste.fd[0],
			   term.paste.buf + term.paste.len,
			   sizeof(term.paste.buf) - term.paste.len);

		if (len <= 0) {
			end_paste();
			return;
		}

		term.need_redraw = true;
		term.paste.len += len;
		while (term.paste.len > 0) {
			if (utf8_to_utf32(&p, &term.paste.len, &code)) {
				memcpy(&term.paste.buf, p, term.paste.len);
				break;
			}
			tsm_vte_handle_keyboard(term.vte, XKB_KEY_NoSymbol,
						0, 0, code);
		}
	} else if (ev & EPOLLHUP) {
		end_paste();
	}
}

static struct epcb {
	void (*f)(int);
} dfp = { handle_display }
, tfp = { handle_tty }
, rfp = { handle_repeat }
, pfp = { handle_paste };

static void buf_release(void *data, struct wl_buffer *b)
{
	struct buffer *buffer = data;

	buffer->busy = false;
}

static const struct wl_buffer_listener buf_listener = {
	buf_release
};

static int new_buffer(struct buffer *buf)
{
	struct wl_shm_pool *pool;
	char shm_name[14];
	int fd, stride;
	int max = 100;

	assert(!buf->busy);
	if (buf->b) {
		wl_buffer_destroy(buf->b);
		munmap(buf->data, buf->size);
	}

	stride = term.width * 4;
	buf->size = stride * term.height;

	srand(time(NULL));
	do {
		errno = 0;
		sprintf(shm_name, "/havoc-%d", rand() % 1000000);
		fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
	} while (errno == EEXIST && --max);

	if (fd < 0) {
		fprintf(stderr, "shm_open failed: %s\n", strerror(errno));
		return -1;
	}

	if (ftruncate(fd, buf->size) < 0) {
		fprintf(stderr, "ftruncate failed: %s\n", strerror(errno));
		return -1;
	}

	buf->data = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			 fd, 0);
	shm_unlink(shm_name);

	if (buf->data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	pool = wl_shm_create_pool(term.shm, fd, buf->size);
	buf->b = wl_shm_pool_create_buffer(pool, 0, term.width, term.height,
					   stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buf->b, &buf_listener, buf);
	wl_shm_pool_destroy(pool);
	close(fd);

	buf->age = 0;

	return 0;
}

static struct buffer *swap_buffers(void)
{
	struct buffer *buf;

	assert(term.configured);

	if (!term.buf[0].busy)
		buf = &term.buf[0];
	else if (!term.buf[1].busy)
		buf = &term.buf[1];
	else
		abort();

	if (term.resize)
		if (new_buffer(buf) < 0)
			exit(1);

	return buf;
}

#define mul(a, b) (((a) * (b) + 255) >> 8)
#define join(a, r, g, b) ((a) << 24 | (r) << 16 | (g) << 8 | (b))

typedef uint_fast8_t uf8;

static void blank(uint32_t *dst, uint w,
		  uf8 br, uf8 bg, uf8 bb, uf8 ba)
{
	uint i;
	uint32_t b;
	int h = term.cheight;

	b = join(ba, mul(br, ba), mul(bg, ba), mul(bb, ba));
	w *= term.cwidth;

	while (h--) {
		for (i = 0; i < w; ++i)
			dst[i] = b;
		dst += term.width;
	}
}

static void print(uint32_t *dst, uint w,
		  uf8 br, uf8 bg, uf8 bb,
		  uf8 fr, uf8 fg, uf8 fb,
		  uf8 ba, uchr *glyph)
{
	uint i;
	int h = term.cheight;

	w *= term.cwidth;

	br = mul(br, ba);
	bg = mul(bg, ba);
	bb = mul(bb, ba);
	while (h--) {
		for (i = 0; i < w; ++i) {
			uf8 fa = glyph[i];
			if (fa == 0) {
				dst[i] = join(ba, br, bg, bb);
			} else if (fa == 0xff) {
				dst[i] = join(0xff, fr, fg, fb);
			} else {
				uf8 ca = 255 - fa;
				dst[i] = join(fa + mul(ba, ca),
					mul(fr, fa) + mul(br, ca),
					mul(fg, fa) + mul(bg, ca),
					mul(fb, fa) + mul(bb, ca));
			}
		}

		glyph += w;
		dst += term.width;
	}
}

static int draw_cell(struct tsm_screen *tsm, uint32_t id, const uint32_t *ch,
		     size_t len, unsigned char_width, unsigned x, unsigned y,
		     const struct tsm_screen_attr *a, tsm_age_t age,
		     void *data)
{
	struct buffer *buffer = data;
	uint32_t *dst = buffer->data;

	if (age && age <= buffer->age)
		return 0;

	dst += term.margin.top * term.width + term.margin.left;
	dst = &dst[y * term.cheight * term.width + x * term.cwidth];

	if (len == 0) {
		if (a->inverse)
			blank(dst, char_width,
			      ~a->br, ~a->bg, ~a->bb, term.cfg.opacity);
		else
			blank(dst, char_width,
			      a->br, a->bg, a->bb, term.cfg.opacity);
	} else {
		/* todo, combining marks */
		uchr *g = get_glyph(id, ch[0], char_width);

		if (a->inverse)
			print(dst, char_width,
			      ~a->br, ~a->bg, ~a->bb,
			      ~a->fr, ~a->fg, ~a->fb,
			      term.cfg.opacity, g);
		else
			print(dst, char_width,
			      a->br, a->bg, a->bb,
			      a->fr, a->fg, a->fb,
			      term.cfg.opacity, g);
	}

	return 0;
}

static void draw_margin(struct buffer *buffer)
{
	uint32_t *dst = buffer->data;
	uint8_t a = term.cfg.opacity;
	uint8_t *rgb = term.cfg.colors[TSM_COLOR_BACKGROUND];
	uint32_t c = join(a, mul(rgb[0], a), mul(rgb[1], a), mul(rgb[2], a));
	size_t inw = term.col * term.cwidth;
	size_t inh = term.row * term.cheight;
	size_t i, j;

	for (i = 0; i < term.width * term.margin.top; ++i)
		dst[i] = c;

	for (i = (term.margin.top + inh) * term.width;
	     i < term.height * term.width;
	     ++i)
		dst[i] = c;

	for (i = term.margin.top; i < term.margin.top + inh; ++i) {
		for (j = 0; j < term.margin.left; ++j)
			dst[i * term.width + j] = c;

		for (j = term.margin.left + inw; j < term.width; ++j)
			dst[i * term.width + j] = c;
	}
}

static void frame_callback(void *data, struct wl_callback *cb, uint32_t time)
{
	assert(term.cb == cb);
	wl_callback_destroy(cb);
	term.cb = NULL;
	term.can_redraw = true;
}

static const struct wl_callback_listener frame_listener = {
	frame_callback
};

static void redraw(void)
{
	struct buffer *buffer = swap_buffers();

	wl_surface_attach(term.surf, buffer->b, 0, 0);
	buffer->age = tsm_screen_draw(term.screen, draw_cell, buffer);
	if (buffer->age == 0)
		term.buf[0].age = term.buf[1].age = 0;
	wl_surface_damage(term.surf, 0, 0, term.width, term.height);

	term.cb = wl_surface_frame(term.surf);
	wl_callback_add_listener(term.cb, &frame_listener, NULL);
	wl_surface_commit(term.surf);

	buffer->busy = true;
	term.can_redraw = false;
	term.need_redraw = false;
	if (term.resize) {
		--term.resize;

		if (term.cfg.margin)
			draw_margin(buffer);
	}
}

static void reset_repeat(void)
{
	struct itimerspec its = {
		{ 0, 0 }, { 0, 0 }
	};

	timerfd_settime(term.repeat.fd, 0, &its, NULL);
}

static void kbd_keymap(void *data, struct wl_keyboard *k, uint32_t fmt,
		       int32_t fd, uint32_t size)
{
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	struct xkb_compose_table *compose_table;
	struct xkb_compose_state *compose_state;
	char *map;

	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}

	map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return;
	}

	keymap = xkb_keymap_new_from_string(term.xkb_ctx, map,
					    XKB_KEYMAP_FORMAT_TEXT_V1,
					    XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	close(fd);

	if (!keymap) {
		fprintf(stderr, "failed to compile keymap\n");
		return;
	}

	state = xkb_state_new(keymap);
	if (!state) {
		fprintf(stderr, "failed to create XKB state\n");
		xkb_keymap_unref(keymap);
		return;
	}

	/* Set up XKB compose table */
	compose_table =
		xkb_compose_table_new_from_locale(term.xkb_ctx,
						  getenv("LANG"),
						  XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (compose_table) {
		/* Set up XKB compose state */
		compose_state = xkb_compose_state_new(compose_table,
					      XKB_COMPOSE_STATE_NO_FLAGS);
		if (compose_state) {
			xkb_compose_state_unref(term.xkb_compose_state);
			xkb_compose_table_unref(term.xkb_compose_table);
			term.xkb_compose_state = compose_state;
			term.xkb_compose_table = compose_table;
		} else {
			fprintf(stderr, "could not create XKB compose state.  "
				"Disabiling compose.\n");
			xkb_compose_table_unref(compose_table);
			compose_table = NULL;
		}
	} else {
		fprintf(stderr, "could not create XKB compose table for locale '%s'.  "
			"Disabiling compose\n", getenv("LANG"));
	}

	xkb_keymap_unref(term.xkb_keymap);
	xkb_state_unref(term.xkb_state);
	term.xkb_keymap = keymap;
	term.xkb_state = state;

	term.xkb_ctrl = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
	term.xkb_alt = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);
	term.xkb_shift = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
}

static void kbd_enter(void *data, struct wl_keyboard *k, uint32_t serial,
		      struct wl_surface *surf, struct wl_array *keys)
{
}

static void kbd_leave(void *data, struct wl_keyboard *k, uint32_t serial,
		      struct wl_surface *surf)
{
	reset_repeat();
}

static xkb_keysym_t process_key(xkb_keysym_t sym)
{
	if (!term.xkb_compose_state)
		return sym;
	if (sym == XKB_KEY_NoSymbol)
		return sym;
	if (xkb_compose_state_feed(term.xkb_compose_state,
				   sym) != XKB_COMPOSE_FEED_ACCEPTED)
		return sym;

	switch (xkb_compose_state_get_status(term.xkb_compose_state)) {
	case XKB_COMPOSE_COMPOSING:
		return XKB_KEY_NoSymbol;
	case XKB_COMPOSE_COMPOSED:
		return xkb_compose_state_get_one_sym(term.xkb_compose_state);
	case XKB_COMPOSE_CANCELLED:
		return XKB_KEY_NoSymbol;
	case XKB_COMPOSE_NOTHING:
		return sym;
	default:
		return sym;
	}
}

static void kbd_key(void *data, struct wl_keyboard *k, uint32_t serial,
		    uint32_t time, uint32_t key, uint32_t state)
{
	xkb_keysym_t sym;
	const xkb_keysym_t *syms;
	uint32_t num_syms;
	uint32_t unicode;

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		reset_repeat();
		return;
	}
	
	num_syms = xkb_state_key_get_syms(term.xkb_state, key + 8, &syms); 

	sym = XKB_KEY_NoSymbol;
	if (num_syms == 1)
		sym = syms[0];

	sym = process_key(sym);

	switch (sym) {
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R:
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
	case XKB_KEY_Hyper_L:
	case XKB_KEY_Hyper_R:
		return;
	}

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		unicode = xkb_keysym_to_utf32(sym);

		if (xkb_compose_state_get_status(term.xkb_compose_state) != XKB_COMPOSE_COMPOSING) {
			term.repeat.sym = sym;
			term.repeat.unicode = unicode;
			timerfd_settime(term.repeat.fd, 0, &term.repeat.its, NULL);
		}

		tsm_vte_handle_keyboard(term.vte, sym, 0, term.mods, unicode);
	}
}

static void kbd_mods(void *data, struct wl_keyboard *k, uint32_t serial,
		     uint32_t depressed, uint32_t latched, uint32_t locked,
		     uint32_t group)
{
	xkb_mod_mask_t m;

	if (!term.xkb_keymap)
		return;

	xkb_state_update_mask(term.xkb_state, depressed, latched, locked,
			      0, 0, group);

	m = xkb_state_serialize_mods(term.xkb_state, XKB_STATE_MODS_EFFECTIVE);

	term.mods = 0;
	if (m & 1 << term.xkb_alt)
		term.mods |= TSM_ALT_MASK;
	if (m & 1 << term.xkb_ctrl)
		term.mods |= TSM_CONTROL_MASK;
	if (m & 1 << term.xkb_shift)
		term.mods |= TSM_SHIFT_MASK;

	reset_repeat();
}

static void kbd_repeat(void *data, struct wl_keyboard *k,
		       int32_t rate, int32_t delay)
{
	if (rate == 0)
		return;
	else if (rate == 1)
		term.repeat.its.it_interval.tv_sec = 1;
	else
		term.repeat.its.it_interval.tv_nsec = 1000000000 / rate;

	term.repeat.its.it_value.tv_sec = delay / 1000;
	delay -= term.repeat.its.it_value.tv_sec * 1000;
	term.repeat.its.it_value.tv_nsec = delay * 1000 * 1000;
}

static struct wl_keyboard_listener kbd_listener = {
	kbd_keymap,
	kbd_enter,
	kbd_leave,
	kbd_key,
	kbd_mods,
	kbd_repeat
};

static void paste(void)
{
	struct epoll_event ee;

	if (!term.ps_dm)
		return;

	if (!term.paste.acceptable)
		return;

	if (term.paste.active)
		end_paste();

	if (pipe(term.paste.fd) < 0)
		return;

	gtk_primary_selection_offer_receive(term.paste.offer, "UTF8_STRING",
					    term.paste.fd[1]);

	ee.events = EPOLLIN;
	ee.data.ptr = &pfp;
	epoll_ctl(term.fd, EPOLL_CTL_ADD, term.paste.fd[0], &ee);

	term.paste.active = true;
}

static void pss_send(void *data,
		     struct gtk_primary_selection_source *source,
		     const char *mime_type,
		     int32_t fd)
{
	write(fd, term.copy.data, strlen(term.copy.data));
	close(fd);
}

static void pss_cancelled(void *data,
			  struct gtk_primary_selection_source *source)
{
	gtk_primary_selection_source_destroy(term.copy.source);
	term.copy.source = NULL;
	free(term.copy.data);
}

static struct gtk_primary_selection_source_listener pss_listener = {
	pss_send,
	pss_cancelled
};

static void copy(uint32_t serial)
{
	if (!term.ps_dm)
		return;

	term.copy.source =
		gtk_primary_selection_device_manager_create_source(term.ps_dm);
	gtk_primary_selection_source_offer(term.copy.source, "UTF8_STRING");
	gtk_primary_selection_source_add_listener(term.copy.source,
						  &pss_listener, NULL);
	gtk_primary_selection_device_set_selection(term.ps_d, term.copy.source,
						   serial);
	tsm_screen_selection_copy(term.screen, &term.copy.data);
}

static void uncopy(void)
{
	if (!term.ps_dm)
		return;

	if (!term.copy.source)
		return;

	pss_cancelled(NULL, term.copy.source);
}

static inline uint grid_x(void)
{
	return wl_fixed_to_double(term.ptr_x) / term.cwidth;
}

static inline uint grid_y(void)
{
	return wl_fixed_to_double(term.ptr_y) / term.cheight;
}

static void ptr_enter(void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, struct wl_surface *surface,
		      wl_fixed_t x, wl_fixed_t y)
{
	term.ptr_x = x;
	term.ptr_y = y;
}

static void ptr_leave(void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, struct wl_surface *surface)
{
}

static void ptr_motion(void *data, struct wl_pointer *wl_pointer,
		       uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	term.ptr_x = x;
	term.ptr_y = y;

	switch (term.select) {
	case 1:
		uncopy();
		term.select = 2;
		tsm_screen_selection_start(term.screen, grid_x(), grid_y());
		term.need_redraw = true;
		break;
	case 2:
		tsm_screen_selection_target(term.screen, grid_x(), grid_y());
		term.need_redraw = true;
	}
}

static void ptr_button(void *data, struct wl_pointer *wl_pointer,
		       uint32_t serial, uint32_t time, uint32_t button,
		       uint32_t state)
{
	if (button == 0x110) {
		switch (state) {
		case WL_POINTER_BUTTON_STATE_PRESSED:
			if (term.select == 3) {
				tsm_screen_selection_reset(term.screen);
				term.need_redraw = true;
			}
			term.select = 1;
			break;
		case WL_POINTER_BUTTON_STATE_RELEASED:
			if (term.select == 2) {
				copy(serial);
				term.select = 3;
			} else {
				term.select = 0;
			}
		}
	} else if (button == 0x112 &&
		   state == WL_POINTER_BUTTON_STATE_RELEASED) {
		paste();
	}
}

static void ptr_axis(void *data, struct wl_pointer *wl_pointer,
		     uint32_t time, uint32_t axis, wl_fixed_t value)
{
	int v = wl_fixed_to_double(value) / 3;

	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;

	if (v > 0)
		tsm_screen_sb_down(term.screen, v);
	else
		tsm_screen_sb_up(term.screen, -v);
	term.need_redraw = true;
}

static void ptr_frame(void *data, struct wl_pointer *wl_pointer)
{
}

static void ptr_axis_source(void *data, struct wl_pointer *wl_pointer,
			    uint32_t axis_source)
{
}

static void ptr_axis_stop(void *data, struct wl_pointer *wl_pointer,
			  uint32_t time, uint32_t axis)
{
}

static void ptr_axis_discrete(void *data, struct wl_pointer *wl_pointer,
			      uint32_t axis, int32_t discrete)
{
}

static struct wl_pointer_listener ptr_listener = {
	ptr_enter,
	ptr_leave,
	ptr_motion,
	ptr_button,
	ptr_axis,
	ptr_frame,
	ptr_axis_source,
	ptr_axis_stop,
	ptr_axis_discrete
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !term.kbd) {
		term.kbd = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(term.kbd, &kbd_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && term.kbd) {
		wl_keyboard_release(term.kbd);
		term.kbd = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !term.ptr) {
		term.ptr = wl_seat_get_pointer(term.seat);
		wl_pointer_add_listener(term.ptr, &ptr_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && term.ptr) {
		wl_pointer_release(term.ptr);
		term.ptr = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static struct wl_seat_listener seat_listener = {
	seat_capabilities,
	seat_name
};

static void pso_offer(void *data,
		      struct gtk_primary_selection_offer *offer,
		      const char *mime_type)
{
	if (strcmp(mime_type, "UTF8_STRING") == 0) {
		term.paste.acceptable = true;
	}
}

static struct gtk_primary_selection_offer_listener pso_listener = {
	pso_offer
};

static void psd_data_offer(void *data,
			   struct gtk_primary_selection_device *ps_d,
			   struct gtk_primary_selection_offer *offer)
{
	if (term.paste.offer)
		gtk_primary_selection_offer_destroy(term.paste.offer);
	term.paste.offer = offer;
	term.paste.acceptable = false;
	gtk_primary_selection_offer_add_listener(offer, &pso_listener, NULL);
}

static void psd_selection(void *data,
			  struct gtk_primary_selection_device *ps_d,
			  struct gtk_primary_selection_offer *id)
{
	if (id == NULL && term.paste.offer) {
		gtk_primary_selection_offer_destroy(term.paste.offer);
		term.paste.offer = NULL;
		term.paste.acceptable = false;
	}
}

static struct gtk_primary_selection_device_listener psd_listener = {
	psd_data_offer,
	psd_selection
};

static void toplvl_configure(void *data, struct xdg_toplevel *xdg_toplevel,
			     int32_t width, int32_t height,
			     struct wl_array *state)
{
	term.configured = false;
	term.confwidth = width ? width : term.cfg.col * term.cwidth;
	term.confheight = height ? height : term.cfg.row * term.cheight;
}

static void toplvl_close(void *data, struct xdg_toplevel *t)
{
	term.die = true;
}

static const struct xdg_toplevel_listener toplvl_listener = {
	toplvl_configure,
	toplvl_close
};

static void configure(void *d, struct xdg_surface *surf, uint32_t serial)
{
	xdg_surface_ack_configure(surf, serial);
	int col = term.confwidth / term.cwidth;
	int row = term.confheight / term.cheight;
	struct winsize ws = {
		row, col, 0, 0
	};

	assert(!term.configured);
	term.configured = true;

	if (col == 0 || row == 0)
		return;

	if (term.width == term.confwidth && term.height == term.confheight)
		return;

	if (term.cfg.margin) {
		term.width = term.confwidth;
		term.height = term.confheight;
		term.margin.left = (term.width - col * term.cwidth) / 2;
		term.margin.top = (term.height - row * term.cheight) / 2;
		term.need_redraw = true;
		term.resize = 2;
	} else {
		term.width = col * term.cwidth;
		term.height = row * term.cheight;
	}

	if (term.col == col && term.row == row)
		return;

	term.col = col;
	term.row = row;
	tsm_screen_resize(term.screen, col, row);
	if (term.master_fd >= 0 && ioctl(term.master_fd, TIOCSWINSZ, &ws) < 0)
		fprintf(stderr, "could not resize pty: %m\n");

	term.need_redraw = true;
	term.resize = 2;
}

static const struct xdg_surface_listener surf_listener = {
	configure
};

static void ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	ping
};

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
	if (format == WL_SHM_FORMAT_ARGB8888)
		term.has_argb = true;
}

static const struct wl_shm_listener shm_listener = {
	shm_format
};

static void registry_get(void *data, struct wl_registry *r, uint32_t id,
			 const char *i, uint32_t version)
{
	if (strcmp(i, "wl_compositor") == 0) {
		term.cp = wl_registry_bind(r, id, &wl_compositor_interface, 1);
	} else if (strcmp(i, "wl_shm") == 0) {
		term.shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
		wl_shm_add_listener(term.shm, &shm_listener, NULL);
	} else if (strcmp(i, "xdg_wm_base") == 0) {
		term.wm_base = wl_registry_bind(r, id, &xdg_wm_base_interface,
					      1);
		xdg_wm_base_add_listener(term.wm_base, &wm_base_listener, NULL);
	} else if (strcmp(i, "wl_seat") == 0) {
		term.seat = wl_registry_bind(r, id, &wl_seat_interface, 5);
		wl_seat_add_listener(term.seat, &seat_listener, NULL);
	} else if (strcmp(i, "gtk_primary_selection_device_manager") == 0) {
		term.ps_dm = wl_registry_bind(r, id,
			&gtk_primary_selection_device_manager_interface, 1);
	}
}

static void registry_loose(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener reg_listener = {
	registry_get,
	registry_loose
};

void setup_pty(char *argv[])
{
	pid_t pid = forkpty(&term.master_fd, NULL, NULL, NULL);

	if (pid < 0) {
		fprintf(stderr, "forkpty failed: %m");
		exit(EXIT_FAILURE);
	} else if (pid == 0) {
		char *prog;
		setenv("TERM", "xterm-256color", 1);
		if (*argv) {
			execvp(*argv, argv);
			prog = *argv;
		} else {
			execlp(term.cfg.shell, term.cfg.shell, NULL);
			prog = term.cfg.shell;
		}
		fprintf(stderr, "could not execute %s: %m", prog);
		pause();
		exit(EXIT_FAILURE);
	}
	fcntl(term.master_fd, F_SETFL, O_NONBLOCK);
}


#define CONF_FILE "havoc.cfg"

static int clip(int a, int b, int c)
{
	return a < b ? b : a > c ? c : a;
}

static void child_config(char *key, char *val)
{
	if (strcmp(key, "program") == 0)
		strncpy(term.cfg.shell, val, sizeof(term.cfg.shell) - 1);
}

static void window_config(char *key, char *val)
{
	if (strcmp(key, "opacity") == 0)
		term.cfg.opacity = clip(atoi(val), 0, 255);
	else if (strcmp(key, "margin") == 0)
		term.cfg.margin = strcmp(val, "yes") == 0;
}

static void terminal_config(char *key, char *val)
{
	if (strcmp(key, "rows") == 0)
		term.cfg.row = clip(atoi(val), 1, 300);
	else if (strcmp(key, "columns") == 0)
		term.cfg.col = clip(atoi(val), 1, 600);
	else if (strcmp(key, "scrollback") == 0)
		term.cfg.scrollback = strtoul(val, NULL, 10);
}

static void font_config(char *key, char *val)
{
	if (strcmp(key, "size") == 0)
		term.cfg.font_size = clip(atoi(val), 6, 200);
	else if (strcmp(key, "path") == 0)
		strncpy(term.cfg.font_path, val,
			sizeof(term.cfg.font_path) - 1);
}

static void set_color(enum tsm_vte_color field, uint32_t val)
{
	term.cfg.colors[field][2] = val;
	term.cfg.colors[field][1] = val >> 8;
	term.cfg.colors[field][0] = val >> 16;
}

static void color_config(char *key, char *val)
{
	uint32_t color;
	char *p;

	if (*val == '#') {
		color = strtol(++val, &p, 16);
		if (*p != '\0')
			goto invalid;
	} else {
		goto invalid;
	}

	if (strcmp(key, "foreground") == 0) {
		set_color(TSM_COLOR_FOREGROUND, color);
	} else if (strcmp(key, "background") == 0) {
		set_color(TSM_COLOR_BACKGROUND, color);
	} else if (strstr(key, "color") == key) {
		long int i = strtol(key + 5, &p, 10);
		if (*p == '\0' && i >= 0 && i < 16)
			set_color(i, color);
	}
	return;

invalid:
	fprintf(stderr, "invalid color for %s\n", key);
}

static FILE *open_config(void)
{
	char *dir;
	char path[512];
	FILE *f;

	if (term.opt.config) {
		if (*term.opt.config == '\0')
			return NULL;

		f = fopen(term.opt.config, "r");
		if (f == NULL)
			fprintf(stderr, "could not open '%s': %m, "
				"using default configuration\n",
				term.opt.config);
		return f;
	}

	dir = getenv("XDG_CONFIG_HOME");
	if (dir) {
		snprintf(path, sizeof(path), "%s/%s", dir, CONF_FILE);
		f = fopen(path, "r");
		if (f)
			return f;
	}

	dir = getenv("HOME");
	if (dir) {
		snprintf(path, sizeof(path), "%s/.config/%s", dir, CONF_FILE);
		f = fopen(path, "r");
		if (f)
			return f;
	}

	f = fopen(CONF_FILE, "r");
	return f;
}

static void read_config(void)
{
	FILE *f = open_config();
	char *key, *val, *p, line[512];
	void (*section)(char *, char *) = NULL;
	int i = 0;

	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f)) {
		++i;
		key = line;
		while (isblank(*key))
			++key;

		switch (*key) {
		case '\n':
		case '#':
			continue;
		case '[':
			p = strchr(key, ']');
			if (p == NULL) {
				fprintf(stderr, "error on config line %d\n", i);
				continue;
			}
			*p = '\0';
			++key;

			if (strcmp(key, "child") == 0)
				section = &child_config;
			else if (strcmp(key, "window") == 0)
				section = &window_config;
			else if (strcmp(key, "terminal") == 0)
				section = &terminal_config;
			else if (strcmp(key, "font") == 0)
				section = &font_config;
			else if (strcmp(key, "colors") == 0)
				section = &color_config;

			continue;
		default:
			val = strchr(key, '=');
			if (val == NULL) {
				fprintf(stderr, "error on config line %d\n", i);
				continue;
			}

			p = val - 1;
			while (isblank(*p) && p > key) {
				*p = '\0';
				--p;
			}

			val[0] = '\0';
			++val;
			while (isblank(*val))
				++val;

			p = val + strlen(val) - 1;
			while (isspace(*p) && p > val) {
				*p = '\0';
				--p;
			}

			if (section)
				section(key, val);
		}
	}

	fclose(f);
}

static void usage(void)
{
	printf("usage: havoc [option...] [program [args...]]\n\n"
	       "  -c <file>  Specify configuration file."
			     " Use empty string for defaults.\n"
	       "  -l         Keep window open after the child process exits.\n"
	       "  -s <name>  Wayland display server to connect to.\n"
	       "  -h         Show this help.\n");
	exit(EXIT_SUCCESS);
}

#define take(s) (*(argv+1) \
	? *++argv \
	: (fprintf(stderr, "missing " s " after option '%s'\n", *argv), \
	  exit(EXIT_FAILURE), NULL))

int main(int argc, char *argv[])
{
	int display_fd;
	struct epoll_event ee[16];
	int n, i, ret = 1;

	while (++argv, *argv && **argv == '-') {
retry:
		switch (*++*argv) {
		case 'c':
			term.opt.config = take("config file path");
			break;
		case 'l':
			term.opt.linger = true;
			break;
		case 's':
			term.opt.display = take("display name or socket");
			break;
		case 'h':
			usage();
			break;
		case '-':
			goto retry;
		default:
			fprintf(stderr, "unrecognized command line option "
				"'%s'\n", *argv);
			exit(EXIT_FAILURE);
		}
	}
	read_config();
	setup_pty(argv);

	if (font_init(term.cfg.font_size, term.cfg.font_path,
		      &term.cwidth, &term.cheight) < 0) {
		fprintf(stderr, "could not load font\n");
		goto efont;
	}

	term.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (term.xkb_ctx == NULL)
		goto exkb;

	term.display = wl_display_connect(term.opt.display);
	if (term.display == NULL) {
		fprintf(stderr, "could not connect to display\n");
		goto econnect;
	}

	term.registry = wl_display_get_registry(term.display);
	wl_registry_add_listener(term.registry, &reg_listener, NULL);

	wl_display_roundtrip(term.display);
	if (!term.cp || !term.shm) {
		fprintf(stderr, "missing required globals\n");
		goto eglobals;
	}
	if (!term.wm_base) {
		fprintf(stderr, "your compositor does not support xdg_wm_base,"
			" make sure you have the latest version.\n");
		goto eglobals;
	}

	wl_display_roundtrip(term.display);
	if (term.has_argb == false) {
		fprintf(stderr, "missing required formats\n");
		goto eglobals;
	}

	if (tsm_screen_new(&term.screen, log_tsm, NULL) < 0) {
		fprintf(stderr, "failed to initialize tsm\n");
		goto etsm;
	}
	tsm_screen_set_max_sb(term.screen, term.cfg.scrollback);

	if (tsm_vte_new(&term.vte, term.screen, wcb, NULL, log_tsm, NULL) < 0)
		goto evte;
	tsm_vte_set_palette(term.vte, term.cfg.colors);

	term.surf = wl_compositor_create_surface(term.cp);
	if (term.surf == NULL)
		goto esurf;
	term.xdgsurf = xdg_wm_base_get_xdg_surface(term.wm_base, term.surf);
	if (term.xdgsurf == NULL)
		goto exdgsurf;
	xdg_surface_add_listener(term.xdgsurf, &surf_listener, NULL);
	term.toplvl = xdg_surface_get_toplevel(term.xdgsurf);
	if (term.toplvl == NULL)
		goto etoplvl;
	xdg_toplevel_add_listener(term.toplvl, &toplvl_listener, NULL);
	xdg_toplevel_set_title(term.toplvl, "havoc");
	wl_surface_commit(term.surf);
	term.can_redraw = true;

	term.repeat.fd = timerfd_create(CLOCK_MONOTONIC,
					TFD_NONBLOCK | TFD_CLOEXEC);
	if (term.repeat.fd < 0) {
		fprintf(stderr, "could not create key repeat timer: %m\n");
		goto etimer;
	}

	if (term.ps_dm && term.seat) {
		term.ps_d = gtk_primary_selection_device_manager_get_device(
			term.ps_dm, term.seat);
		gtk_primary_selection_device_add_listener(term.ps_d,
							  &psd_listener, NULL);
	}

	display_fd = wl_display_get_fd(term.display);
	term.fd = epoll_create1(EPOLL_CLOEXEC);

	ee[0].events = EPOLLIN;

	ee[0].data.ptr = &dfp;
	epoll_ctl(term.fd, EPOLL_CTL_ADD, display_fd, &ee[0]);

	ee[0].data.ptr = &tfp;
	epoll_ctl(term.fd, EPOLL_CTL_ADD, term.master_fd, &ee[0]);

	ee[0].data.ptr = &rfp;
	epoll_ctl(term.fd, EPOLL_CTL_ADD, term.repeat.fd, &ee[0]);

	while (!term.die) {
		if (term.can_redraw && term.need_redraw && term.configured)
			redraw();

		wl_display_flush(term.display);

		n = epoll_wait(term.fd, ee, 16, -1);
		for (i = 0; i < n; i++) {
			void (*f)(int) = ((struct epcb *)ee[i].data.ptr)->f;
			if (ee[i].events & EPOLLERR)
				abort();
			f(ee[i].events);
		}
	}

	ret = 0;

	if (term.buf[0].b)
		wl_buffer_destroy(term.buf[0].b);
	if (term.buf[1].b)
		wl_buffer_destroy(term.buf[1].b);
	if (term.cb)
		wl_callback_destroy(term.cb);

	close(term.repeat.fd);
etimer:
	if (term.ps_d)
		gtk_primary_selection_device_destroy(term.ps_d);
	if (term.ptr)
		wl_pointer_release(term.ptr);
	if (term.kbd)
		wl_keyboard_release(term.kbd);

	xdg_toplevel_destroy(term.toplvl);
etoplvl:
	xdg_surface_destroy(term.xdgsurf);
exdgsurf:
	wl_surface_destroy(term.surf);
esurf:
	tsm_vte_unref(term.vte);
evte:
	tsm_screen_unref(term.screen);
etsm:
eglobals:
	if (term.ps_dm)
		gtk_primary_selection_device_manager_destroy(term.ps_dm);
	if (term.seat)
		wl_seat_destroy(term.seat);
	if (term.wm_base)
		xdg_wm_base_destroy(term.wm_base);
	if (term.shm)
		wl_shm_destroy(term.shm);
	if (term.cp)
		wl_compositor_destroy(term.cp);

	wl_registry_destroy(term.registry);
	wl_display_flush(term.display);
	wl_display_disconnect(term.display);

econnect:
	xkb_context_unref(term.xkb_ctx);
exkb:
	font_deinit();
efont:
	return ret;
}
