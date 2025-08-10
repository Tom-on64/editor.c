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
noreturn
static void die(const char* s) {
	fprintf(
		stderr,
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

/*
 * Initialization
 */
int main(void) {
	enable_raw();

	while (1) {
		char c = '\0';
		if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");

		if (c == 'q') break;

		if (iscntrl(c)) printf("%d\n", c);
		else printf("%d (%c)", c, c);
	}

	exit(0);
}

