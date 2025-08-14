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
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>

#define VERSION	"0.0.15"

/*
 * Global definitions
 */
#define MAX_COUNT	0x7FFFFF
#define TAB_SIZE	8

typedef uint8_t	 u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

enum mode 		 { M_NORMAL, M_INSERT, M_COMMAND, M_VISUAL };
const char* MODE_STR[] = { "Normal", "Insert", "Command", "Visual" };

enum optype { OP_NONE, OP_DELETE, OP_YANK, OP_CHANGE };

typedef struct { u32 x, y; } pos_t;
typedef pos_t (*motion_fn)(pos_t start, u32 count);
motion_fn motions[128];

static struct editor {
	u8 mode;
	u8 pending_op;
	u32 pending_count;
	u32 cx, cy;
	u32 rx, ry;
	u32 screen_rows, screen_cols;
	u32 row_offset, col_offset;
	u32 row_count;
	struct erow* rows;
	bool dirty;
	char* filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
} E;

// Special keys
enum {
	RETURN = '\r',
	ESCAPE = '\x1b',
	BACKSPACE = 127,
	ARROW_UP = 1000,
	ARROW_DOWN,
	ARROW_LEFT,
	ARROW_RIGHT,
	DELETE,
	HOME,
	END,
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

struct abuf { char* b; u32 len; };

static void ab_append(struct abuf* ab, const char* s, u32 len) {
	char* new = realloc(ab->b, ab->len + len);
	if (new == NULL) die("realloc");
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

static void ab_free(struct abuf* ab) {
	free(ab->b);
}

struct erow { u32 len, rlen; char *chars, *render; };

static void row_update(struct erow* row) {
	u32 tabs = 0;
	for (u32 i = 0; i < row->len; i++) {
		if (row->chars[i] == '\t') tabs++;
	}

	free(row->render);
	row->render = malloc(row->len + tabs * (TAB_SIZE - 1) + 1);
	if (row->render == NULL) die("malloc");

	u32 j = 0;
	for (u32 i = 0; i < row->len; i++) {
		if (row->chars[i] == '\t') {
			row->render[j++] = ' ';
			while (j % TAB_SIZE != 0) row->render[j++] = ' ';
		} else row->render[j++] = row->chars[i];
	}

	row->render[j] = '\0';
	row->rlen = j;
}

static void row_free(struct erow *row) {
	free(row->render);
	free(row->chars);
}

static void row_insert(u32 at, char* s, u32 len) {
	if (at > E.row_count) at = E.row_count;

	E.rows = realloc(E.rows, sizeof(struct erow) * (E.row_count + 1));
	if (E.rows == NULL) die("realloc");
	memmove(&E.rows[at + 1], &E.rows[at], sizeof(struct erow) * (E.row_count - at));
	
	E.rows[at].len = len;
	E.rows[at].chars = malloc(len + 1);
	memcpy(E.rows[at].chars, s, len);
	E.rows[at].chars[len] = '\0';

	E.rows[at].rlen = 0;
	E.rows[at].render = NULL;
	row_update(&E.rows[at]);

	E.row_count++;
	E.dirty = true;
}

static void row_append_string(struct erow* row, char* s, u32 len) {
	row->chars = realloc(row->chars, row->len + len + 1);
	if (row->chars == NULL) die("realloc");
	memcpy(&row->chars[row->len], s, len);
	row->len += len;
	row->chars[row->len] = '\0';
	row_update(row);
	E.dirty = true;
}

static void row_insert_char(struct erow* row, u32 at, u32 c) {
	if (at > row->len) at = row->len;
	row->chars = realloc(row->chars, row->len + 2);
	memmove(&row->chars[at + 1], &row->chars[at], row->len - at + 1);
	row->len++;
	row->chars[at] = c;
	row_update(row);
	E.dirty = true;
}

static void row_delete(u32 at) {
	if (at >= E.row_count) return;
	row_free(&E.rows[at]);
	memmove(&E.rows[at], &E.rows[at + 1], sizeof(struct erow) * (E.row_count - at - 1));
	E.row_count--;
	E.dirty = true;
}

static void row_delete_char(struct erow* row, u32 at) {
	if (at >= row->len) return;
	memmove(&row->chars[at], &row->chars[at + 1], row->len - at);
	row->len--;
	row_update(row);
	E.dirty = true;
}

static char* rows_to_string(u32* buflen) {
	u32 total = 0;
	for (u32 j = 0; j < E.row_count; j++) total += E.rows[j].len + 1;
	*buflen = total;

	if (total == 0) {
		*buflen = 0;
		return NULL;
	}

	char* buf = malloc(total);
	if (buf == NULL) die("malloc");
	char* p = buf;
	for (u32 j = 0; j < E.row_count; j++) {
		memcpy(p, E.rows[j].chars, E.rows[j].len);
		p += E.rows[j].len;
		*p = '\n';
		p++;
	}

	return buf;
}

static void refresh_screen(void);
static void statusmsg_set(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
	refresh_screen();
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

static u32 read_key(void) {
	u32 nread;
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
					case '1': return HOME;
					case '3': return DELETE;
					case '4': return END;
					case '5': return PAGE_UP;
					case '6': return PAGE_DOWN;
					case '7': return HOME;
					case '8': return END;
					default: return '\x1b';
					}
				}
			}

			switch (seq[1]) {
			case 'A': return ARROW_UP;
			case 'B': return ARROW_DOWN;
			case 'C': return ARROW_RIGHT;
			case 'D': return ARROW_LEFT;
			case 'F': return END;
			case 'H': return HOME;
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
			case 'F': return END;
			case 'H': return HOME;
			}
		} 
	}

	return c;
}

static u8 get_cursor_pos(u32* row, u32* col) {
	char buf[32];
	u32 i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return 1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}

	buf[i] = '\0';
	if (buf[0] != '\x1b' || buf[1] != '[') return 1;
	if (sscanf(&buf[2], "%u;%u", row, col) != 2) return 1;

	return 0;
}

static u8 get_winsize(u32* rows, u32* cols) {
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
 * File I/O
 */

static void open_file(char* filepath) {
	if (!filepath && !E.filename) {
		statusmsg_set("No file name");
		return;
	} else if (filepath) {
		free(E.filename);
		E.filename = strdup(filepath);
	} else filepath = E.filename;

	for (u32 i = 0; i < E.row_count; i++) row_free(&E.rows[i]);
	free(E.rows);
	E.rows = NULL;
	E.row_count = 0;

	E.cx = 0;
	E.cy = 0;

	FILE* fp = fopen(filepath, "r");
	if (fp) {
		char* row = NULL;
		size_t row_cap = 0;
		ssize_t row_len;

		while ((row_len = getline(&row, &row_cap, fp)) != -1) {
			while (row_len > 0 && (row[row_len - 1] == '\n' || row[row_len - 1] == '\r')) row_len--;
			row_insert(E.row_count, row, row_len);
		}
		free(row);
		fclose(fp);
	} else {
		if (errno != ENOENT) statusmsg_set("Could not open %s", strerror(errno));
		else statusmsg_set("New file");
	}

	E.dirty = false;
}

static void save_file(char* filepath) {
	if (!E.filename && !filepath) {
		statusmsg_set("No file name");
		return;
	} else if (!E.filename) E.filename = strdup(filepath);
	else if (!filepath) filepath = E.filename;

	u32 len;
	char* buf = rows_to_string(&len);

	int fd = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (
		fd != -1 &&
		ftruncate(fd, len) != -1 &&
		write(fd, buf, len) == len &&
		fsync(fd) != -1
	   ) {
		statusmsg_set("\"%s\" %uL, %uB written.", filepath, E.row_count, len);
		E.dirty = false;
	} else statusmsg_set("%s", strerror(errno));

	if (fd != -1) close(fd);
	free(buf);
	
}

/*
 * Rendering
 */
static void draw_banner_row(struct abuf* ab, u32 y) {
	if (y != E.screen_rows / 3) {
		ab_append(ab, "~", 1);
		return;
	}

	char msg[80];
	u32 msg_len = snprintf(msg, sizeof(msg), "editor -- version %s", VERSION);
	if (msg_len > E.screen_cols) msg_len = E.screen_cols;
	u32 padding = (E.screen_cols - msg_len) / 2;
	if (padding > 0) {
		ab_append(ab, "~", 1);
		padding--;
	}
	while (padding--) ab_append(ab, " ", 1);
	ab_append(ab, msg, msg_len);
}

static void draw_file_row(struct abuf* ab, u32 file_row) {
	u32 rlen = E.rows[file_row].rlen;
	u32 len = (E.col_offset >= rlen) ? 0 : E.rows[file_row].rlen - E.col_offset;

	char linenr[6];
	u32 linenr_len = snprintf(linenr, sizeof(linenr), "%4u ", file_row + 1);

	if (len + linenr_len > E.screen_cols) len = E.screen_cols;
	
	ab_append(ab, linenr, linenr_len);
	ab_append(ab, &E.rows[file_row].render[E.col_offset], len);
}

static void draw_rows(struct abuf* ab) {
	for (u32 y = 0; y < E.screen_rows; y++) {
		u32 file_row = y + E.row_offset;
		if (E.row_count == 0) draw_banner_row(ab, y);
		else if (file_row >= E.row_count) ab_append(ab, "~", 1);
		else draw_file_row(ab, file_row);

		ab_append(ab, "\x1b[K", 3);
		ab_append(ab, "\r\n", 2);
	}
}

static void draw_statusbar(struct abuf* ab) {
	ab_append(ab, "\x1b[7m", 4);
	char lstatus[80], rstatus[80];

	u32 llen = snprintf(lstatus, sizeof(lstatus), "%c %.20s %s", MODE_STR[E.mode][0], E.filename ? E.filename : "No file", E.dirty ? "[modified]" : "");
	u32 rlen = snprintf(rstatus, sizeof(rstatus), "%u:%u", E.cx + 1, E.cy + 1);

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
	u32 msg_len = strlen(E.statusmsg);
	if (msg_len > E.screen_cols) msg_len = E.screen_cols;
	if (msg_len && time(NULL) - E.statusmsg_time < 5) ab_append(ab, E.statusmsg, msg_len);
}

static void scroll(void) {
	E.rx = E.cx;
	E.ry = E.cy;

	if (E.cy < E.row_count) {
		E.rx = 0;
		for (u32 i = 0; i < E.cx; i++) {
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

	ab_append(&ab, "\x1b[H", 3);
	ab_append(&ab, "\x1b[?25l", 6);

	draw_rows(&ab);
	draw_statusbar(&ab);
	draw_statusmsg(&ab);

	char buf[32];
	u32 len = snprintf(buf, sizeof(buf), "\x1b[%u;%uH", (E.ry - E.row_offset) + 1, E.rx + 6);
	ab_append(&ab, buf, len);
	
	ab_append(&ab, "\x1b[?25h", 6);
	write(STDOUT_FILENO, ab.b, ab.len);
	ab_free(&ab);
}

/*
 * Motions
 */
static pos_t fix_toofar(pos_t p) {
	if (p.y >= E.row_count) p.y = E.row_count ? E.row_count - 1 : 0;
	u32 len = E.rows[p.y].len;
	if (p.x > len) p.x = len;
	return p;
}

static pos_t motion_up(pos_t p, u32 count) {
	u32 dy = (count > p.y) ? p.y : count;
	p.y -= dy;
	return fix_toofar(p);
}

static pos_t motion_down(pos_t p, u32 count) {
	u32 dy = (p.y + count > E.row_count) ? E.row_count - p.y : count;
	p.y += dy;
	return fix_toofar(p);
}

static pos_t motion_left(pos_t p, u32 count) {
	u32 dx = (count > p.x) ? p.x : count;
	p.x -= dx;
	return p;
}

static pos_t motion_right(pos_t p, u32 count) {
	u32 len = (p.y >= E.row_count) ? 0 : E.rows[p.y].len;
	u32 dx = (p.x + count > len) ? len - p.x : count;
	p.x += dx;
	return p;
}

static pos_t motion_home(pos_t p, u32 count) {
	p.x = 0;
	return motion_down(p, count - 1);
}

static pos_t motion_end(pos_t p, u32 count) {
	struct erow* row = (p.y >= E.row_count) ? NULL : &E.rows[p.y];
	p.x = row ? row->len : 0;
	return motion_down(p, count - 1);
}

static pos_t motion_fword(pos_t p, u32 count) { return p; }
static pos_t motion_bword(pos_t p, u32 count) { return p; }
static pos_t motion_fWORD(pos_t p, u32 count) { return p; }
static pos_t motion_bWORD(pos_t p, u32 count) { return p; }

static pos_t motion_file_top(pos_t p, u32 count) {
	(void)count;
	p.y = 0;
	struct erow* row = (p.y >= E.row_count) ? NULL : &E.rows[p.y];
	u32 len = row ? row->len : 0;
	if (p.x > len) p.x = len;
	return fix_toofar(p);
}

static pos_t motion_file_bottom(pos_t p, u32 count) {
	(void)count;
	p.y = E.row_count ? E.row_count - 1 : 0;
	struct erow* row = (p.y >= E.row_count) ? NULL : &E.rows[p.y];
	u32 len = row ? row->len : 0;
	if (p.x > len) p.x = len;
	return fix_toofar(p);
}

static void motions_init(void) {
	memset(motions, 0, sizeof(motions));

	motions['h'] = motion_left;
	motions['j'] = motion_down;
	motions['k'] = motion_up;
	motions['l'] = motion_right;
	motions['_'] = motion_home;
	motions['$'] = motion_end;
	motions['g'] = motion_file_top;
	motions['G'] = motion_file_bottom;
	motions['w'] = motion_fword;
	motions['b'] = motion_bword;
	motions['W'] = motion_fWORD;
	motions['B'] = motion_bWORD;
}

static pos_t run_motion(int key, pos_t start, u32 count) {
	if (key < sizeof(motions)) {
		motion_fn motion = motions[key];
		if (motion) return motion(start, count);
	}

	statusmsg_set("'%c' is not implemented", isprint((u8)key) ? (u8)key : '?');
	return start;
}

// TODO:
static void change_range(pos_t start, pos_t end) {}

static void delete_range(pos_t start, pos_t end) {}

static void yank_range(pos_t start, pos_t end) {}


/*
 * Input handling
 */
static char* prompt(char* msg) {
	u32 cap = 128;
	char* buf = malloc(cap);
	if (buf == NULL) die("malloc");
	u32 len = 0;
	buf[0] = '\0';

	while (1) {
		statusmsg_set(msg, buf);

		u32 c = read_key();
		if (c == ESCAPE) break;
		else if (c == '\r') return buf;
		else if (c == CTRL_KEY('h') || c == BACKSPACE) {
			if (len > 0) buf[--len] = '\0';
			else break;
			continue;
		} else if (!isprint((u8)c)) continue;

		if (len == cap - 1) {
			cap *= 2;
			buf = realloc(buf, cap);
			if (buf == NULL) die("realloc");
		} 

		buf[len++] = (u8)c;
		buf[len] = '\0';
	}

	statusmsg_set("");

	free(buf);
	return NULL;
}

static void eval_command(char* cmd) {
	if (!cmd) return;

	while (*cmd && isspace(*cmd)) cmd++;
	u32 len = strlen(cmd);
	while (len > 0 && isspace(cmd[len - 1])) cmd[--len] = '\0';
	if (*cmd == '\0') return;
	
	char* arg = strchr(cmd, ' ');
	if (arg) {
		*arg++ = '\0';
		while (*arg && isspace(*arg)) arg++;
		if (*arg == '\0') arg = NULL;
	}

	if (strcmp(cmd, "q") == 0) {
		if (E.dirty) statusmsg_set("No write since last change");
		else exit(0);
	} else if (strcmp(cmd, "q!") == 0) {
		exit(0);
	} else if (strcmp(cmd, "w") == 0) {
		save_file(arg);
	} else if (strcmp(cmd, "wq") == 0) {
		save_file(arg);
		exit(0);
	} else if (strcmp(cmd, "e") == 0) {
		if (E.dirty) statusmsg_set("No write since last change");
		else open_file(arg);
	} else if (strcmp(cmd, "e!") == 0) {
		open_file(arg);
	} else statusmsg_set("'%s' is not implemented", cmd);
}

static void insert_char(u32 c) {
	if (E.cy == E.row_count) row_insert(E.row_count, "", 0);
	row_insert_char(&E.rows[E.cy], E.cx, c);
	E.cx++;
}

static void insert_newline(void) {
	if (E.cy > E.row_count) E.cy = E.row_count ? E.row_count - 1 : 0;

	if (E.cx == 0) row_insert(E.cy, "", 0);
	else {
		struct erow* row = &E.rows[E.cy];
		row_insert(E.cy + 1, &row->chars[E.cx], row->len - E.cx);
		row = &E.rows[E.cy];
		row->len = E.cx;
		row->chars[row->len] = '\0';
		row_update(row);
	}
	E.cy++;
	E.cx = 0;
}

static void delete_char(void) {
	if (E.cy >= E.row_count || (E.cx == 0 && E.cy == 0)) return;

	struct erow* row = &E.rows[E.cy];
	if (E.cx > 0) {
		row_delete_char(row, E.cx - 1);
		E.cx--;
	} else {
		E.cx = E.rows[E.cy - 1].len;
		row_append_string(&E.rows[E.cy - 1], row->chars, row->len);
		row_delete(E.cy);
		E.cy--;
	}
}

static void process_normal(u32 c) {
	if (isdigit((u8)c)) {
		u32 digit = c - '0';
		if (E.pending_count == 0 && digit == 0) goto handle_as_motion;
		E.pending_count = E.pending_count * 10 + digit;
		return;
	}

	if (c == 'c') { E.pending_op = OP_CHANGE; return; }
	if (c == 'd') { E.pending_op = OP_DELETE; return; }
	if (c == 'y') { E.pending_op = OP_YANK; return; }

handle_as_motion:
	u32 count = E.pending_count ? E.pending_count : 1;
	E.pending_count = 0;

	pos_t start = { E.cx, E.cy };
	pos_t end = run_motion(c, start, count);

	if (E.pending_op != OP_NONE) {
		switch (E.pending_op) {
		case OP_CHANGE:	change_range(start, end); break;
		case OP_DELETE:	delete_range(start, end); break;
		case OP_YANK:	yank_range(start, end); break;
		}

		E.pending_op = OP_NONE;
	} else {
		E.cx = end.x;
		E.cy = end.y;
	}
}

// TODO: Refactor this mess
static void process_keypress(void) {
	u32 c = read_key();

	if (E.mode == M_NORMAL) switch (c) {
	case 'i': E.mode = M_INSERT; break;
	case 'I': process_normal('_'); E.mode = M_INSERT; break;
	case 'a': process_normal('l'); E.mode = M_INSERT; break;
	case 'A': process_normal('$'); E.mode = M_INSERT; break;
	case ':': E.mode = M_COMMAND; break;
	default: process_normal(c);
	} else if (E.mode == M_INSERT) switch (c) {
	case CTRL_KEY('c'):
	case ESCAPE: E.mode = M_NORMAL; process_normal('h'); break;
	case RETURN: insert_newline(); break;
	case BACKSPACE:
	case CTRL_KEY('h'): delete_char(); break;
	default: if (isprint((u8)c)) insert_char(c); break;
	}

	if (E.mode == M_COMMAND) {
		char* cmd = prompt(":%s");
		eval_command(cmd);
		free(cmd);
		E.mode = M_NORMAL;
	}
}

/*
 * Initialization & cleanup
 */
static void cleanup_editor(void) {
	for (u32 i = 0; i < E.row_count; i++) row_free(&E.rows[i]);
	free(E.rows);
	free(E.filename);
	disable_raw();
	write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
}

static void init_editor(void) {
	memset(&E, 0, sizeof(E));
	enable_raw();
	if (get_winsize(&E.screen_rows, &E.screen_cols) != 0) die("get_winsize");
	E.screen_rows -= 2;
	motions_init();
	atexit(cleanup_editor);
}

int main(int argc, char** argv) {
	init_editor();

	if (argc >= 2) open_file(argv[1]);

	while (1) {
		refresh_screen();
		process_keypress();
	}
}

