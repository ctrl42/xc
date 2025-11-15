/*
 * xc
 * by stx4
 */

#define _POSIX_SOURCE

#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ < 202311L
#include <stdbool.h>
#endif

#define BACKUP_FILE "~/.xcbackup"

/* --- PROGRAM STRUCTURES --- */

typedef enum { COMMAND, INSERT } xc_mode;
enum { UP, DOWN, RIGHT, LEFT, PGUP, PGDN };

typedef enum { 
	DEFAULT, STRING, COMMENT, PREPROCESSOR 
} xc_hl_state_t;

typedef struct {
	int cap;
	int size;
	char* text;
} xc_line_t;

typedef struct {
	int count;
	int scroll;
	xc_line_t* lines;
} xc_buffer_t;

typedef struct {
	bool dirty;
	bool running;
	bool syntax_highlight;

	int scr_w, scr_h;
	int buf_x, buf_y;
	int cur_x, cur_y;

	char* filename;
	char status_line[32];

	xc_mode mode;
	xc_buffer_t buffer;
} xc_state_t;

/* --- SYNTAX SETTINGS --- */

#define TAB_WIDTH 4

#define TYPE_PREFIX         "\e[32m"
#define STRING_PREFIX       "\e[35m"
#define COMMENT_PREFIX      "\e[90m"
#define KEYWORD_PREFIX      "\e[33m"
#define PREPROCESSOR_PREFIX "\e[36m"

char* proc[] = {
	"#include", "#define", "#undef", "#if", "#ifdef",
	"#ifndef", "#error", "#pragma"
};

char* types[] = {
	"void", "int", "char", "float", "long", "short",
	"double", "signed", "unsigned", "_Bool", "const",
	"static", "size_t", "int8_t", "int16_t", "int32_t",
	"int64_t", "uint8_t", "uint16_t", "uint32_t",
	"uint64_t", "bool"
};

char* keywords[] = {
	"auto", "break", "case", "continue", "default",
	"do", "else", "extern", "for", "goto", "if", "inline",
	"register", "return", "sizeof", "switch", "typedef",
	"volatile", "while", "struct", "enum", "true", "false"
};

int proc_len = sizeof(proc) / sizeof(proc[0]);
int types_len = sizeof(types) / sizeof(types[0]);
int keywords_len = sizeof(keywords) / sizeof(keywords[0]);

bool is_delim(char c) {
	switch (c) {
		case ' ': case '(': case ')': case ';':
		case '[': case ']': case '{': case '}':
		case '/': case '%': case '=': case '!':
		case '<': case '>': case '&': case '|':
		case '^': case '~': case '?': case ':':
		case '*': case '\'': case '"': return true;
	}
	return false;
}

/* --- UTILITIES --- */

int count_dig(int n) {
	if (n == 0) return 1;
	if (n < 0) n = -n;

	int count = 0;
	while (n > 0) n /= 10, count++;

	return count;
}

bool is_token(char* token, char** array, int array_len) {
	for (int i = 0; i < array_len; i++)
		if (!strcmp(token, array[i])) return true;
	return false;
}

/* --- KEYBOARD --- */

int get_key(void) {
	char c;
	if (read(0, &c, 1) != 1) return -1;
	if (c != '\e') return c;

	struct timeval tv = {0, 20000};
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(0, &fds);

	int n = select(1, &fds, NULL, NULL, &tv);
	if (n == -1) return -1;
	if (n == 0) return '\e';

	char esc[3];
	if (read(0, &esc[0], 1) != 1) return '\e';
	if (read(0, &esc[1], 1) != 1) return '\e';

	if (esc[0] != '[') return '\e';
	switch (esc[1]) {
		case 'A': case 'B': case 'C': case 'D':
			return esc[1] - 'A';
		case '5': case '6': {
			read(0, &esc[2], 1);
			if (esc[2] != '~') return '\e';
			return esc[1] - '0' - 1;
		}
	}
	return '\e';
}

/* --- CURSOR --- */

void set_cur_x(xc_state_t* state) {
	int i = 0, cur_x = 0;
	int len    = state->buffer.lines[state->buf_y].cap;
	char* line = state->buffer.lines[state->buf_y].text;
	while (i < state->buf_x && i < len) {
		if (line[i] == '\t') cur_x += TAB_WIDTH; else cur_x++;
		i++;
	}
	state->cur_x = cur_x;
}

/* --- STATE DRAWING --- */

void flush_token(char* token, int* tok_len) {
	if (tok_len == 0) return;
	token[*tok_len] = 0;
	if (is_token(token, types, types_len))
		printf(TYPE_PREFIX "%s\e[0m", token);	
	else if (is_token(token, keywords, keywords_len))
		printf(KEYWORD_PREFIX "%s\e[0m", token);
	else if (is_token(token, proc, proc_len))
		printf(PREPROCESSOR_PREFIX "%s\e[0m", token);
	else printf("%s", token);
	*tok_len = 0;
}

void draw_line(xc_state_t* state, int line_num) {
	if (line_num > state->buffer.count) return;

	char* line = state->buffer.lines[line_num].text;
	int len = state->buffer.lines[line_num].cap;

	int tok_len = 0;
	char token[len];
	xc_hl_state_t hl_state = DEFAULT;

	for (int i = 0; line[i]; i++) {
		char c = line[i];
		char n = line[i + 1];
		char p = 0;
		if (i > 0) p = line[i - 1];

		if (c == '\t') {
			printf("%*s", TAB_WIDTH, "");
			continue;
		}

		switch (hl_state) {
		case DEFAULT:
			if (c == '"' && p != '\\' && p != '\'') {
				flush_token(token, &tok_len);
				hl_state = STRING;
				printf(STRING_PREFIX "%c", c);
				continue;
			} else if (c == '#' && n != '\'') {
				hl_state = PREPROCESSOR;
				printf(PREPROCESSOR_PREFIX "%c", c);
				continue;
			} else if ((c == '/' && n == '/')) {
				hl_state = COMMENT;
				printf(COMMENT_PREFIX "%c%c", c, n);
				i++;
				continue;
			}

			if (is_delim(c) || n == 0) {
				token[tok_len] = 0;
				flush_token(token, &tok_len);
				tok_len = 0;

				hl_state = DEFAULT;
				printf("%c", c);
				continue;
			} else {
				token[tok_len++] = c;
			}
			break;
		case STRING:
			printf(STRING_PREFIX "%c", c);
			if (c == '"' && p != '\\') {
				hl_state = DEFAULT;
				printf("\e[0m");
			}
			break;
		case COMMENT:
			printf(COMMENT_PREFIX "%c", c);
			break;
		case PREPROCESSOR:
			printf(PREPROCESSOR_PREFIX "%c", c);
			break;
		}
	}

	flush_token(token, &tok_len);
	putchar('\n');
}

void state_draw(xc_state_t* state) {
	int line_offset = count_dig(state->buffer.count);

	printf("\e[?25l\e[1;1H");
	for (int i = state->buffer.scroll; i < state->scr_h +
		state->buffer.scroll - 1; i++) {
		printf("\33[2K\r");
		if (i >= state->buffer.count) {
			printf("\e[0;90m%.*s\n\e[0m", count_dig(state->buffer.count), 
				"~");
		} else {
			printf("\e[0;%dm%*d\e[0m ", i == state->buf_y ? 39 : 90, 
				line_offset, i + 1);
			draw_line(state, i);
		}
	}

	printf("\e[%d;1H\e[48;5;236m\e[38m%*s\e[%d;1H", state->scr_h + 1,
		state->scr_w, "", state->scr_h);
	sprintf(state->status_line, "%d,%d-%d  %5.1f%%", state->buf_y + 1, 
		state->buf_x, state->cur_x + 1, state->buf_y > 0 ? 
		(double)state->buf_y / (state->buffer.count - 1) * 100.0 : 0.0);
	printf("%s%s    %s\e[%d;%dH%s\e[0m", state->filename,
		state->dirty ? "*" : " ",
		state->mode == INSERT ? "-- insert --" : "", 
		state->scr_h + 1, (int)(state->scr_w - strlen(state->status_line) 
		+ 1), state->status_line);
	printf("\e[%d;%dH", state->cur_y + 1, state->cur_x + line_offset + 2);
	/*                                    cursor x     + line number + 1 
	 *                                    for ANSI, 1 for space */

	printf("\e[?25h");
	fflush(stdout);
}

/* --- STATE HANDLERS --- */

bool state_init(xc_state_t* state, char* path) {
	int fd = open(path, O_RDONLY);
	if (!fd) {
		fd = open(path, O_RDWR | O_CREAT, 0644);
		if (fd) while(1);
		if (!fd) return false;
	}

	struct stat st;
	stat(path, &st);

	char* raw_buffer = malloc(st.st_size);
	int rd = read(fd, raw_buffer, st.st_size);
	if (rd != st.st_size) {
		free(raw_buffer);
		return false;
	}

	state->buffer.count = 0;
	state->buffer.scroll = 0;
	for (int i = 0; i < st.st_size; i++)
		if (raw_buffer[i] == '\n') state->buffer.count++;
	
	int last = 0, cur_line = 0;
	state->buffer.lines = calloc(state->buffer.count, sizeof(xc_line_t));
	for (int i = 0; i < st.st_size; i++) {
		if (raw_buffer[i] == '\n') {
			int len = i - last;
			xc_line_t* cur = &state->buffer.lines[cur_line];
			cur->cap = len;
			cur->size = len + 32;
			cur->text = calloc(1, cur->size);
			memcpy(cur->text, raw_buffer + last, len);

			last = i + 1, cur_line++;
		}
	}

	free(raw_buffer);

	if (state->buffer.count == 0) {
		state->buffer.count = 1;
		state->buffer.lines = malloc(sizeof(xc_line_t));

		xc_line_t* line = &state->buffer.lines[0];
		line->cap = 0;
		line->size = 32;
		line->text = calloc(line->size, 1);
	}

	state->mode = COMMAND;
	state->dirty = false;
	state->running = true;
	char* extension = strrchr(path, '.');
	/* C is pure */
	if (extension && extension + 1 != 0 && !strcmp(extension + 1, "c"))
		state->syntax_highlight = true;

	struct winsize ws;
	ioctl(0, TIOCGWINSZ, &ws);
	state->scr_w = ws.ws_col;
	state->scr_h = ws.ws_row;

	state->buf_x = 0;
	state->buf_y = 0;

	state->cur_x = 0;
	state->cur_y = 0;

	state->filename = strdup(path);
	printf("\e[?1049h");
	return true;
}

void state_trash(xc_state_t* state) {
	for (int i = 0; i < state->buffer.count; i++)
		free(state->buffer.lines[i].text);
	free(state->buffer.lines);
	free(state->filename);

	printf("\e[?1049l");
}

bool state_write(xc_state_t* state, char* filename) {
	int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return false;

	for (int i = 0; i < state->buffer.count; i++) {
		xc_line_t* line = &state->buffer.lines[i];
		int res = write(fd, line->text, line->cap);
		if (res != line->cap) {
			close(fd);
			return false;
		}
		write(fd, (char[]){'\n'}, 1);
	}

	close(fd);
	state->dirty = false;

	return true;
}

void state_insert(xc_state_t* state, char c) {
	xc_line_t* line = &state->buffer.lines[state->buf_y];

	switch (c) {
	case '\b': case 0x7F: {
		if (state->buf_x == 0) {
			if (state->buf_y == 0) break;

			xc_buffer_t* buf = &state->buffer;
			xc_line_t* prev = &buf->lines[state->buf_y - 1];
			
			int new_buf_x = prev->cap;
			prev->text = realloc(prev->text, prev->cap + line->cap + 32);
			memcpy(prev->text + prev->cap, line->text, line->cap);
			prev->cap += line->cap;
			prev->text[prev->cap] = 0;

			free(line->text);
			memmove(&buf->lines[state->buf_y],
				&buf->lines[state->buf_y + 1],
				sizeof(xc_line_t) * (buf->count - state->buf_y - 1));
			buf->count--;

			state->buf_y--;
			state->buf_x = new_buf_x;
		} else {
			memmove(&line->text[state->buf_x - 1],
				&line->text[state->buf_x],
				line->cap - state->buf_x + 1);
			line->cap--;
			state->buf_x--;
		}
		break;
	}
	case '\r': case '\n': {
		xc_buffer_t* buf = &state->buffer;
		buf->lines = realloc(buf->lines, sizeof(xc_line_t) *
			(buf->count + 1));

		memmove(&buf->lines[state->buf_y + 2],
			&buf->lines[state->buf_y + 1],
			sizeof(xc_line_t) * (buf->count - state->buf_y - 1));

		/* MEMORY HAS SHIFTED! */
		line = &buf->lines[state->buf_y];

		xc_line_t* new_line = &buf->lines[state->buf_y + 1];
		int broken_len = line->cap - state->buf_x;
		new_line->size = broken_len + 32;
		new_line->cap  = broken_len;
		new_line->text = malloc(new_line->size);
		memcpy(new_line->text, &line->text[state->buf_x], broken_len);
		new_line->text[broken_len] = 0;

		line->cap = state->buf_x;
		line->text[state->buf_x] = 0;

		buf->count++;
		state->buf_y++;
		state->buf_x = 0;
		break;
	}
	default: {
		if (!isprint(c) && c != '\t') break;
		if (line->cap + 2 >= line->size) {
			line->size *= 2;
			line->text = realloc(line->text, line->size);
		}
		memmove(&line->text[state->buf_x + 1], 
			&line->text[state->buf_x],
			line->cap - state->buf_x + 1);
		line->text[state->buf_x] = c;
		line->cap++;
		state->buf_x++;
		break;
	}
	case '\e':
		state->mode = COMMAND;
		state_write(state, BACKUP_FILE);
		break;
	}

	state->dirty = true;
}

void state_command(xc_state_t* state, char c) {
	switch (c) {
	case 'i': state->mode = INSERT; break;
	case 'q': if (!state->dirty) state->running = false; break;
	case 'w': state_write(state, state->filename); break;
	case 's': state_write(state, state->filename); 
			  state->running = false; break;

	case '.': state->buffer.scroll = state->buffer.count - 
		state->scr_h + 1, state->buf_y = state->buffer.count - 1; break;
	case ',': state->buffer.scroll = 0, state->buf_y = 0; break;

	case ';': state->buf_x = 0; break;
	case '\'': state->buf_x = INT_MAX; break;
	}
}

#define CUR_Y (state->buf_y - state->buffer.scroll)

void state_step(xc_state_t* state) {
	char c = get_key();
	int cur_y = CUR_Y;
	
	switch (c) {
	case UP:
		if (cur_y <= 0) state->buffer.scroll--;
		state->buf_y--;
		break;
	case DOWN:
		if (cur_y >= state->scr_h - 2) state->buffer.scroll++, 
			state->buf_y++;
		else state->buf_y++;
		break;
	case LEFT:
		state->buf_x--;
		break;
	case RIGHT:
		state->buf_x++;
		break;
	case PGUP:
		state->buffer.scroll -= state->scr_h - 1; 
		state->buf_y -= state->scr_h - 1;
		break;
	case PGDN:
		state->buffer.scroll += state->scr_h - 1;
		state->buf_y += state->scr_h - 1;
		break;
	}

	/* clamp */
	if (state->buffer.scroll < 0) state->buffer.scroll = 0;
	if (state->buffer.scroll > state->buffer.count - 1) 
		state->buffer.scroll = state->buffer.count - 1;

	if (state->buf_y < 0) state->buf_y = 0;
	if (state->buf_y >= state->buffer.count - 1) state->buf_y = 
		state->buffer.count - 1;

	int line_len = state->buffer.lines[state->buf_y].cap;
	if (state->buf_x < 0) state->buf_x = 0;
	if (state->buf_x > line_len) state->buf_x = line_len;

	/* handle insert/command */
	switch (state->mode) {
		case INSERT:  state_insert(state, c);  break;
		case COMMAND: state_command(state, c); break;
	}

	/* another clamp for command mode safety */
	line_len = state->buffer.lines[state->buf_y].cap;
	if (state->buf_x < 0) state->buf_x = 0;
	if (state->buf_x > line_len) state->buf_x = line_len;

	/* set cursor position */
	set_cur_x(state);
	state->cur_y = CUR_Y;

	/* FUCKIN REDRAW (i forgot this early rewrite) */
	state_draw(state);
}

int main(int argc, char* argv[]) {
	struct termios attr;
	tcgetattr(0, &attr);
	attr.c_lflag &= ~(ICANON | ECHO);
	tcsetattr(0, TCSANOW, &attr);

	xc_state_t state;
	for (int i = 1; i < argc; i++) {
		memset(&state, 0, sizeof(xc_state_t));
		if (!state_init(&state, argv[i])) {
			fprintf(stderr, "failed to init for file: %s\n", argv[i]);
			continue;
		}

		state_draw(&state);
		while (state.running) state_step(&state);
		state_trash(&state);
	}

	attr.c_lflag |= (ICANON | ECHO);
	tcsetattr(0, TCSANOW, &attr);
	return 0;
}
