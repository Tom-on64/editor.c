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
#include <ctype.h>
#include <errno.h>
#include <stdio.h>

/*
 * Global variables
 */
static struct editor_config {
	int screen_rows, screen_cols;
	struct termios orig_termios;
} E;

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

static char read_key(void) {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
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
static void draw_rows(void) {
	for (int y = 0; y < E.screen_rows; y++) {
		write(STDOUT_FILENO, "~", 1);

		if (y < E.screen_rows - 1) write(STDOUT_FILENO, "\r\n", 2);
	}
}

static void refresh_screen(void) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	draw_rows();
}

/*
 * Input handling
 */
static void process_keypress(void) {
	char c = read_key();

	switch (c) {
	case CTRL_KEY('q'): exit(0); break;
	}
}

/*
 * Initialization
 */
static void init_editor(void) {
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

