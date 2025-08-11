/*
 *
 * editor.c - A simple vi-like editor
 *      (c) 2025 Tomas Kubecek
 *
 */

#include <stdnoreturn.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

const char* VERSION = "0.0.2";

/*
 * Global definitions
 */
static struct editor_config {
	int cx, cy;
	int screen_rows, screen_cols;
	struct termios orig_termios;
} E;

enum editor_key {
	MOVE_UP = 1000,
	MOVE_DOWN,
	MOVE_LEFT,
	MOVE_RIGHT,
	MOVE_HOME,
	MOVE_END,
	PAGE_UP,
	PAGE_DOWN,
};

struct abuf {
	char* b;
	int len;
};
#define ABUF_NEW { NULL, 0 }

/*
 * Generic helpers
 */
#define CTRL_KEY(_k) ((_k) & 0x1F)

noreturn
static void die(const char* s) {
	fprintf(
		stderr,
		"\x1b[2J\x1b[H"
		"%s: %s\n",
		s, strerror(errno)
	       );
	exit(1);
}

void ab_append(struct abuf* ab, const char* s, int len) {
	char* new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void ab_free(struct abuf* ab) {
	free(ab->b);
}

/*
 * Terminal functions
 */
static void disable_raw(void) {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

static void enable_raw(void) {
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
	atexit(disable_raw);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

static int read_key(void) {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
					switch (seq[1]) {
					case '1': return MOVE_HOME;
					case '4': return MOVE_END;
					case '5': return PAGE_UP;
					case '6': return PAGE_DOWN;
					case '7': return MOVE_HOME;
					case '8': return MOVE_END;
					default: return '\x1b';
					}
				}
			}

			switch (seq[1]) {
			case 'A': return MOVE_UP;
			case 'B': return MOVE_DOWN;
			case 'C': return MOVE_RIGHT;
			case 'D': return MOVE_LEFT;
			case 'F': return MOVE_END;
			case 'H': return MOVE_HOME;
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
			case 'F': return MOVE_END;
			case 'H': return MOVE_HOME;
			}
		} 
	}

	return c;
}

static int get_cursor_pos(int* rows, int* cols) {
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return 1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}

	buf[i] = '\0';
	if (buf[0] != '\x1b' || buf[1] != '[') return 1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return 1;

	return 0;
}

static int get_winsize(int* rows, int* cols) {
	struct winsize ws;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return 1;
		return get_cursor_pos(rows, cols);
	}

	*rows = ws.ws_row;
	*cols = ws.ws_col;
	return 0;
}

/*
 * Rendering
 */
static void draw_rows(struct abuf* ab) {
	for (int y = 0; y < E.screen_rows; y++) {
		if (y == E.screen_rows / 3) {
			char msg[64];
			int msg_len = snprintf(msg, sizeof(msg), "editor -- version %s", VERSION);
			if (msg_len > E.screen_cols) msg_len = E.screen_cols;
			int padding = (E.screen_cols - msg_len) / 2;
			if (padding > 0) {
				ab_append(ab, "~", 1);
				padding--;
			}
			while (padding--) ab_append(ab, " ", 1);
			ab_append(ab, msg, msg_len);
		} else ab_append(ab, "~", 1);

		ab_append(ab, "\x1b[K", 3);
		if (y < E.screen_rows - 1) ab_append(ab, "\r\n", 2);
	}
}

static void refresh_screen(void) {
	struct abuf ab = ABUF_NEW;

	ab_append(&ab, "\x1b[?25l", 6);
	ab_append(&ab, "\x1b[H", 3);

	draw_rows(&ab);

	char buf[32];
	int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
	ab_append(&ab, buf, len);
	
	ab_append(&ab, "\x1b[?25h", 6);
	write(STDOUT_FILENO, ab.b, ab.len);
	ab_free(&ab);
}

/*
 * Input handling
 */
static void move_cursor(int key) {
	switch (key) {
	case MOVE_UP:	 if (E.cy != 0) E.cy--; break;
	case MOVE_DOWN:  if (E.cy != E.screen_rows) E.cy++; break;
	case MOVE_LEFT:  if (E.cx != 0) E.cx--; break;
	case MOVE_RIGHT: if (E.cx != E.screen_cols) E.cx++; break;
	}
}

static void process_keypress(void) {
	int c = read_key();

	switch (c) {
	case CTRL_KEY('q'): exit(0); break;
	case MOVE_HOME: E.cx = 0; break;
	case MOVE_END: E.cx = E.screen_cols - 1; break;
	case PAGE_UP:
	case PAGE_DOWN: for (int i = 0; i < E.screen_rows; i++) move_cursor(c == PAGE_UP ? MOVE_UP : MOVE_DOWN); break;
	case MOVE_UP:
	case MOVE_DOWN:
	case MOVE_LEFT:
	case MOVE_RIGHT: move_cursor(c); break;
	}
}

/*
 * Initialization
 */
static void init_editor(void) {
	E.cx = 0;
	E.cy = 0;
	if (get_winsize(&E.screen_rows, &E.screen_cols) != 0) die("get_winsize");
}

int main(void) {
	enable_raw();
	init_editor();

	while (1) {
		refresh_screen();
		process_keypress();
	}
}

