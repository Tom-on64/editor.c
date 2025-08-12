/*
 *
 * editor.c - A simple vi-like editor
 *      (c) 2025 Tomas Kubecek
 *
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdnoreturn.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>

#define VERSION	"0.0.7"

/*
 * Global definitions
 */
// TODO: Make these configurable
#define TAB_SIZE	8

static struct editor {
	int cx, cy;
	int rx, ry;
	int screen_rows, screen_cols;
	int row_offset, col_offset;
	int row_count;
	struct erow* rows;
	char* filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
} E;

enum editor_key {
	ENTER = '\r',
	ESCAPE = '\x1b',
	BACKSPACE = 127,
	MOVE_UP = 1000,
	MOVE_DOWN,
	MOVE_LEFT,
	MOVE_RIGHT,
	DELETE,
	MOVE_HOME,
	MOVE_END,
	PAGE_UP,
	PAGE_DOWN,
};

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

struct abuf { char* b; int len; };

static void ab_append(struct abuf* ab, const char* s, int len) {
	char* new = realloc(ab->b, ab->len + len);
	if (new == NULL) die("realloc");
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

static void ab_free(struct abuf* ab) {
	free(ab->b);
}

struct erow { int len, rlen; char *chars, *render; };

static void row_update(struct erow* row) {
	int tabs = 0;
	for (int i = 0; i < row->len; i++) {
		if (row->chars[i] == '\t') tabs++;
	}

	free(row->render);
	row->render = malloc(row->len + tabs * (TAB_SIZE - 1) + 1);

	int j = 0;
	for (int i = 0; i < row->len; i++) {
		if (row->chars[i] == '\t') {
			row->render[j++] = ' ';
			while (j % TAB_SIZE != 0) row->render[j++] = ' ';
		} else row->render[j++] = row->chars[i];
	}

	row->render[j] = '\0';
	row->rlen = j;
}

static void row_append(char* s, size_t len) {
	E.rows = realloc(E.rows, sizeof(struct erow) * (E.row_count + 1));
	if (E.rows == NULL) die("realloc");
	
	int at = E.row_count;
	E.rows[at].len = len;
	E.rows[at].chars = malloc(len + 1);
	memcpy(E.rows[at].chars, s, len);
	E.rows[at].chars[len] = '\0';

	E.rows[at].rlen = 0;
	E.rows[at].render = NULL;
	row_update(&E.rows[at]);

	E.row_count++;
}

static void row_insert(struct erow* row, int at, int c) {
	if (at < 0 || at > row->len) at = row->len;
	row->chars = realloc(row->chars, row->len + 2);
	memmove(&row->chars[at + 1], &row->chars[at], row->len - at + 1);
	row->len++;
	row->chars[at] = c;
	row_update(row);
}

static char* rows_to_string(int* buflen) {
	int total = 0;

	for (int j = 0; j < E.row_count; j++) total += E.rows[j].len + 1;
	*buflen = total;

	char* buf = malloc(total);
	char* p = buf;
	for (int j = 0; j < E.row_count; j++) {
		memcpy(p, E.rows[j].chars, E.rows[j].len);
		p += E.rows[j].len;
		*p = '\n';
		p++;
	}

	return buf;
}

static void statusmsg_set(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

/*
 * Terminal functions
 */
static void disable_raw(void) {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

static void enable_raw(void) {
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");

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

	// TODO: Make this escape bullshit parsing prettier
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
					case '3': return DELETE;
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

static int get_cursor_pos(int* row, int* col) {
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
	if (sscanf(&buf[2], "%d;%d", row, col) != 2) return 1;

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

static void open_file(const char* filepath) {
	FILE* fp = fopen(filepath, "r");
	if (!fp) die("fopen");

	free(E.filename);
	E.filename = strdup(filepath);

	char* row = NULL;
	size_t row_cap = 0;
	ssize_t row_len;

	while ((row_len = getline(&row, &row_cap, fp)) != -1) {
		while (row_len > 0 && (row[row_len - 1] == '\n' || row[row_len - 1] == '\r')) row_len--;
		row_append(row, row_len);
	}
	free(row);
	fclose(fp);
}

static void save_file(void) {
	if (E.filename == NULL) {
		statusmsg_set("No filename");
		return;
	}

	int len;
	char* buf = rows_to_string(&len);

	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if (
		fd != -1 &&
		ftruncate(fd, len) != -1 &&
		write(fd, buf, len) == len
	   ) statusmsg_set("\"%s\" %dL, %dB written.", E.filename, E.row_count, len);
	else statusmsg_set("%s", strerror(errno));

	if (fd != -1) close(fd);
	free(buf);
}

/*
 * Rendering
 */
static void draw_banner_row(struct abuf* ab, int y) {
	if (y != E.screen_rows / 3) {
		ab_append(ab, "~", 1);
		return;
	}

	char msg[80];
	int msg_len = snprintf(msg, sizeof(msg), "editor -- version %s", VERSION);
	if (msg_len > E.screen_cols) msg_len = E.screen_cols;
	int padding = (E.screen_cols - msg_len) / 2;
	if (padding > 0) {
		ab_append(ab, "~", 1);
		padding--;
	}
	while (padding--) ab_append(ab, " ", 1);
	ab_append(ab, msg, msg_len);
}

static void draw_file_row(struct abuf* ab, int file_row) {
	int len = E.rows[file_row].rlen - E.col_offset;

	if (len < 0) len = 0;
	if (len > E.screen_cols) len = E.screen_cols;
	
	ab_append(ab, &E.rows[file_row].render[E.col_offset], len);
}

static void draw_statusbar(struct abuf* ab) {
	ab_append(ab, "\x1b[7m", 4);
	char lstatus[80], rstatus[80];

	int llen = snprintf(lstatus, sizeof(lstatus), "- %.20s - %d lines", E.filename ? E.filename : "No file", E.row_count);
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d:%d", E.cx + 1, E.cy + 1);

	if (llen > E.screen_cols) llen = E.screen_cols;
	ab_append(ab, lstatus, llen);

	while (llen < E.screen_cols) {
		if (E.screen_cols - llen == rlen) {
			ab_append(ab, rstatus, rlen);
			break;
		}

		ab_append(ab, " ", 1);
		llen++;
	}

	ab_append(ab, "\x1b[m", 3);
	ab_append(ab, "\r\n", 2);
}

static void draw_statusmsg(struct abuf* ab) {
	ab_append(ab, "\x1b[K", 3);
	int msg_len = strlen(E.statusmsg);
	if (msg_len > E.screen_cols) msg_len = E.screen_cols;
	if (msg_len && time(NULL) - E.statusmsg_time < 5) ab_append(ab, E.statusmsg, msg_len);
}

static void draw_rows(struct abuf* ab) {
	for (int y = 0; y < E.screen_rows; y++) {
		int file_row = y + E.row_offset;
		if (E.row_count == 0) draw_banner_row(ab, y);
		else if (file_row >= E.row_count) ab_append(ab, "~", 1);
		else draw_file_row(ab, file_row);

		ab_append(ab, "\x1b[K", 3);
		ab_append(ab, "\r\n", 2);
	}
}

static void scroll(void) {
	E.rx = E.cx;
	E.ry = E.cy;

	if (E.rx < E.row_count) {
		E.rx = 0;
		for (int i = 0; i < E.cx; i++) {
			if (E.rows[E.cy].chars[i] == '\t') E.rx += (TAB_SIZE - 1) - (E.rx % TAB_SIZE);
			E.rx++;
		}
	}

	if (E.ry < E.row_offset) E.row_offset = E.ry;
	if (E.ry >= E.row_offset + E.screen_rows) E.row_offset = E.ry - E.screen_rows + 1;
	if (E.rx < E.col_offset) E.col_offset = E.rx;
	if (E.rx >= E.col_offset + E.screen_cols) E.col_offset = E.rx - E.screen_cols + 1;
}

static void refresh_screen(void) {
	scroll();

	struct abuf ab = { 0 };

	ab_append(&ab, "\x1b[?25l", 6);
	ab_append(&ab, "\x1b[H", 3);

	draw_rows(&ab);
	draw_statusbar(&ab);
	draw_statusmsg(&ab);

	char buf[32];
	int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.ry - E.row_offset) + 1, E.rx + 1);
	ab_append(&ab, buf, len);
	
	ab_append(&ab, "\x1b[?25h", 6);
	write(STDOUT_FILENO, ab.b, ab.len);
	ab_free(&ab);
}

/*
 * Input handling
 */
static void insert_char(int c) {
	if (E.cy == E.row_count) row_append("", 0);
	row_insert(&E.rows[E.cy], E.cx, c);
	E.cx++;
}

static void move_cursor(int key) {
	struct erow* row = (E.cy >= E.row_count) ? NULL : &E.rows[E.cy];

	switch (key) {
	case MOVE_UP:	 if (E.cy != 0) E.cy--; break;
	case MOVE_DOWN:  if (E.cy < E.row_count - 1) E.cy++; break;
	case MOVE_LEFT:  if (E.cx != 0) E.cx--; break;
	case MOVE_RIGHT: if (row && E.cx < row->len) E.cx++; break;
	}

	row = (E.cy >= E.row_count) ? NULL : &E.rows[E.cy];
	int row_len = row ? row->len : 0;
	if (E.cx > row_len) E.cx = row_len;
}

static void process_keypress(void) {
	int c = read_key();

	switch (c) {
	case CTRL_KEY('q'): exit(0); break;
	case CTRL_KEY('c'): statusmsg_set("Press ^Q to quit."); break;
	case CTRL_KEY('s'): save_file(); break;
	case BACKSPACE:
	case CTRL_KEY('h'):
	case DELETE:
		break;
	case ENTER:
		break;
	case CTRL_KEY('l'):
	case ESCAPE:
		break;
	case MOVE_HOME: E.cx = 0; break;
	case MOVE_END: E.cx = E.rows ? E.rows[E.cy].len : 0; break;
	case PAGE_UP:
	case PAGE_DOWN:
	{
		E.cy = E.row_offset;
		if (c == PAGE_DOWN) {
			E.cy += E.screen_rows - 1; 
			if (E.cy > E.row_count) E.cy = E.row_count;
		}

		for (int i = 0; i < E.screen_rows; i++) move_cursor(c == PAGE_UP ? MOVE_UP : MOVE_DOWN);
	} break;
	case MOVE_UP:
	case MOVE_DOWN:
	case MOVE_LEFT:
	case MOVE_RIGHT: move_cursor(c); break;
	default: insert_char(c); break;
	}

}

/*
 * Initialization
 */
static void cleanup_editor(void) {
	if (E.filename) free(E.filename);
	for (int i = 0; i < E.row_count; i++) {
		if (E.rows[i].chars) free(E.rows[i].chars);
		if (E.rows[i].render) free(E.rows[i].render);
	}
	if (E.rows) free(E.rows);

	disable_raw();
}

static void init_editor(void) {
	enable_raw();
	atexit(cleanup_editor);

	memset(&E, 0, sizeof(E));
	if (get_winsize(&E.screen_rows, &E.screen_cols) != 0) die("get_winsize");
	E.screen_rows -= 2;
}

int main(int argc, char** argv) {
	init_editor();

	if (argc >= 2) open_file(argv[1]);

	while (1) {
		refresh_screen();
		process_keypress();
	}
}

