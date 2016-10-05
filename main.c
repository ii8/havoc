
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

#include <libtsm.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include "xdg-shell.h"

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

	int master_fd;

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *cp;
	struct wl_shm *shm;
	struct zxdg_shell_v6 *shell;
	struct wl_seat *seat;

	struct wl_surface *surf;
	struct zxdg_surface_v6 *xdgsurf;
	struct zxdg_toplevel_v6 *toplvl;

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
	int width, height;
	int confheight, confwidth;

	struct tsm_screen *screen;
	struct tsm_vte *vte;
	enum tsm_vte_modifier mods;

	struct xkb_context *xkb_ctx;
	struct xkb_state *xkb_state;
	struct xkb_keymap *xkb_keymap;
	xkb_mod_index_t xkb_alt;
	xkb_mod_index_t xkb_ctrl;
	xkb_mod_index_t xkb_shift;

	struct wl_keyboard *kbd;

	struct {
		int fd;
		xkb_keysym_t sym;
		uint32_t unicode;
		struct itimerspec its;
	} repeat;

	bool has_argb;
	bool has_kbd;

	struct {
		char shell[32];
		int col, row;
		uchr opacity;
		int font_size;
		char font_path[512];
	} cfg;
} term = {
	.cfg.shell = "/bin/sh",
	.cfg.col = 80,
	.cfg.row = 24,
	.cfg.opacity = 0xff,
	.cfg.font_size = 18,
	.cfg.font_path = ""
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
	if (write(term.master_fd, u8, len) < 0)
		abort();
}

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

static void blank(uint32_t *dst, uint w, uint32_t bg)
{
	uint i;
	int h = term.cheight;

	w *= term.cwidth;

	while (h--) {
		for (i = 0; i < w; ++i)
			dst[i] = bg;
		dst += term.width;
	}
}

static void print(uint32_t *dst, uint w, uint32_t bg, uint32_t fg, uchr *glyph)
{
	uint i;
	int h = term.cheight;

	w *= term.cwidth;

	while (h--) {
		for (i = 0; i < w; ++i) {
			if (glyph[i] == 0) {
				dst[i] = bg;
			} else if (glyph[i] == 0xff) {
				dst[i] = fg;
			} else {
				uint_fast32_t a, ba = bg >> 24, fa = glyph[i];
				uint_fast32_t rb = bg & 0x00ff00ff;
				uint_fast32_t g = bg & 0x0000ff00;

				a = ((ba << 8) + (fa << 8) - fa * ba) >> 8;
				rb += ((fg & 0x00ff00ff) - rb) * fa >> 8;
				g += ((fg & 0x0000ff00) - g) * fa >> 8;

				dst[i] = a << 24 | rb & 0xff00ff | g & 0xff00;
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
	uint32_t bg, fg, *dst = buffer->data;

	if (age <= buffer->age && !term.resize)
		return 0;

	dst = &dst[y * term.cheight * term.width + x * term.cwidth];

	if (a->inverse) {
		bg = term.cfg.opacity << 24 | a->fr << 16 | a->fg << 8 | a->fb;
		fg = 0xff000000 | a->br << 16 | a->bg << 8 | a->bb;
	} else {
		bg = term.cfg.opacity << 24 | a->br << 16 | a->bg << 8 | a->bb;
		fg = 0xff000000 | a->fr << 16 | a->fg << 8 | a->fb;
	}

	if (len == 0) {
		blank(dst, char_width, bg);
	} else {
		assert(len == 1);
		uchr *g = get_glyph(id, ch[0], char_width);

		print(dst, char_width, bg, fg, g);
	}

	return 0;
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

static void redraw()
{
	struct buffer *buffer = swap_buffers();

	wl_surface_attach(term.surf, buffer->b, 0, 0);
	buffer->age = tsm_screen_draw(term.screen, draw_cell, buffer);
	wl_surface_damage(term.surf, 0, 0, term.width, term.height);

	term.cb = wl_surface_frame(term.surf);
	wl_callback_add_listener(term.cb, &frame_listener, NULL);
	wl_surface_commit(term.surf);

	buffer->busy = true;
	term.can_redraw = false;
	term.need_redraw = false;
	if (term.resize)
		--term.resize;
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

static void kbd_key(void *data, struct wl_keyboard *k, uint32_t serial,
		    uint32_t time, uint32_t key, uint32_t state)
{
	xkb_keysym_t sym;
	uint32_t unicode;

	sym = xkb_state_key_get_one_sym(term.xkb_state, key + 8);

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

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		if (sym == term.repeat.sym)
			reset_repeat();
	} else if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		unicode = xkb_keysym_to_utf32(sym);

		term.repeat.sym = sym;
		term.repeat.unicode = unicode;
		timerfd_settime(term.repeat.fd, 0, &term.repeat.its, NULL);
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

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	if (caps & WL_SEAT_CAPABILITY_KEYBOARD)
		term.has_kbd = true;
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static struct wl_seat_listener seat_listener = {
	seat_capabilities,
	seat_name
};

static void toplvl_configure(void *data, struct zxdg_toplevel_v6 *xdg_toplevel,
			     int32_t width, int32_t height,
			     struct wl_array *state)
{
	term.configured = false;
	term.confwidth = width ? width : term.cfg.col * term.cwidth;
	term.confheight = height ? height : term.cfg.row * term.cheight;
}

static void toplvl_close(void *data, struct zxdg_toplevel_v6 *t)
{
	term.die = true;
}

static const struct zxdg_toplevel_v6_listener toplvl_listener = {
	toplvl_configure,
	toplvl_close
};

static void configure(void *d, struct zxdg_surface_v6 *surf, uint32_t serial)
{
	zxdg_surface_v6_ack_configure(surf, serial);
	int col = term.confwidth / term.cwidth;
	int row = term.confheight / term.cheight;

	assert(!term.configured);

	if (col && row && (term.col != col || term.row != row)) {
		struct winsize ws = {
			row, col, 0, 0
		};

		tsm_screen_resize(term.screen, col, row);
		if (ioctl(term.master_fd, TIOCSWINSZ, &ws) < 0)
			fprintf(stderr, "failed to resize pty %s",
				strerror(errno));
		term.col = col;
		term.row = row;
		term.width = col * term.cwidth;
		term.height = row * term.cheight;
		term.need_redraw = true;
		term.resize = 2;
	}
	term.configured = true;
}

static const struct zxdg_surface_v6_listener surf_listener = {
	configure
};

static void ping(void *data, struct zxdg_shell_v6 *shell, uint32_t serial)
{
	zxdg_shell_v6_pong(shell, serial);
}

static const struct zxdg_shell_v6_listener shell_listener = {
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
	} else if (strcmp(i, "zxdg_shell_v6") == 0) {
		term.shell = wl_registry_bind(r, id, &zxdg_shell_v6_interface,
					      1);
		zxdg_shell_v6_add_listener(term.shell, &shell_listener, NULL);
	} else if (strcmp(i, "wl_seat") == 0) {
		term.seat = wl_registry_bind(r, id, &wl_seat_interface, 5);
		wl_seat_add_listener(term.seat, &seat_listener, NULL);
	}
}

static void registry_loose(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener reg_listener = {
	registry_get,
	registry_loose
};

static void handle_display(int ev)
{
	if (ev & EPOLLHUP || ev & EPOLLERR) {
		term.die = true;
		return;
	}

	if (ev & EPOLLIN) {
		if (wl_display_dispatch(term.display) < 0) {
			term.die = true;
			return;
		}
	}
}

static void handle_tty(int ev)
{
	char data[256];
	int len;

	if (ev & EPOLLHUP || ev & EPOLLERR) {
		term.die = true;
		return;
	}

	if (ev & EPOLLIN) {
		term.need_redraw = true;
		len = read(term.master_fd, data, sizeof(data));
		if (len < 0)
			term.die = true;
		else
			tsm_vte_input(term.vte, data, len);
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

static struct epcb {
	void (*f)(int);
} dfp = { handle_display }, tfp = { handle_tty }, rfp = { handle_repeat };

#define CONF_FILE "havoc.cfg"

enum section {
	SECTION_GENERAL = 1,
	SECTION_FONT
};

static int clip(int a, int b, int c)
{
	return a < b ? b : a > c ? c : a;
}

static void general_config(char *key, char *val)
{
	if (strcmp(key, "shell") == 0)
		strncpy(term.cfg.shell, val, sizeof(term.cfg.shell) - 1);
	else if (strcmp(key, "opacity") == 0)
		term.cfg.opacity = clip(atoi(val), 0, 255);
	else if (strcmp(key, "rows") == 0)
		term.cfg.row = clip(atoi(val), 1, 300);
	else if (strcmp(key, "columns") == 0)
		term.cfg.col = clip(atoi(val), 1, 600);
}

static void font_config(char *key, char *val)
{
	if (strcmp(key, "size") == 0)
		term.cfg.font_size = clip(atoi(val), 6, 200);
	else if (strcmp(key, "path") == 0)
		strncpy(term.cfg.font_path, val,
			sizeof(term.cfg.font_path) - 1);
}

static FILE *open_config(void)
{
	char *dir;
	char path[512];
	FILE *f;

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
	enum section section = 0;
	int i;

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
				fprintf(stderr, "bad line in config at %d", i);
				continue;
			}
			*p = '\0';
			++key;

			if (strcmp(key, "general") == 0)
				section = SECTION_GENERAL;
			else if (strcmp(key, "font") == 0)
				section = SECTION_FONT;

			continue;
		default:
			val = strchr(key, '=');
			if (val == NULL) {
				fprintf(stderr, "bad line in config at %d", i);
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

			switch (section) {
			case SECTION_GENERAL:
				general_config(key, val);
				break;
			case SECTION_FONT:
				font_config(key, val);
				break;
			}
		}
	}

	fclose(f);
}

int main(int argc, char *argv[])
{
	int fd, display_fd;
	pid_t pid;
	struct epoll_event ee[16];
	int n, i, ret = 1;

	read_config();

	if (font_init(term.cfg.font_size, term.cfg.font_path,
		      &term.cwidth, &term.cheight) < 0)
		goto efont;

	term.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (term.xkb_ctx == NULL)
		goto exkb;

	term.display = wl_display_connect(NULL);
	if (term.display == NULL) {
		fprintf(stderr, "could not connect to display\n");
		goto econnect;
	}

	term.registry = wl_display_get_registry(term.display);
	wl_registry_add_listener(term.registry, &reg_listener, NULL);

	wl_display_roundtrip(term.display);
	if (!term.cp || !term.shm || !term.shell) {
		fprintf(stderr, "missing required globals\n");
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
	tsm_screen_set_max_sb(term.screen, 100);

	if (tsm_vte_new(&term.vte, term.screen, wcb, NULL, log_tsm, NULL) < 0)
		goto evte;

	term.surf = wl_compositor_create_surface(term.cp);
	if (term.surf == NULL)
		goto esurf;
	term.xdgsurf = zxdg_shell_v6_get_xdg_surface(term.shell, term.surf);
	if (term.xdgsurf == NULL)
		goto exdgsurf;
	zxdg_surface_v6_add_listener(term.xdgsurf, &surf_listener, NULL);
	term.toplvl = zxdg_surface_v6_get_toplevel(term.xdgsurf);
	if (term.toplvl == NULL)
		goto etoplvl;
	zxdg_toplevel_v6_add_listener(term.toplvl, &toplvl_listener, NULL);
	zxdg_toplevel_v6_set_title(term.toplvl, "havoc");
	wl_surface_commit(term.surf);
	term.can_redraw = true;

	if (term.seat && term.has_kbd) {
		term.kbd = wl_seat_get_keyboard(term.seat);
		if (term.kbd == NULL)
			goto ekbd;

		wl_keyboard_add_listener(term.kbd, &kbd_listener, NULL);
		term.repeat.fd = timerfd_create(CLOCK_MONOTONIC,
						TFD_NONBLOCK | TFD_CLOEXEC);
		if (term.repeat.fd < 0) {
			perror("failed to create repeat timer");
			goto etimer;
		}
	}

	pid = forkpty(&term.master_fd, NULL, NULL, NULL);
	if (pid < 0) {
		goto etty;
	} else if (pid == 0) {
		setenv("TERM", "xterm-256color", 1);
		if (execl(term.cfg.shell, term.cfg.shell, NULL)) {
			exit(EXIT_FAILURE);
		}
	}
	fcntl(term.master_fd, F_SETFL, O_NONBLOCK);
	display_fd = wl_display_get_fd(term.display);
	fd = epoll_create1(EPOLL_CLOEXEC);

	ee[0].events = EPOLLIN | EPOLLHUP | EPOLLERR;
	ee[0].data.ptr = &dfp;
	epoll_ctl(fd, EPOLL_CTL_ADD, display_fd, &ee[0]);

	ee[0].events = EPOLLIN | EPOLLHUP | EPOLLERR;
	ee[0].data.ptr = &tfp;
	epoll_ctl(fd, EPOLL_CTL_ADD, term.master_fd, &ee[0]);

	ee[0].events = EPOLLIN;
	ee[0].data.ptr = &rfp;
	epoll_ctl(fd, EPOLL_CTL_ADD, term.repeat.fd, &ee[0]);

	while (!term.die) {
		if (term.can_redraw && term.need_redraw && term.configured)
			redraw();

		if (wl_display_flush(term.display) < 0)
			goto eflush;

		n = epoll_wait(fd, ee, 16, -1);
		for (i = 0; i < n; i++) {
			void (*f)(int) = ((struct epcb *)ee[i].data.ptr)->f;
			f(ee[i].events);
		}
	}

	ret = 0;

eflush:
	if (term.buf[0].b) {
		wl_buffer_destroy(term.buf[0].b);
	}
	if (term.buf[1].b) {
		wl_buffer_destroy(term.buf[1].b);
	}
	if (term.cb)
		wl_callback_destroy(term.cb);
etty:
	close(term.repeat.fd);
etimer:
	if (term.kbd)
		wl_keyboard_release(term.kbd);
ekbd:
	zxdg_toplevel_v6_destroy(term.toplvl);
etoplvl:
	zxdg_surface_v6_destroy(term.xdgsurf);
exdgsurf:
	wl_surface_destroy(term.surf);
esurf:
	tsm_vte_unref(term.vte);
evte:
	tsm_screen_unref(term.screen);
etsm:
eglobals:
	if (term.seat)
		wl_seat_destroy(term.seat);
	if (term.shell)
		zxdg_shell_v6_destroy(term.shell);
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
	fprintf(stderr, "Clean exit\n");
	return ret;
}
