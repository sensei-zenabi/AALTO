#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>

/*
 * Design principles and notes:
 * - Plain C using -std=c11 and only standard libraries.
 * - No separate header files.
 * - Selection mode is toggled with CTRL+T.
 * - "Select all" functionality with CTRL+A selects the entire text buffer.
 * - When a selection is active, pressing Backspace (or CTRL+H) or Delete deletes the selection.
 * - The Delete key is implemented as a separate case (DEL_KEY).
 * - New keys Home, End, Page Up, and Page Down have been added for quick navigation.
 * - Vertical movement now remembers a preferred horizontal position.
 * - New Search Functionality: CTRL+F is bound to a search that lets the user input a query,
 *   scans the text buffer for matches, and displays a simple full‐screen menu to choose a match.
 */

#define EDITOR_VERSION "0.1-micro-like"

#define CTRL_KEY(k) ((k) & 0x1f)
#define BACKSPACE 127
#define DEL_KEY 1004  // Key code for Delete

/* Enumeration for editor keys.
   New keys added:
     HOME_KEY: Home key (beginning of line).
     END_KEY: End key (end of line).
     PGUP_KEY: Page Up key.
     PGDN_KEY: Page Down key.
*/
enum EditorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,    // 1001
    ARROW_UP,       // 1002
    ARROW_DOWN,     // 1003
    HOME_KEY = 1005, // explicitly set to 1005
    END_KEY,        // 1006
    PGUP_KEY,       // 1007
    PGDN_KEY        // 1008
};

/* Data structure for a text line */
typedef struct {
    int size;
    char *chars;
    int modified;
} EditorLine;

/* Global clipboard for cut/copy/paste functionality */
char *clipboard = NULL;
size_t clipboard_len = 0;

/* Global editor state */
struct EditorConfig {
    int cx, cy;           // cursor position
    int screenrows;       // terminal rows
    int screencols;       // terminal columns
    int rowoff;           // vertical scroll offset
    int coloff;           // horizontal scroll offset
    int numrows;          // number of rows in the file
    EditorLine *row;      // array of rows
    char *filename;
    int dirty;            // unsaved changes flag
    struct termios orig_termios; // original terminal settings

    char status_message[80];
    int textrows;         // text area rows (will be screenrows - 2)

    /* Selection fields:
       CTRL+T toggles selection mode.
       When active, the selection anchor is stored in sel_anchor_x, sel_anchor_y;
       the current cursor is the selection end.
    */
    int selecting;
    int sel_anchor_x;
    int sel_anchor_y;

    /* Preferred horizontal (desired) column */
    int preferred_cx;
} E;

/* Undo state structure */
typedef struct {
    int cx, cy;
    int numrows;
    EditorLine *row;
} UndoState;

UndoState *undo_history[100];
int undo_history_len = 0;

/* --- Function Prototypes --- */
void die(const char *s);
void disableRawMode(void);
void enableRawMode(void);
int editorReadKey(void);
int getWindowSize(int *rows, int *cols);
void editorRefreshScreen(void);
int getRowNumWidth(void);
int editorDisplayWidth(const char *s);
int editorRowCxToByteIndex(EditorLine *row, int cx);
void editorRenderRow(EditorLine *row, int avail);
void editorRenderRowWithSelection(EditorLine *row, int file_row, int avail);
void editorDrawRows(int rn_width);
void editorDrawStatusBar(void);
void editorDrawShortcutBar(void);
void editorProcessKeypress(void);
void editorOpen(const char *filename);
void editorSave(void);
void editorAppendLine(char *s, size_t len);
void editorInsertChar(int c);
void editorInsertUTF8(const char *s, int len);
void editorInsertNewline(void);
void editorDelChar(void);
void push_undo_state(void);
void free_undo_state(UndoState *state);
void pop_undo_state(void);
void editorCopySelection(void);
void editorCutSelection(void);
void editorPasteClipboard(void);
void editorInsertString(const char *s);

/* New function prototypes */
void editorDeleteSelection(void);
void editorDelCharAtCursor(void);
void editorSearch(void);

/* --- Helper Functions --- */

int getRowNumWidth(void) {
    int n = E.numrows, digits = 1;
    while (n >= 10) { n /= 10; digits++; }
    return digits + 1;
}

int editorDisplayWidth(const char *s) {
    int width = 0;
    size_t bytes;
    wchar_t wc;
    while (*s) {
        bytes = mbrtowc(&wc, s, MB_CUR_MAX, NULL);
        if (bytes == (size_t)-1 || bytes == (size_t)-2) { bytes = 1; wc = *s; }
        int w = wcwidth(wc);
        if (w < 0) w = 0;
        width += w; s += bytes;
    }
    return width;
}

int editorRowCxToByteIndex(EditorLine *row, int cx) {
    int cur_width = 0, index = 0;
    size_t bytes;
    wchar_t wc;
    while (index < row->size) {
        bytes = mbrtowc(&wc, row->chars + index, MB_CUR_MAX, NULL);
        if (bytes == (size_t)-1 || bytes == (size_t)-2) { bytes = 1; wc = row->chars[index]; }
        int w = wcwidth(wc);
        if (w < 0) w = 0;
        if (cur_width + w > cx)
            break;
        cur_width += w; index += bytes;
    }
    return index;
}

void editorRenderRow(EditorLine *row, int avail) {
    int logical_width = 0, byte_index = editorRowCxToByteIndex(row, E.coloff);
    char buffer[1024]; int buf_index = 0;
    size_t bytes;
    wchar_t wc;
    while (byte_index < row->size && logical_width < avail) {
        bytes = mbrtowc(&wc, row->chars + byte_index, MB_CUR_MAX, NULL);
        if (bytes == (size_t)-1 || bytes == (size_t)-2)
            bytes = 1;
        int w = wcwidth(wc);
        if (w < 0) w = 0;
        if (logical_width + w > avail)
            break;
        for (int j = 0; j < (int)bytes; j++) {
            if (buf_index < (int)sizeof(buffer) - 1)
                buffer[buf_index++] = row->chars[byte_index + j];
        }
        logical_width += w; byte_index += bytes;
    }
    buffer[buf_index] = '\0';
    write(STDOUT_FILENO, buffer, buf_index);
}

void editorRenderRowWithSelection(EditorLine *row, int file_row, int avail) {
    int logical_width = 0, byte_index = editorRowCxToByteIndex(row, E.coloff);
    size_t bytes;
    wchar_t wc;
    int current_disp = 0;
    int selection_active_for_row = 0, sel_local_start = 0, sel_local_end = 0;
    if (E.selecting) {
        int start_line = (E.sel_anchor_y < E.cy ? E.sel_anchor_y : E.cy);
        int end_line = (E.sel_anchor_y > E.cy ? E.sel_anchor_y : E.cy);
        if (file_row >= start_line && file_row <= end_line) {
            selection_active_for_row = 1;
            if (start_line == end_line) {
                sel_local_start = (E.sel_anchor_x < E.cx ? E.sel_anchor_x : E.cx);
                sel_local_end   = (E.sel_anchor_x < E.cx ? E.cx   : E.sel_anchor_x);
            } else if (file_row == start_line) {
                if (E.sel_anchor_y < E.cy) {
                    sel_local_start = E.sel_anchor_x;
                    sel_local_end = editorDisplayWidth(row->chars);
                } else {
                    sel_local_start = 0;
                    sel_local_end = E.sel_anchor_x;
                }
            } else if (file_row == end_line) {
                if (E.sel_anchor_y < E.cy) {
                    sel_local_start = 0;
                    sel_local_end = E.cx;
                } else {
                    sel_local_start = E.cx;
                    sel_local_end = editorDisplayWidth(row->chars);
                }
            } else {
                sel_local_start = 0;
                sel_local_end = editorDisplayWidth(row->chars);
            }
        }
    }
    int eff_sel_start = 0, eff_sel_end = 0;
    if (selection_active_for_row) {
        eff_sel_start = (sel_local_start < E.coloff ? 0 : sel_local_start - E.coloff);
        eff_sel_end = sel_local_end - E.coloff;
        if (eff_sel_end < 0) eff_sel_end = 0;
        if (eff_sel_start < 0) eff_sel_start = 0;
        if (eff_sel_end > avail) eff_sel_end = avail;
    }
    int in_selection = 0;
    while (byte_index < row->size && logical_width < avail) {
        bytes = mbrtowc(&wc, row->chars + byte_index, MB_CUR_MAX, NULL);
        if (bytes == (size_t)-1 || bytes == (size_t)-2)
            bytes = 1;
        int w = wcwidth(wc);
        if (w < 0) w = 0;
        if (selection_active_for_row && current_disp >= eff_sel_start && current_disp < eff_sel_end) {
            if (!in_selection) { write(STDOUT_FILENO, "\x1b[7m", 4); in_selection = 1; }
        } else {
            if (in_selection) { write(STDOUT_FILENO, "\x1b[0m", 4); in_selection = 0; }
        }
        if (logical_width + w > avail)
            break;
        write(STDOUT_FILENO, row->chars + byte_index, bytes);
        logical_width += w; current_disp += w; byte_index += bytes;
    }
    if (in_selection)
        write(STDOUT_FILENO, "\x1b[0m", 4);
}

/* --- Undo Functions --- */

void push_undo_state(void) {
    UndoState *state = malloc(sizeof(UndoState));
    if (!state) die("malloc undo state");
    state->cx = E.cx; state->cy = E.cy; state->numrows = E.numrows;
    state->row = malloc(sizeof(EditorLine) * E.numrows);
    if (!state->row) die("malloc undo rows");
    for (int i = 0; i < E.numrows; i++) {
        state->row[i].size = E.row[i].size;
        state->row[i].chars = malloc(E.row[i].size + 1);
        if (!state->row[i].chars) die("malloc undo row char");
        memcpy(state->row[i].chars, E.row[i].chars, E.row[i].size);
        state->row[i].chars[E.row[i].size] = '\0';
    }
    if (undo_history_len == 100) {
        free_undo_state(undo_history[0]);
        for (int i = 1; i < 100; i++)
            undo_history[i - 1] = undo_history[i];
        undo_history_len--;
    }
    undo_history[undo_history_len++] = state;
}

void free_undo_state(UndoState *state) {
    for (int i = 0; i < state->numrows; i++)
        free(state->row[i].chars);
    free(state->row);
    free(state);
}

void pop_undo_state(void) {
    if (undo_history_len == 0)
        return;
    UndoState *state = undo_history[undo_history_len - 1];
    undo_history_len--;
    for (int i = 0; i < E.numrows; i++)
        free(E.row[i].chars);
    free(E.row);
    E.numrows = state->numrows;
    E.row = malloc(sizeof(EditorLine) * E.numrows);
    if (!E.row) die("malloc restore rows");
    for (int i = 0; i < E.numrows; i++) {
        E.row[i].size = state->row[i].size;
        E.row[i].chars = malloc(E.row[i].size + 1);
        if (!E.row[i].chars) die("malloc restore row char");
        memcpy(E.row[i].chars, state->row[i].chars, E.row[i].size);
        E.row[i].chars[E.row[i].size] = '\0';
        E.row[i].modified = 0;
    }
    E.cx = state->cx; E.cy = state->cy;
    free_undo_state(state);
}

/* --- Terminal Setup Functions --- */

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");
    atexit(disableRawMode);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

/* --- Input Function --- */

int editorReadKey(void) {
    char c; int nread;
    while ((nread = read(STDIN_FILENO, &c, 1)) == 0)
        ;
    if (nread == -1 && errno != EAGAIN)
        die("read");
    if (c == '\x1b') {
        char seq[6];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                char seq3;
                if (read(STDIN_FILENO, &seq3, 1) != 1)
                    return '\x1b';
                if (seq3 == '~') {
                    switch(seq[1]) {
                        case '1': return HOME_KEY;   // ESC [ 1 ~ (Home)
                        case '3': return DEL_KEY;    // ESC [ 3 ~ (Delete)
                        case '4': return END_KEY;    // ESC [ 4 ~ (End)
                        case '5': return PGUP_KEY;   // ESC [ 5 ~ (Page Up)
                        case '6': return PGDN_KEY;   // ESC [ 6 ~ (Page Down)
                        default: return '\x1b';
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                    default: return '\x1b';
                }
            }
        }
        return '\x1b';
    }
    return c;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
        return -1;
    *cols = ws.ws_col; *rows = ws.ws_row;
    return 0;
}

/* --- Drawing Routines --- */

void editorDrawRows(int rn_width) {
    int text_width = E.screencols - rn_width - 1;
    char numbuf[16];
    for (int y = 0; y < E.textrows; y++) {
        int file_row = E.rowoff + y;
        if (file_row < E.numrows) {
            int rn = file_row + 1;
            int num_len = snprintf(numbuf, sizeof(numbuf), "%*d ", rn_width - 1, rn);
            write(STDOUT_FILENO, numbuf, num_len);
            editorRenderRowWithSelection(&E.row[file_row], file_row, text_width);
            int printed_width = editorDisplayWidth(E.row[file_row].chars) - E.coloff;
            if (printed_width < 0) printed_width = 0;
            if (printed_width > text_width) printed_width = text_width;
            for (int i = printed_width; i < text_width; i++)
                write(STDOUT_FILENO, " ", 1);
            if (E.row[file_row].modified)
                write(STDOUT_FILENO, "\x1b[41m \x1b[0m", 10);
            else
                write(STDOUT_FILENO, " ", 1);
        } else {
            for (int i = 0; i < rn_width; i++)
                write(STDOUT_FILENO, " ", 1);
            write(STDOUT_FILENO, "~", 1);
        }
        write(STDOUT_FILENO, "\x1b[K", 3);
        if (y < E.textrows - 1)
            write(STDOUT_FILENO, "\r\n", 2);
    }
}

void editorDrawStatusBar(void) {
    char status[80];
    char rstatus[32];
    int len = snprintf(status, sizeof(status), "%.20s%s",
                       E.filename ? E.filename : "[No Name]",
                       E.dirty ? " (modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "Ln %d, Col %d",
                        E.cy + 1, E.cx + 1);
    if (len > E.screencols)
        len = E.screencols;
    write(STDOUT_FILENO, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen)
            break;
        write(STDOUT_FILENO, " ", 1);
        len++;
    }
    write(STDOUT_FILENO, rstatus, rlen);
}

void editorDrawShortcutBar(void) {
    write(STDOUT_FILENO, "\x1b[2m", 4);
    char menu[256];
    snprintf(menu, sizeof(menu),
             "Ctrl+Q Quit | Ctrl+S Save | Ctrl+Z Undo | Ctrl+X Cut | Ctrl+C Copy | Ctrl+V Paste | Ctrl+T Select | Ctrl+A Select All | Ctrl+F Search");
    int len = strlen(menu);
    if (len > E.screencols) len = E.screencols;
    write(STDOUT_FILENO, menu, len);
    for (int i = len; i < E.screencols; i++)
        write(STDOUT_FILENO, " ", 1);
    write(STDOUT_FILENO, "\x1b[0m", 4);
}

void editorRefreshScreen(void) {
    int rn_width = getRowNumWidth();
    E.textrows = E.screenrows - 2;
    write(STDOUT_FILENO, "\x1b[?25l", 6);
    write(STDOUT_FILENO, "\x1b[H", 3);
    editorDrawRows(rn_width);
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;1H", E.textrows + 1);
    write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\x1b[2m", 4);
    editorDrawStatusBar();
    write(STDOUT_FILENO, "\x1b[0m", 4);
    snprintf(buf, sizeof(buf), "\x1b[%d;1H", E.screenrows);
    write(STDOUT_FILENO, buf, strlen(buf));
    editorDrawShortcutBar();
    int cursor_y = (E.cy - E.rowoff) + 1;
    int cursor_x = rn_width + (E.cx - E.coloff) + 1;
    if (cursor_y < 1) cursor_y = 1;
    if (cursor_x < 1) cursor_x = 1;
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cursor_y, cursor_x);
    write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\x1b[?25h", 6);
}

/* --- Key Processing --- */

void editorProcessKeypress(void) {
    int c = editorReadKey();
    static int last_key_was_vertical = 0;

    if (c == CTRL_KEY('t')) {
        if (E.selecting) {
            E.selecting = 0;
            snprintf(E.status_message, sizeof(E.status_message), "Selection canceled");
        } else {
            E.selecting = 1;
            E.sel_anchor_x = E.cx;
            E.sel_anchor_y = E.cy;
            snprintf(E.status_message, sizeof(E.status_message), "Selection started");
        }
        last_key_was_vertical = 0;
        return;
    }
    if (c == CTRL_KEY('a')) {
        if (E.numrows > 0) {
            E.selecting = 1;
            E.sel_anchor_x = 0;
            E.sel_anchor_y = 0;
            E.cy = E.numrows - 1;
            E.cx = editorDisplayWidth(E.row[E.cy].chars);
            snprintf(E.status_message, sizeof(E.status_message), "Selected all text");
        }
        last_key_was_vertical = 0;
        return;
    }
    if ((c == CTRL_KEY('h') || c == BACKSPACE) && E.selecting) {
        push_undo_state();
        editorDeleteSelection();
        last_key_was_vertical = 0;
        return;
    }
    if (c == DEL_KEY && E.selecting) {
        push_undo_state();
        editorDeleteSelection();
        last_key_was_vertical = 0;
        return;
    }
    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case CTRL_KEY('s'):
            editorSave();
            break;
        case CTRL_KEY('z'):
            pop_undo_state();
            break;
        case CTRL_KEY('x'):
            push_undo_state();
            editorCutSelection();
            break;
        case CTRL_KEY('c'):
            editorCopySelection();
            break;
        case CTRL_KEY('v'):
            push_undo_state();
            editorPasteClipboard();
            break;
        case DEL_KEY:
            push_undo_state();
            editorDelCharAtCursor();
            break;
        case HOME_KEY:
            E.cx = 0;
            E.preferred_cx = E.cx;
            last_key_was_vertical = 0;
            break;
        case END_KEY:
            E.cx = editorDisplayWidth(E.row[E.cy].chars);
            E.preferred_cx = E.cx;
            last_key_was_vertical = 0;
            break;
        case PGUP_KEY:
            E.cy -= E.textrows;
            if (E.cy < 0)
                E.cy = 0;
            last_key_was_vertical = 0;
            break;
        case PGDN_KEY:
            E.cy += E.textrows;
            if (E.cy >= E.numrows)
                E.cy = E.numrows - 1;
            last_key_was_vertical = 0;
            break;
        case CTRL_KEY('f'):
            push_undo_state();
            editorSearch();
            last_key_was_vertical = 0;
            break;
        case '\r':
            push_undo_state();
            editorInsertNewline();
            last_key_was_vertical = 0;
            break;
        case CTRL_KEY('h'):
        case BACKSPACE:
            push_undo_state();
            editorDelChar();
            last_key_was_vertical = 0;
            break;
        case ARROW_UP:
            if (!last_key_was_vertical)
                E.preferred_cx = E.cx;
            last_key_was_vertical = 1;
            if (E.cy > 0) {
                E.cy--;
                int row_width = editorDisplayWidth(E.row[E.cy].chars);
                if (E.preferred_cx > row_width)
                    E.cx = row_width;
                else
                    E.cx = E.preferred_cx;
            }
            break;
        case ARROW_DOWN:
            if (!last_key_was_vertical)
                E.preferred_cx = E.cx;
            last_key_was_vertical = 1;
            if (E.cy < E.numrows - 1) {
                E.cy++;
                int row_width = editorDisplayWidth(E.row[E.cy].chars);
                if (E.preferred_cx > row_width)
                    E.cx = row_width;
                else
                    E.cx = E.preferred_cx;
            }
            break;
        case ARROW_LEFT:
            if (E.cx > 0)
                E.cx--;
            else if (E.cy > 0) {
                E.cy--;
                E.cx = editorDisplayWidth(E.row[E.cy].chars);
            }
            E.preferred_cx = E.cx;
            last_key_was_vertical = 0;
            break;
        case ARROW_RIGHT: {
            int roww = editorDisplayWidth(E.row[E.cy].chars);
            if (E.cx < roww)
                E.cx++;
            else if (E.cy < E.numrows - 1) {
                E.cy++;
                E.cx = 0;
            }
            E.preferred_cx = E.cx;
            last_key_was_vertical = 0;
            break;
        }
        default:
            if (!iscntrl(c)) {
                push_undo_state();
                if ((unsigned char)c < 0x80) {
                    editorInsertChar(c);
                } else {
                    int utf8_len = 0;
                    unsigned char uc = (unsigned char)c;
                    if ((uc & 0xE0) == 0xC0)
                        utf8_len = 2;
                    else if ((uc & 0xF0) == 0xE0)
                        utf8_len = 3;
                    else if ((uc & 0xF8) == 0xF0)
                        utf8_len = 4;
                    else
                        utf8_len = 1;
                    char utf8_buf[5];
                    utf8_buf[0] = c;
                    for (int i = 1; i < utf8_len; i++)
                        utf8_buf[i] = editorReadKey();
                    utf8_buf[utf8_len] = '\0';
                    editorInsertUTF8(utf8_buf, utf8_len);
                }
            }
            last_key_was_vertical = 0;
            break;
    }
    int rn_width = getRowNumWidth();
    if (E.cx < E.coloff)
        E.coloff = E.cx;
    if (E.cx >= E.coloff + (E.screencols - rn_width - 1))
        E.coloff = E.cx - (E.screencols - rn_width - 1) + 1;
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.textrows)
        E.rowoff = E.cy - E.textrows + 1;
}

/* --- New Functions for Selection Deletion and Delete Key --- */

void editorDeleteSelection(void) {
    if (!E.selecting)
        return;
    int start_line = (E.sel_anchor_y < E.cy ? E.sel_anchor_y : E.cy);
    int end_line = (E.sel_anchor_y > E.cy ? E.sel_anchor_y : E.cy);
    int anchor_x = (E.sel_anchor_y <= E.cy ? E.sel_anchor_x : E.cx);
    int current_x = (E.sel_anchor_y <= E.cy ? E.cx : E.sel_anchor_x);
    for (int i = start_line; i <= end_line; i++) {
        int line_width = editorDisplayWidth(E.row[i].chars);
        int sel_start, sel_end;
        if (start_line == end_line) {
            sel_start = (anchor_x < current_x ? anchor_x : current_x);
            sel_end   = (anchor_x < current_x ? current_x : anchor_x);
        } else if (i == start_line) {
            sel_start = (E.sel_anchor_y < E.cy ? E.sel_anchor_x : 0);
            sel_end = line_width;
        } else if (i == end_line) {
            sel_start = 0;
            sel_end = (E.sel_anchor_y < E.cy ? E.cx : E.sel_anchor_x);
        } else {
            sel_start = 0;
            sel_end = line_width;
        }
        int start_byte = editorRowCxToByteIndex(&E.row[i], sel_start);
        int end_byte = editorRowCxToByteIndex(&E.row[i], sel_end);
        if (i == start_line && i == end_line) {
            int new_size = E.row[i].size - (end_byte - start_byte);
            memmove(E.row[i].chars + start_byte,
                    E.row[i].chars + end_byte,
                    E.row[i].size - end_byte + 1);
            E.row[i].size = new_size;
        } else if (i == start_line) {
            E.row[i].chars[start_byte] = '\0';
            E.row[i].size = start_byte;
        } else if (i == end_line) {
            char *new_last = strdup(E.row[i].chars + end_byte);
            free(E.row[i].chars);
            E.row[i].chars = new_last;
            E.row[i].size = strlen(new_last);
        } else {
            E.row[i].size = 0;
            free(E.row[i].chars);
            E.row[i].chars = strdup("");
        }
    }
    if (start_line != end_line) {
        EditorLine *first_line = &E.row[start_line];
        EditorLine *last_line = &E.row[end_line];
        int new_size = first_line->size + last_line->size;
        first_line->chars = realloc(first_line->chars, new_size + 1);
        memcpy(first_line->chars + first_line->size, last_line->chars, last_line->size + 1);
        first_line->size = new_size;
        for (int i = end_line; i > start_line; i--) {
            free(E.row[i].chars);
            for (int j = i; j < E.numrows - 1; j++)
                E.row[j] = E.row[j + 1];
            E.numrows--;
        }
    }
    E.cx = (E.sel_anchor_y < E.cy ? E.sel_anchor_x : E.cx);
    E.cy = start_line;
    E.selecting = 0;
    E.dirty = 1;
    snprintf(E.status_message, sizeof(E.status_message), "Deleted selection");
}

void editorDelCharAtCursor(void) {
    if (E.cy == E.numrows)
        return;
    EditorLine *line = &E.row[E.cy];
    int row_display_width = editorDisplayWidth(line->chars);
    if (E.cx < row_display_width) {
       int index = editorRowCxToByteIndex(line, E.cx);
       int next_index = editorRowCxToByteIndex(line, E.cx + 1);
       memmove(&line->chars[index], &line->chars[next_index], line->size - next_index + 1);
       line->size -= (next_index - index);
       line->modified = 1;
       E.dirty = 1;
    } else if (E.cx == row_display_width && E.cy < E.numrows - 1) {
       EditorLine *next_line = &E.row[E.cy+1];
       line->chars = realloc(line->chars, line->size + next_line->size + 1);
       if (!line->chars) die("realloc");
       memcpy(line->chars + line->size, next_line->chars, next_line->size);
       line->size += next_line->size;
       line->chars[line->size] = '\0';
       line->modified = 1;
       free(next_line->chars);
       for (int j = E.cy+1; j < E.numrows - 1; j++)
           E.row[j] = E.row[j+1];
       E.numrows--;
       E.dirty = 1;
    }
}

/* --- New Function for Search --- */

void editorSearch(void) {
    char query[256];
    /* Switch to the alternate screen buffer so the search UI doesn't overlay the editor */
    printf("\033[?1049h");
    fflush(stdout);

    /* Temporarily disable raw mode to get query input */
    disableRawMode();
    printf("\rSearch: ");
    fflush(stdout);
    if (fgets(query, sizeof(query), stdin) == NULL) {
        enableRawMode();
        /* Restore main screen */
        printf("\033[?1049l");
        fflush(stdout);
        return;
    }
    query[strcspn(query, "\n")] = '\0';
    enableRawMode();

    /* Get terminal size */
    int rows, cols;
    if (getWindowSize(&rows, &cols) == -1) {
        rows = 24; cols = 80;
    }

    /* Build list of matching line indices */
    int *matches = malloc(E.numrows * sizeof(int));
    if (!matches) {
        snprintf(E.status_message, sizeof(E.status_message), "Search: malloc failed");
        printf("\033[?1049l"); // restore main screen
        fflush(stdout);
        return;
    }
    int match_count = 0;
    for (int i = 0; i < E.numrows; i++) {
        if (strstr(E.row[i].chars, query) != NULL)
            matches[match_count++] = i;
    }
    if (match_count == 0) {
        free(matches);
        snprintf(E.status_message, sizeof(E.status_message), "No matches found");
        printf("\033[?1049l"); // restore main screen
        fflush(stdout);
        return;
    }

    /* Improved full-screen menu UI */
    int active = 0;
    int menu_start = 0;
    int menu_height = rows - 4;  /* Reserve some lines for header and footer */

    while (1) {
        /* Clear the alternate screen */
        printf("\033[2J\033[H");
        /* Header */
        printf("Search results for: \"%s\"\n", query);
        printf("--------------------------------------------------\n");

        int end = menu_start + menu_height;
        if (end > match_count)
            end = match_count;
        for (int i = menu_start; i < end; i++) {
            if (i == active)
                printf("\033[7m");  /* Inverse video for active selection */
            /* Ensure each line starts at column 1 */
            printf("\rLine %d: %s", matches[i] + 1, E.row[matches[i]].chars);
            printf("\033[0m\n");   /* Reset formatting and newline */
        }
        /* Footer prompt */
        printf("--------------------------------------------------\n");
        printf("Use Up/Down arrows to select, Enter to jump, 'q' to cancel.\n");
        fflush(stdout);

        int c = editorReadKey();
        if (c == 'q') {
            active = -1;
            break;
        } else if (c == '\r') {
            break;
        } else if (c == ARROW_UP) {
            if (active > 0) {
                active--;
                if (active < menu_start)
                    menu_start = active;
            }
        } else if (c == ARROW_DOWN) {
            if (active < match_count - 1) {
                active++;
                if (active >= menu_start + menu_height)
                    menu_start = active - menu_height + 1;
            }
        }
    }
    int result = -1;
    if (active != -1)
        result = matches[active];
    free(matches);

    /* Restore the main screen buffer so the editor content is visible again */
    printf("\033[?1049l");
    fflush(stdout);

    if (result != -1) {
        E.cy = result;
        char *pos = strstr(E.row[result].chars, query);
        if (pos != NULL) {
            int col = 0;
            for (char *p = E.row[result].chars; p < pos; p++)
                col++;
            E.cx = col;
        } else {
            E.cx = 0;
        }
        snprintf(E.status_message, sizeof(E.status_message),
                 "Jumped to match on line %d", result + 1);
    } else {
        snprintf(E.status_message, sizeof(E.status_message), "Search canceled");
    }
}

/* --- Clipboard and Selection Functions --- */

void editorCopySelection(void) {
    if (!E.selecting)
        return;
    int start_line = (E.sel_anchor_y < E.cy ? E.sel_anchor_y : E.cy);
    int end_line = (E.sel_anchor_y > E.cy ? E.sel_anchor_y : E.cy);
    int anchor_x = (E.sel_anchor_y <= E.cy ? E.sel_anchor_x : E.cx);
    int current_x = (E.sel_anchor_y <= E.cy ? E.cx : E.sel_anchor_x);
    size_t bufsize = 1024;
    char *buf = malloc(bufsize);
    if (!buf) die("malloc clipboard");
    buf[0] = '\0';
    size_t len = 0;
    for (int i = start_line; i <= end_line; i++) {
        int line_width = editorDisplayWidth(E.row[i].chars);
        int sel_start, sel_end;
        if (start_line == end_line) {
            sel_start = (anchor_x < current_x ? anchor_x : current_x);
            sel_end   = (anchor_x < current_x ? current_x : anchor_x);
        } else if (i == start_line) {
            sel_start = (E.sel_anchor_y < E.cy ? E.sel_anchor_x : 0);
            sel_end = line_width;
        } else if (i == end_line) {
            sel_start = 0;
            sel_end = (E.sel_anchor_y < E.cy ? E.cx : E.sel_anchor_x);
        } else {
            sel_start = 0;
            sel_end = line_width;
        }
        int start_byte = editorRowCxToByteIndex(&E.row[i], sel_start);
        int end_byte = editorRowCxToByteIndex(&E.row[i], sel_end);
        int chunk_len = end_byte - start_byte;
        if (len + chunk_len + 2 > bufsize) {
            bufsize *= 2;
            buf = realloc(buf, bufsize);
            if (!buf) die("realloc clipboard");
        }
        memcpy(buf + len, E.row[i].chars + start_byte, chunk_len);
        len += chunk_len;
        if (i != end_line)
            buf[len++] = '\n';
    }
    buf[len] = '\0';
    free(clipboard);
    clipboard = buf;
    clipboard_len = len;
    snprintf(E.status_message, sizeof(E.status_message),
             "Copied selection (%zu bytes)", clipboard_len);
}

void editorCutSelection(void) {
    if (!E.selecting)
        return;
    editorCopySelection();
    int start_line = (E.sel_anchor_y < E.cy ? E.sel_anchor_y : E.cy);
    int end_line = (E.sel_anchor_y > E.cy ? E.sel_anchor_y : E.cy);
    int anchor_x = (E.sel_anchor_y <= E.cy ? E.sel_anchor_x : E.cx);
    int current_x = (E.sel_anchor_y <= E.cy ? E.cx : E.sel_anchor_x);
    if (start_line == end_line) {
        int sel_start = (anchor_x < current_x ? anchor_x : current_x);
        int sel_end   = (anchor_x < current_x ? current_x : anchor_x);
        int start_byte = editorRowCxToByteIndex(&E.row[start_line], sel_start);
        int end_byte = editorRowCxToByteIndex(&E.row[start_line], sel_end);
        int new_size = E.row[start_line].size - (end_byte - start_byte);
        memmove(E.row[start_line].chars + start_byte,
                E.row[start_line].chars + end_byte,
                E.row[start_line].size - end_byte + 1);
        E.row[start_line].size = new_size;
    } else {
        int first_sel_start = (E.sel_anchor_y < E.cy ? E.sel_anchor_x : 0);
        int last_sel_end = (E.sel_anchor_y < E.cy ? E.cx : E.sel_anchor_x);
        int first_byte = editorRowCxToByteIndex(&E.row[start_line], first_sel_start);
        E.row[start_line].chars[first_byte] = '\0';
        E.row[start_line].size = first_byte;
        int last_byte = editorRowCxToByteIndex(&E.row[end_line], last_sel_end);
        char *new_last = strdup(E.row[end_line].chars + last_byte);
        free(E.row[end_line].chars);
        E.row[end_line].chars = new_last;
        E.row[end_line].size = strlen(new_last);
        int new_size = E.row[start_line].size + E.row[end_line].size;
        E.row[start_line].chars = realloc(E.row[start_line].chars, new_size + 1);
        memcpy(E.row[start_line].chars + E.row[start_line].size,
               E.row[end_line].chars,
               E.row[end_line].size + 1);
        E.row[start_line].size = new_size;
        for (int i = end_line; i > start_line; i--) {
            free(E.row[i].chars);
            for (int j = i; j < E.numrows - 1; j++)
                E.row[j] = E.row[j + 1];
            E.numrows--;
        }
    }
    E.cx = (E.sel_anchor_y < E.cy ? E.sel_anchor_x : E.cx);
    E.cy = start_line;
    E.selecting = 0;
    E.dirty = 1;
    snprintf(E.status_message, sizeof(E.status_message), "Cut selection");
}

void editorPasteClipboard(void) {
    if (!clipboard)
        return;
    push_undo_state();
    editorInsertString(clipboard);
    snprintf(E.status_message, sizeof(E.status_message),
             "Pasted clipboard (%zu bytes)", clipboard_len);
}

void editorInsertString(const char *s) {
    while (*s) {
        if (*s == '\n')
            editorInsertNewline();
        else
            editorInsertChar(*s);
        s++;
    }
}

/* --- File/Buffer Operations --- */

void editorOpen(const char *filename) {
    free(E.filename);
    E.filename = strdup(filename);
    if (E.filename == NULL)
        die("strdup");
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        if (errno == ENOENT) {
            editorAppendLine("", 0);
            E.dirty = 0;
            return;
        } else {
            die("fopen");
        }
    }
    char *line = NULL; size_t linecap = 0; ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        if (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            line[linelen - 1] = '\0';
            linelen--;
        }
        editorAppendLine(line, linelen);
    }
    free(line); fclose(fp);
    E.dirty = 0;
    if (E.numrows == 0)
        editorAppendLine("", 0);
}

void editorSave(void) {
    if (E.filename == NULL)
        return;
    int total_len = 0;
    for (int j = 0; j < E.numrows; j++)
        total_len += E.row[j].size + 1;
    char *buf = malloc(total_len);
    if (buf == NULL)
        die("malloc");
    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    int fd = open(E.filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd != -1) {
        if (write(fd, buf, total_len) == total_len) {
            close(fd); free(buf);
            E.dirty = 0;
            for (int i = 0; i < E.numrows; i++)
                E.row[i].modified = 0;
            return;
        }
        close(fd);
    }
    free(buf);
    die("write");
}

void editorAppendLine(char *s, size_t len) {
    EditorLine *new_row = realloc(E.row, sizeof(EditorLine) * (E.numrows + 1));
    if (new_row == NULL)
        die("realloc");
    E.row = new_row;
    E.row[E.numrows].chars = malloc(len + 1);
    if (E.row[E.numrows].chars == NULL)
        die("malloc");
    memcpy(E.row[E.numrows].chars, s, len);
    E.row[E.numrows].chars[len] = '\0';
    E.row[E.numrows].size = (int)len;
    E.row[E.numrows].modified = 0;
    E.numrows++;
}

void editorInsertChar(int c) {
    if (E.cy == E.numrows)
        editorAppendLine("", 0);
    EditorLine *line = &E.row[E.cy];
    if (E.cx > editorDisplayWidth(line->chars))
        E.cx = editorDisplayWidth(line->chars);
    int index = editorRowCxToByteIndex(line, E.cx);
    char *new_chars = realloc(line->chars, line->size + 2);
    if (new_chars == NULL)
        die("realloc");
    line->chars = new_chars;
    memmove(&line->chars[index + 1], &line->chars[index], line->size - index + 1);
    line->chars[index] = c;
    line->size++;
    E.cx++;
    E.preferred_cx = E.cx;
    line->modified = 1; E.dirty = 1;
}

void editorInsertUTF8(const char *s, int len) {
    if (E.cy == E.numrows)
        editorAppendLine("", 0);
    EditorLine *line = &E.row[E.cy];
    if (E.cx > editorDisplayWidth(line->chars))
        E.cx = editorDisplayWidth(line->chars);
    int index = editorRowCxToByteIndex(line, E.cx);
    char *new_chars = realloc(line->chars, line->size + len + 1);
    if (new_chars == NULL)
        die("realloc");
    line->chars = new_chars;
    memmove(&line->chars[index + len], &line->chars[index], line->size - index + 1);
    memcpy(&line->chars[index], s, len);
    line->size += len;
    wchar_t wc;
    size_t bytes = mbrtowc(&wc, s, len, NULL);
    int width = (bytes == (size_t)-1 || bytes == (size_t)-2) ? 1 : wcwidth(wc);
    if (width < 0) width = 1;
    E.cx += width;
    E.preferred_cx = E.cx;
    line->modified = 1; E.dirty = 1;
}

void editorInsertNewline(void) {
    if (E.cx == 0) {
        editorAppendLine("", 0);
        for (int i = E.numrows - 1; i > E.cy; i--)
            E.row[i] = E.row[i - 1];
        E.row[E.cy].chars = malloc(1);
        E.row[E.cy].chars[0] = '\0';
        E.row[E.cy].size = 0; E.row[E.cy].modified = 0;
        E.cy++;
    } else {
        EditorLine *line = &E.row[E.cy];
        int index = editorRowCxToByteIndex(line, E.cx);
        char *new_chars = malloc(line->size - index + 1);
        if (new_chars == NULL)
            die("malloc");
        memcpy(new_chars, &line->chars[index], line->size - index);
        new_chars[line->size - index] = '\0';
        int new_len = line->size - index;
        line->size = index; line->chars[index] = '\0';
        EditorLine *new_row = realloc(E.row, sizeof(EditorLine) * (E.numrows + 1));
        if (new_row == NULL)
            die("realloc");
        E.row = new_row;
        for (int j = E.numrows; j > E.cy; j--)
            E.row[j] = E.row[j - 1];
        E.numrows++;
        E.row[E.cy + 1].chars = new_chars;
        E.row[E.cy + 1].size = new_len;
        E.row[E.cy + 1].modified = 1;
        E.cy++; E.cx = 0;
        E.preferred_cx = E.cx;
    }
    E.dirty = 1;
}

void editorDelChar(void) {
    if (E.cy == E.numrows)
        return;
    if (E.cx == 0 && E.cy == 0)
        return;
    EditorLine *line = &E.row[E.cy];
    if (E.cx == 0) {
        EditorLine *prev_line = &E.row[E.cy - 1];
        int prev_size = prev_line->size;
        prev_line->chars = realloc(prev_line->chars, prev_size + line->size + 1);
        if (prev_line->chars == NULL)
            die("realloc");
        memcpy(&prev_line->chars[prev_size], line->chars, line->size);
        prev_line->chars[prev_size + line->size] = '\0';
        prev_line->size = prev_size + line->size; prev_line->modified = 1;
        free(line->chars);
        for (int j = E.cy; j < E.numrows - 1; j++)
            E.row[j] = E.row[j + 1];
        E.numrows--; E.cy--;
        E.cx = editorDisplayWidth(prev_line->chars);
        E.preferred_cx = E.cx;
    } else {
        int index = editorRowCxToByteIndex(line, E.cx);
        int prev_index = editorRowCxToByteIndex(line, E.cx - 1);
        memmove(&line->chars[prev_index], &line->chars[index], line->size - index + 1);
        line->size -= (index - prev_index);
        E.cx -= 1;
        E.preferred_cx = E.cx;
        line->modified = 1;
    }
    E.dirty = 1;
}

/* --- Main --- */

int main(int argc, char *argv[]) {
    setlocale(LC_CTYPE, "");
    E.cx = 0; E.cy = 0;
    E.rowoff = 0; E.coloff = 0;
    E.numrows = 0; E.row = NULL;
    E.filename = NULL; E.dirty = 0;
    E.status_message[0] = '\0';
    E.selecting = 0; E.sel_anchor_x = 0; E.sel_anchor_y = 0;
    E.preferred_cx = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    E.textrows = E.screenrows - 2;

    if (argc >= 2)
        editorOpen(argv[1]);
    else {
        editorAppendLine("", 0);
        E.dirty = 0;
    }

    enableRawMode();
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
