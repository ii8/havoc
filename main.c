#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <ctype.h>

#include <errno.h>
#include <limits.h>
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
#include <wayland-cursor.h>

#include "tsm/libtsm.h"
#include "gtk-primary-selection.h"
#include "surface-extension.h"
#include "touch-extension.h"

int font_init(int, char *, int *, int *);
void font_deinit(void);
unsigned char *get_glyph(uint32_t, uint32_t, unsigned);

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
	bool shm_argb;
	struct wl_shell *shell;
	struct wl_seat *seat;
	struct qt_surface_extension *qt_ext;
	struct qt_touch_extension *qt_touch;

	struct wl_surface *surf;
	struct wl_shell_surface *shell_surf;
	struct qt_extended_surface *ext_surf;

	struct buffer {
		struct wl_buffer *b;
		void *data;
		size_t size;
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
	wl_fixed_t ptr_x, ptr_y;
	int select;

	struct {
		struct wl_cursor_theme *theme;
		struct wl_cursor *current;
		long long anim_start;
		struct wl_surface *surface;
		struct wl_callback *callback;
		uint32_t enter_serial;
	} cursor;

	struct {
		int fd;
		uint32_t key;
		xkb_keysym_t sym;
		uint32_t unicode;
		struct itimerspec its;
	} repeat;

	struct wl_data_device_manager *d_dm;
	struct wl_data_device *d_d;
	struct gtk_primary_selection_device_manager *ps_dm;
	struct gtk_primary_selection_device *ps_d;

	struct {
		struct wl_data_source *source;
		char *data;
	} d_copy;

	struct {
		struct gtk_primary_selection_source *source;
		char *data;
	} ps_copy;

	struct {
		struct wl_data_offer *d_offer;
		struct gtk_primary_selection_offer *ps_offer;
		bool d_acceptable;
		bool ps_acceptable;

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
		unsigned scrollback;
		bool margin;
		unsigned char opacity;
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

static void wcb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	assert(len <= PIPE_BUF);
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

	tsm_vte_handle_keyboard(term.vte, term.repeat.sym, XKB_KEY_NoSymbol,
				term.mods, term.repeat.unicode);
}

static long long now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void cursor_draw(int frame)
{
	struct wl_buffer *buffer;
	struct wl_cursor_image *image;

	if ((int)term.cursor.current->image_count <= frame) {
		fprintf(stderr, "cursor frame index out of range\n");
		return;
	}

	image = term.cursor.current->images[frame];
	buffer = wl_cursor_image_get_buffer(image);
	wl_surface_attach(term.cursor.surface, buffer, 0, 0);
	wl_surface_damage(term.cursor.surface, 0, 0,
			  image->width, image->height);
	wl_surface_commit(term.cursor.surface);
	wl_pointer_set_cursor(term.ptr, term.cursor.enter_serial,
			      term.cursor.surface,
			      image->hotspot_x, image->hotspot_y);
}

static void cursor_request_frame_callback(void);

static void cursor_frame_callback(void *data, struct wl_callback *cb,
				  uint32_t time)
{
	int frame = wl_cursor_frame(term.cursor.current,
				    now() - term.cursor.anim_start);

	assert(cb == term.cursor.callback);
	wl_callback_destroy(term.cursor.callback);
	cursor_request_frame_callback();
	cursor_draw(frame);
}

static const struct wl_callback_listener cursor_frame_listener = {
	cursor_frame_callback
};

static void cursor_request_frame_callback(void)
{
	term.cursor.callback = wl_surface_frame(term.cursor.surface);
	wl_callback_add_listener(term.cursor.callback, &cursor_frame_listener,
				 NULL);
}

static void cursor_unset(void)
{
	if (term.cursor.callback) {
		wl_callback_destroy(term.cursor.callback);
		term.cursor.callback = NULL;
	}
	term.cursor.current = NULL;
}

static void cursor_set(const char *name)
{
	uint32_t duration;
	int frame;

	cursor_unset();

	if (term.ptr == NULL)
		return;

	if (term.cursor.theme == NULL)
		goto hide;

	if (name == NULL)
		goto hide;

	term.cursor.current = wl_cursor_theme_get_cursor(term.cursor.theme,
							 name);
	if (term.cursor.current == NULL)
		goto hide;

	frame = wl_cursor_frame_and_duration(term.cursor.current, 0, &duration);
	if (duration) {
		term.cursor.anim_start = now();
		cursor_request_frame_callback();
	}
	cursor_draw(frame);

	return;
hide:
	wl_pointer_set_cursor(term.ptr, term.cursor.enter_serial, NULL, 0, 0);
}

static void cursor_init(void)
{
	int size = 32;
	char *size_str = getenv("XCURSOR_SIZE");

	if (size_str && *size_str) {
		char *end;
		long s;

		errno = 0;
		s = strtol(size_str, &end, 10);
		if (errno == 0 && *end == '\0' && s > 0)
			size = s;
	}

	term.cursor.theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size,
						 term.shm);
	if (term.cursor.theme == NULL)
		return;

	term.cursor.surface = wl_compositor_create_surface(term.cp);
	if (term.cursor.surface == NULL) {
		wl_cursor_theme_destroy(term.cursor.theme);
		term.cursor.theme = NULL;
	}
}

static void cursor_free(void)
{
	if (term.cursor.callback)
		wl_callback_destroy(term.cursor.callback);
	if (term.cursor.surface)
		wl_surface_destroy(term.cursor.surface);
	if (term.cursor.theme)
		wl_cursor_theme_destroy(term.cursor.theme);
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
	tsm_vte_paste_end(term.vte);
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
						XKB_KEY_NoSymbol, 0, code);
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

static void buffer_release(void *data, struct wl_buffer *b)
{
	struct buffer *buffer = data;

	buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static int buffer_init(struct buffer *buf)
{
	struct wl_shm_pool *pool;
	char shm_name[14];
	int fd, stride;
	int max = 100;

	assert(!buf->busy);

	stride = term.width * 4;
	buf->size = stride * term.height;

	srand(time(NULL));
	do {
		sprintf(shm_name, "/havoc-%d", rand() % 1000000);
		fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
	} while (fd < 0 && errno == EEXIST && --max);

	if (fd < 0) {
		fprintf(stderr, "shm_open failed: %m\n");
		return -1;
	}
	shm_unlink(shm_name);

	if (ftruncate(fd, buf->size) < 0) {
		fprintf(stderr, "ftruncate failed: %m\n");
		close(fd);
		return -1;
	}

	buf->data = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			 fd, 0);

	if (buf->data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
		close(fd);
		return -1;
	}

	pool = wl_shm_create_pool(term.shm, fd, buf->size);
	buf->b = wl_shm_pool_create_buffer(pool, 0, term.width, term.height,
					   stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buf->b, &buffer_listener, buf);
	wl_shm_pool_destroy(pool);
	close(fd);

	buf->age = 0;

	return 0;
}

static void buffer_unmap(struct buffer *buf)
{
	if (buf->b) {
		wl_buffer_destroy(buf->b);
		munmap(buf->data, buf->size);
	}
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

	if (term.resize) {
		buffer_unmap(buf);
		if (buffer_init(buf) < 0)
			abort();
	}

	return buf;
}

#define mul(a, b) (((a) * (b) + 255) >> 8)
#define join(a, r, g, b) ((a) << 24 | (r) << 16 | (g) << 8 | (b))

typedef uint_fast8_t uf8;

static void blank(uint32_t *dst, unsigned w,
		  uf8 br, uf8 bg, uf8 bb, uf8 ba)
{
	int i;
	uint32_t b;
	int h = term.cheight;

	b = join(ba, mul(br, ba), mul(bg, ba), mul(bb, ba));
	w *= term.cwidth;

	while (w--) {
		for (i = 0; i < h; ++i)
			dst[i] = b;
		dst += term.width;
	}
}

static void print(uint32_t *dst, unsigned w,
		  uf8 br, uf8 bg, uf8 bb,
		  uf8 fr, uf8 fg, uf8 fb,
		  uf8 ba, unsigned char *glyph)
{
	int i;
	int h = term.cheight;

	w *= term.cwidth;

	br = mul(br, ba);
	bg = mul(bg, ba);
	bb = mul(bb, ba);
	while (w--) {
		for (i = 0; i < h; ++i) {
			uf8 fa = glyph[(h - (i + 1)) * term.cwidth];
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

		glyph += 1;
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
	dst = &dst[(term.row - (y + 1)) * term.cheight + x * term.cwidth * term.width];

	if (len == 0) {
		if (a->inverse)
			blank(dst, char_width,
			      ~a->br, ~a->bg, ~a->bb, term.cfg.opacity);
		else
			blank(dst, char_width,
			      a->br, a->bg, a->bb, term.cfg.opacity);
	} else {
		/* todo, combining marks */
		unsigned char *g = get_glyph(id, ch[0], char_width);

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
	size_t inw = term.row * term.cheight;
	size_t inh = term.col * term.cwidth;
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

static void paste(bool primary)
{
	struct epoll_event ee;

	if (primary) {
		if (!term.ps_dm || !term.paste.ps_acceptable)
			return;
	} else {
		if (!term.d_dm || !term.paste.d_acceptable)
			return;
	}

	if (term.paste.active)
		end_paste();

	if (pipe(term.paste.fd) < 0)
		return;

	if (primary) {
		gtk_primary_selection_offer_receive(term.paste.ps_offer,
						    "UTF8_STRING",
						    term.paste.fd[1]);
	} else {
		wl_data_offer_receive(term.paste.d_offer, "UTF8_STRING",
				      term.paste.fd[1]);
	}
	close(term.paste.fd[1]);

	ee.events = EPOLLIN;
	ee.data.ptr = &pfp;
	epoll_ctl(term.fd, EPOLL_CTL_ADD, term.paste.fd[0], &ee);

	term.paste.active = true;
	tsm_vte_paste_begin(term.vte);
}

static void ds_target(void *d, struct wl_data_source *ds, const char *mt)
{
}

static void ds_send(void *data, struct wl_data_source *ds,
		    const char *mime_type, int32_t fd)
{
	write(fd, term.d_copy.data, strlen(term.d_copy.data));
	close(fd);
}

static void ds_cancelled(void *data, struct wl_data_source *source)
{
	wl_data_source_destroy(term.d_copy.source);
	term.d_copy.source = NULL;
	free(term.d_copy.data);
}

static void ds_dnd_drop_performed(void *data, struct wl_data_source *ds)
{
}

static void ds_dnd_finished(void *data, struct wl_data_source *ds)
{
}

static void ds_action(void *data, struct wl_data_source *ds, uint32_t a)
{
}

static struct wl_data_source_listener ds_listener = {
	ds_target,
	ds_send,
	ds_cancelled,
	ds_dnd_drop_performed,
	ds_dnd_finished,
	ds_action
};

static void d_copy(uint32_t serial)
{
	if (!term.d_dm)
		return;

	term.d_copy.source =
		wl_data_device_manager_create_data_source(term.d_dm);
	wl_data_source_offer(term.d_copy.source, "UTF8_STRING");
	wl_data_source_add_listener(term.d_copy.source, &ds_listener, NULL);
	wl_data_device_set_selection(term.d_d, term.d_copy.source, serial);
	tsm_screen_selection_copy(term.screen, &term.d_copy.data);
}

static void d_uncopy(void)
{
	if (!term.d_dm)
		return;

	if (!term.d_copy.source)
		return;

	ds_cancelled(NULL, term.d_copy.source);
}

static void reset_repeat(void)
{
	struct itimerspec its = {
		{ 0, 0 }, { 0, 0 }
	};

	timerfd_settime(term.repeat.fd, 0, &its, NULL);
}

static void setup_compose(void)
{
	struct xkb_compose_table *compose_table;
	struct xkb_compose_state *compose_state;
	char *lang = getenv("LANG");

	if (lang == NULL)
		return;

	compose_table =
		xkb_compose_table_new_from_locale(term.xkb_ctx,
						  lang,
						  XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (!compose_table) {
		fprintf(stderr, "could not create XKB compose table "
				"for locale '%s'.\n", lang);
		return;
	}

	compose_state = xkb_compose_state_new(compose_table,
					      XKB_COMPOSE_STATE_NO_FLAGS);
	if (!compose_state) {
		fprintf(stderr, "could not create XKB compose state. "
				"Disabling compose.\n");
		xkb_compose_table_unref(compose_table);
		return;
	}

	xkb_compose_table_unref(term.xkb_compose_table);
	xkb_compose_state_unref(term.xkb_compose_state);
	term.xkb_compose_table = compose_table;
	term.xkb_compose_state = compose_state;
}

static void kbd_keymap(void *data, struct wl_keyboard *k, uint32_t fmt,
		       int32_t fd, uint32_t size)
{
	struct xkb_keymap *keymap;
	struct xkb_state *state;
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

	xkb_keymap_unref(term.xkb_keymap);
	xkb_state_unref(term.xkb_state);
	term.xkb_keymap = keymap;
	term.xkb_state = state;

	setup_compose();

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

static xkb_keysym_t compose(xkb_keysym_t sym)
{
	if (!term.xkb_compose_state)
		return sym;
	if (sym == XKB_KEY_NoSymbol)
		return sym;
	if (xkb_compose_state_feed(term.xkb_compose_state,
				   sym) != XKB_COMPOSE_FEED_ACCEPTED)
		return sym;

	switch (xkb_compose_state_get_status(term.xkb_compose_state)) {
	case XKB_COMPOSE_COMPOSED:
		return xkb_compose_state_get_one_sym(term.xkb_compose_state);
	case XKB_COMPOSE_COMPOSING:
	case XKB_COMPOSE_CANCELLED:
		return XKB_KEY_NoSymbol;
	case XKB_COMPOSE_NOTHING:
	default:
		return sym;
	}
}

static void kbd_key(void *data, struct wl_keyboard *k, uint32_t serial,
		    uint32_t time, uint32_t key, uint32_t state)
{
	xkb_keysym_t sym;
	uint32_t unicode;

	if (!term.xkb_keymap || !term.xkb_state)
		return;

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		if (term.repeat.key == key)
			reset_repeat();
		return;
	}

	cursor_set(NULL);

	sym = compose(xkb_state_key_get_one_sym(term.xkb_state, key + 8));

	unicode = xkb_keysym_to_utf32(sym);
	if (unicode == 0)
		unicode = TSM_VTE_INVALID;

	if (term.mods == (TSM_CONTROL_MASK | TSM_SHIFT_MASK)) {
		switch (sym) {
		case XKB_KEY_c:
		case XKB_KEY_C:
			d_uncopy();
			d_copy(serial);
			return;
		case XKB_KEY_v:
		case XKB_KEY_V:
			paste(false);
			return;
		}
	}

	tsm_vte_handle_keyboard(term.vte, sym, XKB_KEY_NoSymbol,
				term.mods, unicode);

	if (xkb_keymap_key_repeats(term.xkb_keymap, key + 8)) {
		term.repeat.key = key;
		term.repeat.sym = sym;
		term.repeat.unicode = unicode;
		timerfd_settime(term.repeat.fd, 0, &term.repeat.its, NULL);
	}
}

static void kbd_mods(void *data, struct wl_keyboard *k, uint32_t serial,
		     uint32_t depressed, uint32_t latched, uint32_t locked,
		     uint32_t group)
{
	if (!term.xkb_keymap || !term.xkb_state)
		return;

	xkb_state_update_mask(term.xkb_state, depressed, latched, locked,
			      0, 0, group);

	term.mods = 0;
	if (xkb_state_mod_index_is_active(term.xkb_state, term.xkb_alt,
					  XKB_STATE_MODS_EFFECTIVE) == 1)
		term.mods |= TSM_ALT_MASK;
	if (xkb_state_mod_index_is_active(term.xkb_state, term.xkb_ctrl,
					  XKB_STATE_MODS_EFFECTIVE) == 1)
		term.mods |= TSM_CONTROL_MASK;
	if (xkb_state_mod_index_is_active(term.xkb_state, term.xkb_shift,
					  XKB_STATE_MODS_EFFECTIVE) == 1)
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

static void pss_send(void *data,
		     struct gtk_primary_selection_source *source,
		     const char *mime_type,
		     int32_t fd)
{
	write(fd, term.ps_copy.data, strlen(term.ps_copy.data));
	close(fd);
}

static void pss_cancelled(void *data,
			  struct gtk_primary_selection_source *source)
{
	gtk_primary_selection_source_destroy(term.ps_copy.source);
	term.ps_copy.source = NULL;
	free(term.ps_copy.data);
}

static struct gtk_primary_selection_source_listener pss_listener = {
	pss_send,
	pss_cancelled
};

static void ps_copy(uint32_t serial)
{
	if (!term.ps_dm)
		return;

	term.ps_copy.source =
		gtk_primary_selection_device_manager_create_source(term.ps_dm);
	gtk_primary_selection_source_offer(term.ps_copy.source, "UTF8_STRING");
	gtk_primary_selection_source_add_listener(term.ps_copy.source,
						  &pss_listener, NULL);
	gtk_primary_selection_device_set_selection(term.ps_d,
						   term.ps_copy.source,
						   serial);
	tsm_screen_selection_copy(term.screen, &term.ps_copy.data);
}

static void ps_uncopy(void)
{
	if (!term.ps_dm)
		return;

	if (!term.ps_copy.source)
		return;

	pss_cancelled(NULL, term.ps_copy.source);
}

static inline unsigned grid_x(void)
{
	return (wl_fixed_to_double(term.ptr_x) - term.margin.left) / term.cwidth;
}

static inline unsigned grid_y(void)
{
	return (wl_fixed_to_double(term.ptr_y) - term.margin.top) / term.cheight;
}

static void ptr_enter(void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, struct wl_surface *surface,
		      wl_fixed_t x, wl_fixed_t y)
{
	term.ptr_x = x;
	term.ptr_y = y;

	term.cursor.enter_serial = serial;
	cursor_set("text");
}

static void ptr_leave(void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, struct wl_surface *surface)
{
	cursor_unset();
}

static void ptr_motion(void *data, struct wl_pointer *wl_pointer,
		       uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	term.ptr_x = x;
	term.ptr_y = y;

	switch (term.select) {
	case 1:
		ps_uncopy();
		term.select = 2;
		tsm_screen_selection_start(term.screen, grid_x(), grid_y());
		term.need_redraw = true;
		break;
	case 2:
		tsm_screen_selection_target(term.screen, grid_x(), grid_y());
		term.need_redraw = true;
	}

	if (term.cursor.current == NULL)
		cursor_set("text");
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
				ps_copy(serial);
				term.select = 3;
			} else {
				term.select = 0;
			}
		}
	} else if (button == 0x112 &&
		   state == WL_POINTER_BUTTON_STATE_RELEASED) {
		paste(true);
	}

	if (term.cursor.current == NULL)
		cursor_set("text");
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
		cursor_unset();
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	seat_capabilities,
	seat_name
};

static void do_offer(void *d, struct wl_data_offer *o, const char *mime_type)
{
	if (strcmp(mime_type, "UTF8_STRING") == 0)
		term.paste.d_acceptable = true;
}

static void do_source_actions(void *d, struct wl_data_offer *o, uint32_t sa)
{
}

static void do_action(void *d, struct wl_data_offer *o, uint32_t dnd_action)
{
}

static const struct wl_data_offer_listener do_listener = {
	do_offer,
	do_source_actions,
	do_action
};

static void dd_data_offer(void *data, struct wl_data_device *wl_data_device,
			  struct wl_data_offer *offer)
{
	if (term.paste.d_offer)
		wl_data_offer_destroy(term.paste.d_offer);
	term.paste.d_offer = offer;
	term.paste.d_acceptable = false;
	wl_data_offer_add_listener(offer, &do_listener, NULL);
}

static void dd_enter(void *data, struct wl_data_device *wl_data_device,
		     uint32_t serial, struct wl_surface *surface,
		     wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *id)
{
}

static void dd_leave(void *data, struct wl_data_device *wl_data_device)
{
}

static void dd_motion(void *data, struct wl_data_device *wl_data_device,
		      uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
}

static void dd_drop(void *data, struct wl_data_device *wl_data_device)
{
}

static void dd_selection(void *data, struct wl_data_device *wl_data_device,
			 struct wl_data_offer *id)
{
	if (id == NULL && term.paste.d_offer) {
		wl_data_offer_destroy(term.paste.d_offer);
		term.paste.d_offer = NULL;
		term.paste.d_acceptable = false;
	}
}

static const struct wl_data_device_listener dd_listener = {
	dd_data_offer,
	dd_enter,
	dd_leave,
	dd_motion,
	dd_drop,
	dd_selection
};

static void pso_offer(void *data,
		      struct gtk_primary_selection_offer *offer,
		      const char *mime_type)
{
	if (strcmp(mime_type, "UTF8_STRING") == 0)
		term.paste.ps_acceptable = true;
}

static const struct gtk_primary_selection_offer_listener pso_listener = {
	pso_offer
};

static void psd_data_offer(void *data,
			   struct gtk_primary_selection_device *ps_d,
			   struct gtk_primary_selection_offer *offer)
{
	if (term.paste.ps_offer)
		gtk_primary_selection_offer_destroy(term.paste.ps_offer);
	term.paste.ps_offer = offer;
	term.paste.ps_acceptable = false;
	gtk_primary_selection_offer_add_listener(offer, &pso_listener, NULL);
}

static void psd_selection(void *data,
			  struct gtk_primary_selection_device *ps_d,
			  struct gtk_primary_selection_offer *id)
{
	if (id == NULL && term.paste.ps_offer) {
		gtk_primary_selection_offer_destroy(term.paste.ps_offer);
		term.paste.ps_offer = NULL;
		term.paste.ps_acceptable = false;
	}
}

static const struct gtk_primary_selection_device_listener psd_listener = {
	psd_data_offer,
	psd_selection
};

static void ping(void *data, struct wl_shell_surface *surf, uint32_t serial)
{
	wl_shell_surface_pong(surf, serial);
}

static void configure(void *data, struct wl_shell_surface *surf,
		      uint32_t edges, int32_t width, int32_t height)
{
	term.confwidth = width;
	term.confheight = height;

	int col = term.confheight / term.cwidth;
	int row = term.confwidth / term.cheight;
	struct winsize ws = {
		row, col, 0, 0
	};

	term.configured = true;

	if (col == 0 || row == 0)
		return;

	if (term.width == term.confwidth && term.height == term.confheight)
		return;

	if (term.cfg.margin) {
		term.width = term.confwidth;
		term.height = term.confheight;
		term.margin.left = (term.width - row * term.cheight) / 2;
		term.margin.top = (term.height - col * term.cwidth) / 2;
		term.need_redraw = true;
		term.resize = 2;
	} else {
		term.width = row * term.cheight;
		term.height = col * term.cwidth;
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

static void popup_done(void *data, struct wl_shell_surface *surf)
{
}

static const struct wl_shell_surface_listener surf_listener = {
	ping,
	configure,
	popup_done
};

static void onscreen_visibility(void *data,
				struct qt_extended_surface *qt_extended_surface,
				int32_t visible)
{
}

static void set_generic_property(void *data,
				 struct qt_extended_surface *qt_extended_surface,
				 const char *name,
				 struct wl_array *value)
{
}

static void qtclose(void *data, struct qt_extended_surface *qt_extended_surface)
{
	term.die = true;
}

static const struct qt_extended_surface_listener ext_surf_listener = {
	onscreen_visibility,
	set_generic_property,
	qtclose
};

enum Qt_TouchPointState {
	Qt_TouchPointPressed = 0x01,
	Qt_TouchPointMoved = 0x02,
	Qt_TouchPointStationary = 0x04,
	Qt_TouchPointReleased = 0x08
};

static void qt_touch(void *data,
		     struct qt_touch_extension *qt_touch_extension,
		     uint32_t time,
		     uint32_t id,
		     uint32_t state,
		     int32_t x,
		     int32_t y,
		     int32_t normalized_x,
		     int32_t normalized_y,
		     int32_t width,
		     int32_t height,
		     uint32_t pressure,
		     int32_t velocity_x,
		     int32_t velocity_y,
		     uint32_t flags,
		     struct wl_array *rawdata)
{
	static uint32_t tp1 = 0;
	static uint32_t tp2 = 0;
	static int tp1_grid_x = 0;
	static int tp1_grid_y = 0;
	static int tp2_grid_x = 0;
	static int tp2_grid_y = 0;

	int grid_x = (((float)normalized_y / 10000.0) * term.height) - term.margin.top;
	int grid_y = (((float)(10000 - normalized_x) / 10000.0) * term.width) - term.margin.left;

	if (grid_x < 0)
		grid_x = 0;
	if (grid_x >= term.col * term.cwidth)
		grid_x = term.col * term.cwidth - 1;
	if (grid_y < 0)
		grid_y = 0;
	if (grid_y >= term.row * term.cheight)
		grid_y = term.row * term.cheight - 1;

	grid_x /= term.cwidth;
	grid_y /= term.cheight;

	if (id == tp1) {
		if ((state & 0xFFFF) & Qt_TouchPointReleased) {
			tp1 = tp2;
			tp1_grid_x = tp2_grid_x;
			tp1_grid_y = tp2_grid_y;
			tp2 = 0;
		} else {
			if (grid_x != tp1_grid_x || grid_y != tp1_grid_y) {
				if (tp2) {
					tsm_screen_selection_anchor(term.screen, grid_x, grid_y);
				} else {
					int diff = grid_y - tp1_grid_y;
					if (diff > 0)
						tsm_screen_sb_up(term.screen, diff);
					else
						tsm_screen_sb_down(term.screen, -diff);
				}
				term.need_redraw = true;
				tp1_grid_x = grid_x;
				tp1_grid_y = grid_y;
			}
		}
	} else if (id == tp2) {
		assert(tp1);
		if ((state & 0xFFFF) & Qt_TouchPointReleased) {
			tp2 = 0;
		} else {
			if (grid_x != tp2_grid_x || grid_y != tp2_grid_y) {
				tsm_screen_selection_target(term.screen, grid_x, grid_y);
				term.need_redraw = true;
				tp2_grid_x = grid_x;
				tp2_grid_y = grid_y;
			}
		}
	} else {
		if (tp1 == 0) {
			tsm_screen_selection_reset(term.screen);
			tp1 = id;
			tp1_grid_x = grid_x;
			tp1_grid_y = grid_y;
		} else if (tp2 == 0) {
			tsm_screen_selection_start(term.screen, tp1_grid_x, tp1_grid_y);
			tsm_screen_selection_target(term.screen, grid_x, grid_y);
			tp2 = id;
			tp2_grid_x = grid_x;
			tp2_grid_y = grid_y;
		}
		term.need_redraw = true;
	}
}

static void qt_touch_configure(void *data,
			       struct qt_touch_extension *qt_touch_extension,
			       uint32_t flags)
{
}

static const struct qt_touch_extension_listener qt_touch_listener = {
	qt_touch,
	qt_touch_configure
};

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
	if (format == WL_SHM_FORMAT_ARGB8888)
		term.shm_argb = true;
}

static const struct wl_shm_listener shm_listener = {
	shm_format
};

static void registry_get(void *data, struct wl_registry *r, uint32_t id,
			 const char *i, uint32_t version)
{
	if (strcmp(i, "wl_compositor") == 0) {
		term.cp = wl_registry_bind(r, id, &wl_compositor_interface, 2);
	} else if (strcmp(i, "wl_shm") == 0) {
		term.shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
		wl_shm_add_listener(term.shm, &shm_listener, NULL);
	} else if (strcmp(i, "wl_shell") == 0) {
		term.shell = wl_registry_bind(r, id, &wl_shell_interface, 1);
	} else if (strcmp(i, "wl_seat") == 0) {
		term.seat = wl_registry_bind(r, id, &wl_seat_interface, 2);
		wl_seat_add_listener(term.seat, &seat_listener, NULL);
	} else if (strcmp(i, "wl_data_device_manager") == 0) {
		term.d_dm = wl_registry_bind(r, id,
			&wl_data_device_manager_interface, 2);
	} else if (strcmp(i, "gtk_primary_selection_device_manager") == 0) {
		term.ps_dm = wl_registry_bind(r, id,
			&gtk_primary_selection_device_manager_interface, 1);
	} else if (strcmp(i, "qt_surface_extension") == 0) {
		term.qt_ext = wl_registry_bind(r, id, &qt_surface_extension_interface, 1);
	} else if (strcmp(i, "qt_touch_extension") == 0) {
		term.qt_touch = wl_registry_bind(r, id, &qt_touch_extension_interface, 1);
		qt_touch_extension_add_listener(term.qt_touch, &qt_touch_listener, NULL);
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
			execlp(term.cfg.shell, term.cfg.shell, (char *) NULL);
			prog = term.cfg.shell;
		}
		fprintf(stderr, "could not execute %s: %m", prog);
		pause();
		exit(EXIT_FAILURE);
	}
	fcntl(term.master_fd, F_SETFL, O_NONBLOCK);
}


#define CONF_FILE "havoc.cfg"

static long cfg_num(const char *nptr, int base, long min, long max)
{
	long n;

	n = strtol(nptr, NULL, base);
	return n < min ? min : n > max ? max : n;
}

static void child_config(char *key, char *val)
{
	if (strcmp(key, "program") == 0)
		strncpy(term.cfg.shell, val, sizeof(term.cfg.shell) - 1);
}

static void window_config(char *key, char *val)
{
	if (strcmp(key, "opacity") == 0)
		term.cfg.opacity = cfg_num(val, 10, 0, 255);
	else if (strcmp(key, "margin") == 0)
		term.cfg.margin = strcmp(val, "yes") == 0;
}

static void terminal_config(char *key, char *val)
{
	if (strcmp(key, "rows") == 0)
		term.cfg.row = cfg_num(val, 10, 1, 1000);
	else if (strcmp(key, "columns") == 0)
		term.cfg.col = cfg_num(val, 10, 1, 1000);
	else if (strcmp(key, "scrollback") == 0)
		term.cfg.scrollback = cfg_num(val, 10, 0, UINT_MAX);
}

static void font_config(char *key, char *val)
{
	if (strcmp(key, "size") == 0)
		term.cfg.font_size = cfg_num(val, 10, 6, 300);
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
	uint32_t color = 0;

	if (*val == '#')
		color = cfg_num(++val, 16, 0, 0xffffff);

	if (strcmp(key, "foreground") == 0) {
		set_color(TSM_COLOR_FOREGROUND, color);
	} else if (strcmp(key, "background") == 0) {
		set_color(TSM_COLOR_BACKGROUND, color);
	} else if (strstr(key, "color") == key && *(key + 5) != '\0') {
		char *p;
		long i = strtol(key + 5, &p, 10);
		if (*p == '\0' && i >= 0 && i < 16)
			set_color(i, color);
	}
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

	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f)) {
		key = line;
		while (isblank(*key))
			++key;

		switch (*key) {
		case '\n':
		case '#':
			continue;
		case '[':
			p = strchr(key, ']');
			if (p == NULL)
				continue;
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
			else
				section = NULL;

			continue;
		default:
			val = strchr(key, '=');
			if (val == NULL)
				continue;

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
	kbd_repeat(NULL, NULL, 15, 500);

#define fail(e, s) { fprintf(stderr, s "\n"); goto e; }

	if (font_init(term.cfg.font_size, term.cfg.font_path,
		      &term.cwidth, &term.cheight) < 0)
		fail(efont, "could not load font");

	term.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (term.xkb_ctx == NULL)
		fail(exkb, "failed to create xkb context");

	term.display = wl_display_connect(term.opt.display);
	if (term.display == NULL)
		fail(econnect, "could not connect to display");

	term.registry = wl_display_get_registry(term.display);
	wl_registry_add_listener(term.registry, &reg_listener, NULL);

	wl_display_roundtrip(term.display);
	if (!term.cp || !term.shm)
		fail(eglobals, "missing required globals");
	if (!term.shell)
		fail(eglobals, "your compositor does not support wl_shell,"
			       " maybe you should use the xdg_wm_base havoc");

	wl_display_roundtrip(term.display);
	if (term.shm_argb == false)
		fail(eglobals, "missing required ARGB8888 shm format");

	cursor_init();

	if (tsm_screen_new(&term.screen) < 0)
		fail(etsm, "failed to create tsm screen");
	tsm_screen_set_max_sb(term.screen, term.cfg.scrollback);

	if (tsm_vte_new(&term.vte, term.screen, wcb, NULL) < 0)
		fail(evte, "failed to create tsm vte");
	tsm_vte_set_palette(term.vte, term.cfg.colors);

	term.surf = wl_compositor_create_surface(term.cp);
	if (term.surf == NULL)
		fail(esurf, "could not create surface");
	wl_surface_set_buffer_transform(term.surf, WL_OUTPUT_TRANSFORM_270);

	term.shell_surf = wl_shell_get_shell_surface(term.shell, term.surf);
	if (term.shell_surf == NULL)
		fail(eshellsurf, "could not create wl_shell_surface");
	wl_shell_surface_add_listener(term.shell_surf, &surf_listener, NULL);
	wl_shell_surface_set_maximized(term.shell_surf, NULL);
	wl_shell_surface_set_title(term.shell_surf, "havoc");

	term.ext_surf = qt_surface_extension_get_extended_surface(term.qt_ext, term.surf);
	if (term.ext_surf == NULL)
		fail(eextsurf, "could not create qt_extended_surface");
	qt_extended_surface_add_listener(term.ext_surf, &ext_surf_listener, NULL);
	qt_extended_surface_set_content_orientation_mask(term.ext_surf,
		QT_EXTENDED_SURFACE_ORIENTATION_LANDSCAPEORIENTATION);

	wl_surface_commit(term.surf);
	term.can_redraw = true;

	term.repeat.fd = timerfd_create(CLOCK_MONOTONIC,
					TFD_NONBLOCK | TFD_CLOEXEC);
	if (term.repeat.fd < 0)
		fail(etimer, "could not create key repeat timer: %m");

	if (term.d_dm && term.seat) {
		term.d_d = wl_data_device_manager_get_data_device(
			term.d_dm, term.seat);
		wl_data_device_add_listener(term.d_d, &dd_listener, NULL);
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

	buffer_unmap(&term.buf[0]);
	buffer_unmap(&term.buf[1]);
	if (term.cb)
		wl_callback_destroy(term.cb);

	close(term.repeat.fd);
etimer:
	if (term.d_d)
		wl_data_device_release(term.d_d);
	if (term.ps_d)
		gtk_primary_selection_device_destroy(term.ps_d);
	if (term.ptr)
		wl_pointer_release(term.ptr);
	if (term.kbd)
		wl_keyboard_release(term.kbd);

	qt_extended_surface_destroy(term.ext_surf);
eextsurf:
	wl_shell_surface_destroy(term.shell_surf);
eshellsurf:
	wl_surface_destroy(term.surf);
esurf:
	tsm_vte_unref(term.vte);
evte:
	tsm_screen_unref(term.screen);
etsm:
	cursor_free();
eglobals:
	if (term.ps_dm)
		gtk_primary_selection_device_manager_destroy(term.ps_dm);
	if (term.d_dm)
		wl_data_device_manager_destroy(term.d_dm);
	if (term.seat)
		wl_seat_destroy(term.seat);
	if (term.shell)
		wl_shell_destroy(term.shell);
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
