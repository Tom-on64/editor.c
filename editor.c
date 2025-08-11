/*
 *
 * editor.c - A simple vi-like editor
 *      (c) 2025 Tomas Kubecek
 *
 */

#include <stdnoreturn.h>
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
static struct termios orig_termios;

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
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) die("tcsetattr");
}

static void enable_raw(void) {
	if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
	atexit(disable_raw);

	struct termios raw = orig_termios;
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

/*
 * Rendering
 */
static void refresh_screen(void) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
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
int main(void) {
	enable_raw();

	while (1) {
		refresh_screen();
		process_keypress();
	}
}

