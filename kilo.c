/* Kilo -- A very simple editor. Does not depend on libcurses, directly emits VT100
 *         escapes on the terminal.
 *
 * -----------------------------------------------------------------------
 *
 * Copyright (C) 2016 Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (C) 2017 Urja Rannikko <urjaman@gmail.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define KILO_VERSION "0.B.1"

#define _BSD_SOURCE
#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

/* Syntax highlight types */
#define HL_NORMAL 0
#define HL_NONPRINT 1
#define HL_COMMENT 2   /* Single line comment. */
#define HL_MLCOMMENT 3 /* Multi-line comment. */
#define HL_KEYWORD1 4
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8      /* Search match. */

#define HL_HIGHLIGHT_STRINGS (1<<0)
#define HL_HIGHLIGHT_NUMBERS (1<<1)

struct editorSyntax {
    const char * const *filematch;
    const char * const *keywords;
    const char singleline_comment_start[2];
    const char multiline_comment_start[3];
    const char multiline_comment_end[3];
    const int flags;
};

/* This structure represents a single line of the file we are editing. */
typedef struct erow {
    int size;           /* Size of the row, excluding the null term. */
    int rsize;          /* Size of the rendered row. */
    char *chars;        /* Row content. */
    char *render;       /* Row content "rendered" for screen (for TABs). */
    unsigned char *hl;  /* Syntax highlight type for each character in render.*/
    int hl_oc;          /* Row had open comment at end in last syntax highlight
                           check. */
} erow;

struct editorConfig {
    int cx,cy;  /* Cursor x and y position in characters */
    int rowoff;     /* Offset of row displayed. */
    int coloff;     /* Offset of column displayed. */
    int screenrows; /* Number of rows that we can show */
    int screencols; /* Number of cols that we can show */
    char *screendirty; /* A flag whether each row of the screen needs redraw. */
    int numrows;    /* Number of rows */
    erow *row;      /* Rows */
    int dirty;      /* File modified but not saved. */
    int scr_is_dirty; /* Screen contents (anything but cursor position) has changed. */
    char *filename; /* Currently open filename */
    char statusmsg[80];
    time_t statusmsg_time;
    const struct editorSyntax *syntax;    /* Current syntax highlight, or NULL. */
    char **cutbuf; /* The rows in cut buffer */
    int cutcnt; /* Number of rows in cut buffer */
    int cutrow; /* "Source" of the rows cut. */
};

static struct editorConfig E;

enum KEY_ACTION {
        CTRL_C = 3,         /* Ctrl-c */
        CTRL_D = 4,         /* Ctrl-d */
        CTRL_E = 5,         /* Ctrl-e  del line */
        CTRL_F = 6,         /* Ctrl-f */
        CTRL_H = 8,         /* Ctrl-h */
        TAB = 9,            /* Tab */
        CTRL_K = 11,
        CTRL_L = 12,        /* Ctrl+l */
        ENTER = 13,         /* Enter */
        CTRL_O = 15,
        CTRL_Q = 17,        /* Ctrl-q */
        CTRL_S = 19,        /* Ctrl-s */
        CTRL_U = 21,        /* Ctrl-u */
        CTRL_W = 23,
        CTRL_X = 24,
        ESC = 27,           /* Escape */
        BACKSPACE =  127,   /* Backspace */
        /* The following are just soft codes, not really reported by the
         * terminal directly. */
        ARROW_LEFT = 1000,
        ARROW_RIGHT,
        ARROW_UP,
        ARROW_DOWN,
        DEL_KEY,
        HOME_KEY,
        END_KEY,
        PAGE_UP,
        PAGE_DOWN
};

void editorSetStatusMessage(const char *fmt, ...);

/* =========================== Syntax highlights DB =========================
 *
 * In order to add a new syntax, define two arrays with a list of file name
 * matches and keywords. The file name matches are used in order to match
 * a given syntax with a given file name: if a match pattern starts with a
 * dot, it is matched as the last past of the filename, for example ".c".
 * Otherwise the pattern is just searched inside the filenme, like "Makefile").
 *
 * The list of keywords to highlight is just a list of words, however if they
 * a trailing '|' character is added at the end, they are highlighted in
 * a different color, so that you can have two different sets of keywords.
 *
 * Finally add a stanza in the HLDB global variable with two two arrays
 * of strings, and a set of flags in order to enable highlighting of
 * comments and numbers.
 *
 * The characters for single and multi line comments must be exactly two
 * and must be provided as well (see the C language example).
 *
 * There is no support to highlight patterns currently. */

/* C / C++ */
const char * const C_HL_extensions[] = {".c",".cpp",NULL};
const char * const C_HL_keywords[] = {
        /* A few C / C++ keywords */
        "switch","if","while","for","break","continue","return","else",
        /* C / C++ types */
        "struct|","union|","typedef|","static|","enum|","class|",
        "int|","long|","double|","float|","char|","unsigned|","signed|",
        "void|","const|",NULL
};

/* Here we define an array of syntax highlights by extensions, keywords,
 * comments delimiters and flags. */
const struct editorSyntax HLDB[] = {
    {
        /* C / C++ */
        C_HL_extensions,
        C_HL_keywords,
        "//","/*","*/",
        HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS
    }
};

#define HLDB_ENTRIES (sizeof(HLDB)/sizeof(HLDB[0]))

/* =======================  Lack of memory management ======================= */
void oomeExit(void) {
    /* TODO: Try to perform an emergency save of the file contents (to a .save) file. */
    /* This would also be done on SIGSEGV or similar (SIGHUP). */
    /* N.B. No memory allocations in the file save path would help this :P */
    printf("\r\nOut of memory.\r\n");
    exit(1);
}

/* ============================= Robust write() ============================= */

ssize_t writeHard(int fd, const void *buf, size_t count) {
    const uint8_t *d = buf;
    ssize_t r;
    size_t off=0;
    do {
        r = write(fd, d+off, count-off);
        if (r >= 0) {
            off += r;
        } else {
            if ((errno != EAGAIN)&&(errno != EINTR)&&(errno != EWOULDBLOCK)) {
                return r; /* Ignore at your peril, but atleast we tried. */
            }
        }
    } while(off<count);
    return count;
}

/* ======================= Low level terminal handling ====================== */

static struct termios orig_termios; /* In order to restore at exit.*/

void disableRawMode(int fd) {
    /* Don't even check the return value as it's too late. */
    tcsetattr(fd,TCSAFLUSH,&orig_termios);
}

/* Called at exit to avoid remaining in raw mode. */
void editorAtExit(void) {
    disableRawMode(STDIN_FILENO);
}

/* Raw mode: 1960 magic shit. */
int enableRawMode(int fd) {
    struct termios raw;

    if (!isatty(STDIN_FILENO)) goto fatal;
    atexit(editorAtExit);
    if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

    raw = orig_termios;  /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - choing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer. */
    raw.c_cc[VMIN] = 0; /* Return each byte, or zero for timeout. */
    raw.c_cc[VTIME] = 1; /* 100 ms timeout (unit is tens of second). */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

/* Read a key from the terminal put in raw mode, trying to handle
 * escape sequences. */
int editorReadKey(int fd) {
    int nread;
    char c, seq[3];
    while ((nread = read(fd,&c,1)) == 0);
    if (nread == -1) exit(1);

    while(1) {
        switch(c) {
        case ESC:    /* escape sequence */
            /* If this is just an ESC, we'll timeout here. */
            if (read(fd,seq,1) == 0) return ESC;
            if (read(fd,seq+1,1) == 0) return ESC;

            /* ESC [ sequences. */
            if (seq[0] == '[') {
                if (seq[1] >= '0' && seq[1] <= '9') {
                    /* Extended escape, read additional byte. */
                    if (read(fd,seq+2,1) == 0) return ESC;
                    if (seq[2] == '~') {
                        switch(seq[1]) {
                        case '3': return DEL_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        }
                    }
                } else {
                    switch(seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                    }
                }
            }

            /* ESC O sequences. */
            else if (seq[0] == 'O') {
                switch(seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                }
            }
            break;
        default:
            return c;
        }
    }
}

/* Try to get the number of columns in the current terminal.
 * Returns 0 on success, -1 on error. */
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    	return -1;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

void screenSetDirty(int st, int to_end) {
    /* Filter out trying to dirty areas outside screen. */
    if (st < 0) {
        if (!to_end) return;
        st = 0;
    }
    if (st >= E.screenrows) return;
    E.scr_is_dirty = 1;
    int e = to_end ? E.screenrows-1 : st;
    memset(E.screendirty+st, 1, (e-st)+1);
}

/* ====================== Syntax highlight color scheme  ==================== */

int is_separator(int c) {
    return c == '\0' || isspace(c) || strchr(",.()+-/*=~%[];",c) != NULL;
}

/* Return true if the specified row last char is part of a multi line comment
 * that starts at this row or at one before, and does not end at the end
 * of the row but spawns to the next row. */
int editorRowHasOpenComment(erow *row) {
    if (row->hl && row->rsize && row->hl[row->rsize-1] == HL_MLCOMMENT &&
        (row->rsize < 2 || (row->render[row->rsize-2] != '*' ||
                            row->render[row->rsize-1] != '/'))) return 1;
    return 0;
}

/* Set every byte of row->hl (that corresponds to every character in the line)
 * to the right syntax highlight type (HL_* defines). */
void editorUpdateSyntax(int filerow) {
    erow * row = E.row+filerow;
    row->hl = realloc(row->hl,row->rsize);
    memset(row->hl,HL_NORMAL,row->rsize);
    screenSetDirty(filerow-E.rowoff, 0);

    if (E.syntax == NULL) return; /* No syntax, everything is HL_NORMAL. */

    int i, prev_sep, in_string, in_comment;
    char *p;
    const char * const *keywords = E.syntax->keywords;
    const char *scs = E.syntax->singleline_comment_start;
    const char *mcs = E.syntax->multiline_comment_start;
    const char *mce = E.syntax->multiline_comment_end;

    /* Point to the first non-space char. */
    p = row->render;
    i = 0; /* Current char offset */
    while(*p && isspace(*p)) {
        p++;
        i++;
    }
    prev_sep = 1; /* Tell the parser if 'i' points to start of word. */
    in_string = 0; /* Are we inside "" or '' ? */
    in_comment = 0; /* Are we inside multi-line comment? */

    /* If the previous line has an open comment, this line starts
     * with an open comment state. */
    if (filerow > 0 && editorRowHasOpenComment(&E.row[filerow-1]))
        in_comment = 1;

    while(*p) {
        /* Handle // comments. */
        if (prev_sep && *p == scs[0] && *(p+1) == scs[1]) {
            /* From here to end is a comment */
            memset(row->hl+i,HL_COMMENT,row->rsize-i);
            return;
        }

        /* Handle multi line comments. */
        if (in_comment) {
            row->hl[i] = HL_MLCOMMENT;
            if (*p == mce[0] && *(p+1) == mce[1]) {
                row->hl[i+1] = HL_MLCOMMENT;
                p += 2; i += 2;
                in_comment = 0;
                prev_sep = 1;
                continue;
            } else {
                prev_sep = 0;
                p++; i++;
                continue;
            }
        } else if (*p == mcs[0] && *(p+1) == mcs[1]) {
            row->hl[i] = HL_MLCOMMENT;
            row->hl[i+1] = HL_MLCOMMENT;
            p += 2; i += 2;
            in_comment = 1;
            prev_sep = 0;
            continue;
        }

        /* Handle "" and '' */
        if (in_string) {
            row->hl[i] = HL_STRING;
            if (*p == '\\') {
                row->hl[i+1] = HL_STRING;
                p += 2; i += 2;
                prev_sep = 0;
                continue;
            }
            if (*p == in_string) in_string = 0;
            p++; i++;
            continue;
        } else {
            if (*p == '"' || *p == '\'') {
                in_string = *p;
                row->hl[i] = HL_STRING;
                p++; i++;
                prev_sep = 0;
                continue;
            }
        }

        /* Handle non printable chars. */
        if (!isprint(*p)) {
            row->hl[i] = HL_NONPRINT;
            p++; i++;
            prev_sep = 0;
            continue;
        }

        /* Handle numbers */
        if ((isdigit(*p) && (prev_sep || row->hl[i-1] == HL_NUMBER)) ||
            (*p == '.' && i >0 && row->hl[i-1] == HL_NUMBER)) {
            row->hl[i] = HL_NUMBER;
            p++; i++;
            prev_sep = 0;
            continue;
        }

        /* Handle keywords and lib calls */
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen-1] == '|';
                if (kw2) klen--;

                if (!memcmp(p,keywords[j],klen) &&
                    is_separator(*(p+klen)))
                {
                    /* Keyword */
                    memset(row->hl+i,kw2 ? HL_KEYWORD2 : HL_KEYWORD1,klen);
                    p += klen;
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue; /* We had a keyword match */
            }
        }

        /* Not special chars */
        prev_sep = is_separator(*p);
        p++; i++;
    }

    /* Propagate syntax change to the next row if the open comment
     * state changed. This may recursively affect all the following rows
     * in the file. Gosh, atleast make it tail-callable... */
    int prev_oc = row->hl_oc;
    row->hl_oc = editorRowHasOpenComment(row);
    if (row->hl_oc != prev_oc && filerow+1 < E.numrows)
        editorUpdateSyntax(filerow+1);
}

/* Maps syntax highlight token types to terminal colors. */
int editorSyntaxToColor(int hl) {
    switch(hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return 36;     /* cyan */
    case HL_KEYWORD1: return 33;    /* yellow */
    case HL_KEYWORD2: return 32;    /* green */
    case HL_STRING: return 35;      /* magenta */
    case HL_NUMBER: return 31;      /* red */
    case HL_MATCH: return 34;      /* blu */
    default: return 37;             /* white */
    }
}

/* Select the syntax highlight scheme depending on the filename,
 * setting it in the global state E.syntax. */
void editorSelectSyntaxHighlight(char *filename) {
    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        const struct editorSyntax *s = HLDB+j;
        unsigned int i = 0;
        while(s->filematch[i]) {
            char *p;
            int patlen = strlen(s->filematch[i]);
            if ((p = strstr(filename,s->filematch[i])) != NULL) {
                if (s->filematch[i][0] != '.' || p[patlen] == '\0') {
                    E.syntax = s;
                    return;
                }
            }
            i++;
        }
    }
}

/* ======================= Editor rows implementation ======================= */

/* Update the rendered version and the syntax highlight of a row. */
void editorUpdateRow(int filerow) {
    erow * row = E.row+filerow;
    int tabs = 0, j, idx;

   /* Create a version of the row we can directly print on the screen,
     * respecting tabs, substituting non printable characters with '?'. */
    free(row->render);
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == TAB) tabs++;

    row->render = malloc(row->size + tabs*8 + 1);
    idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == TAB) {
            row->render[idx++] = ' ';
            while((idx+1) % 8 != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->rsize = idx;
    row->render[idx] = '\0';

    /* Update the syntax highlighting attributes of the row. */
    editorUpdateSyntax(filerow);
}

/* Insert a row at the specified position, shifting the other rows on the bottom
 * if required. */
void editorInsertRow(int at, char *s, size_t len) {
    if (at > E.numrows) return;
    E.row = realloc(E.row,sizeof(erow)*(E.numrows+1));
    if (at != E.numrows) {
        memmove(E.row+at+1,E.row+at,sizeof(E.row[0])*(E.numrows-at));
    }
    screenSetDirty(at-E.rowoff, 1);
    E.row[at].size = len;
    E.row[at].chars = malloc(len+1);
    memcpy(E.row[at].chars,s,len+1);
    E.row[at].hl = NULL;
    E.row[at].hl_oc = 0;
    E.row[at].render = NULL;
    E.row[at].rsize = 0;
    editorUpdateRow(at);
    E.numrows++;
    E.dirty++;
}

/* Free row's heap allocated stuff. */
void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

/* Remove the row at the specified position, shifting the remainign on the
 * top. */
void editorDelRow(int at) {
    erow *row;

    if (at >= E.numrows) return;
    row = E.row+at;
    editorFreeRow(row);
    memmove(E.row+at,E.row+at+1,sizeof(E.row[0])*(E.numrows-at-1));
    E.numrows--;
    E.dirty++;
    screenSetDirty(at-E.rowoff, 1);
}

void editorCutRow(int at) {
    erow * row;

    if (at >= E.numrows) return;
    row = E.row+at;
    if (at!=E.cutrow) { /* new cut */
        if (E.cutbuf) {
            for (int i=0;i<E.cutcnt;i++) free(E.cutbuf[i]);
            free(E.cutbuf);
        }
        E.cutbuf = 0;
        E.cutcnt = 0;
        E.cutrow = at;
    }
    E.cutcnt++;
    E.cutbuf = realloc(E.cutbuf,sizeof(char*)*E.cutcnt);
    E.cutbuf[E.cutcnt-1] = strdup(row->chars);
    editorDelRow(at);
}

void editorUncutRows(int at) {
    if (!E.cutcnt) return;
    E.cutrow = -1; /* After uncutting, next cut is always new */
    for (int i=0;i<E.cutcnt;i++) editorInsertRow(at+i, E.cutbuf[i], strlen(E.cutbuf[i]));
    E.cy += E.cutcnt;
    int scrl = E.cy - (E.screenrows-1);
    if (scrl>0) {
        E.cy -= scrl;
        E.rowoff += scrl;
        screenSetDirty(0, 1);
    } else {
        screenSetDirty(E.cy-E.cutcnt, 1);
    }
}

/* Turn the editor rows into a single heap-allocated string.
 * Returns the pointer to the heap-allocated string and populate the
 * integer pointed by 'buflen' with the size of the string, escluding
 * the final nulterm. */
char *editorRowsToString(int *buflen) {
    char *buf = NULL, *p;
    int totlen = 0;
    int j;

    /* Compute count of bytes */
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size+1; /* +1 is for "\n" at end of every row */
    *buflen = totlen;
    totlen++; /* Also make space for nulterm */

    p = buf = malloc(totlen);
    for (j = 0; j < E.numrows; j++) {
        memcpy(p,E.row[j].chars,E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    *p = '\0';
    return buf;
}

/* Insert a character at the specified position in a row, moving the remaining
 * chars on the right if needed. */
void editorRowInsertChar(int filerow, int at, int c) {
    erow *row = E.row+filerow;
    if (at > row->size) {
        /* Pad the string with spaces if the insert location is outside the
         * current length by more than a single character. */
        int padlen = at-row->size;
        /* In the next line +2 means: new char and null term. */
        row->chars = realloc(row->chars,row->size+padlen+2);
        memset(row->chars+row->size,' ',padlen);
        row->chars[row->size+padlen+1] = '\0';
        row->size += padlen+1;
    } else {
        /* If we are in the middle of the string just make space for 1 new
         * char plus the (already existing) null term. */
        row->chars = realloc(row->chars,row->size+2);
        memmove(row->chars+at+1,row->chars+at,row->size-at+1);
        row->size++;
    }
    row->chars[at] = c;
    editorUpdateRow(filerow);
    E.dirty++;
}

/* Append the string 's' at the end of a row */
void editorRowAppendString(int filerow, char *s, size_t len) {
    erow *row = E.row+filerow;
    row->chars = realloc(row->chars,row->size+len+1);
    memcpy(row->chars+row->size,s,len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(filerow);
    E.dirty++;
}

/* Delete the character at offset 'at' from the specified row. */
void editorRowDelChar(int filerow, int at) {
    erow *row = E.row+filerow;
    if (row->size <= at) return;
    memmove(row->chars+at,row->chars+at+1,row->size-at);
    editorUpdateRow(filerow);
    row->size--;
    E.dirty++;
}

/* Insert the specified char at the current prompt position. */
void editorInsertChar(int c) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    /* If the row where the cursor is currently located does not exist in our
     * logical representaion of the file, add enough empty rows as needed. */
    if (!row) {
        while(E.numrows <= filerow)
            editorInsertRow(E.numrows,"",0);
    }
    row = &E.row[filerow];
    editorRowInsertChar(filerow,filecol,c);
    if (E.cx == E.screencols-1)
        E.coloff++;
    else
        E.cx++;
    E.dirty++;
}

/* Inserting a newline is slightly complex as we have to handle inserting a
 * newline in the middle of a line, splitting the line as needed. */
void editorInsertNewline(void) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    if (!row) {
        if (filerow == E.numrows) {
            editorInsertRow(filerow,"",0);
            goto fixcursor;
        }
        return;
    }
    /* If the cursor is over the current line size, we want to conceptually
     * think it's just over the last character. */
    if (filecol >= row->size) filecol = row->size;
    if (filecol == 0) {
        editorInsertRow(filerow,"",0);
    } else {
        /* We are in the middle of a line. Split it between two rows. */
        editorInsertRow(filerow+1,row->chars+filecol,row->size-filecol);
        row = &E.row[filerow];
        row->chars[filecol] = '\0';
        row->size = filecol;
        editorUpdateRow(filerow);
        screenSetDirty(filerow-E.rowoff, 1);
    }
fixcursor:
    if (E.coloff) screenSetDirty(E.cy, 0);
    if (E.cy == E.screenrows-1) {
        E.rowoff++;
        screenSetDirty(0,1);
    } else {
        E.cy++;
    }
    E.cx = 0;
    E.coloff = 0;
}

/* Delete the char at the current prompt position. */
void editorDelChar(void) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    if (!row || (filecol == 0 && filerow == 0)) return;
    if (filecol == 0) {
        /* Handle the case of column 0, we need to move the current line
         * on the right of the previous one. */
        filecol = E.row[filerow-1].size;
        editorRowAppendString(filerow-1,row->chars,row->size);
        editorDelRow(filerow);
        row = NULL;
        if (E.cy == 0)
            E.rowoff--;
        else
            E.cy--;
        E.cx = filecol;
        if (E.cx >= E.screencols) {
            int shift = (E.screencols-E.cx)+1;
            E.cx -= shift;
            E.coloff += shift;
        }
        screenSetDirty(E.cy, 1);
    } else {
        editorRowDelChar(filerow,filecol-1);
        if (E.cx == 0 && E.coloff)
            E.coloff--;
        else
            E.cx--;
    }
    if (row) editorUpdateRow(filerow);
    E.dirty++;
}

/* Load the specified program in the editor memory and returns 0 on success
 * or 1 on error. */
int editorOpen(char *filename) {
    FILE *fp;

    E.dirty = 0;
    free(E.filename);
    E.filename = strdup(filename);

    fp = fopen(filename,"r");
    if (!fp) {
        if (errno != ENOENT) {
            perror("Opening file");
            exit(1);
        }
        return 1;
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while((linelen = getline(&line,&linecap,fp)) != -1) {
        if (linelen && (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
            line[--linelen] = '\0';
        editorInsertRow(E.numrows,line,linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
    return 0;
}

/* Save the current file on disk. Return 0 on success, 1 on error. */
int editorSave(void) {
    int len;
    char *buf = editorRowsToString(&len);
    int fd = open(E.filename,O_RDWR|O_CREAT,0644);
    if (fd == -1) goto writeerr;

    /* Use truncate + a single write(2) call in order to make saving
     * a bit safer, under the limits of what we can do in a small editor. */
    if (ftruncate(fd,len) == -1) goto writeerr;
    if (writeHard(fd,buf,len) != len) goto writeerr;

    close(fd);
    free(buf);
    E.dirty = 0;
    editorSetStatusMessage("%d bytes written on disk", len);
    return 0;

writeerr:
    free(buf);
    if (fd != -1) close(fd);
    editorSetStatusMessage("Can't save! I/O error: %s",strerror(errno));
    return 1;
}

/* ============================= Terminal update ============================ */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    int len;
    int blen;
};

void abInit(struct abuf *ab) {
    ab->blen = (E.screenrows + 3) * E.screencols; // Approximation for maximum needed space.
    ab->b = malloc(ab->blen);
    ab->len = 0;
    if (!ab->b) { /* Once more with a silly small buffer ... */
        ab->b = malloc(256);
        ab->blen = 256;
        if (!ab->b) oomeExit();
    }
}

void abAppend(struct abuf *ab, const char *s, int len) {
    int nlen = ab->len + len;
    if (nlen > ab->blen) {
        /* Try to switch to a bigger buffer. */
        int nblen = nlen + 256;
        char *nb = malloc(nblen);
        if (!nb) { /* Okay, keep the current buffer, write in parts. */
            writeHard(STDOUT_FILENO,ab->b,ab->len);
            ab->len = 0;
            nlen = len;
        } else {
            memcpy(nb, ab->b, ab->len);
            free(ab->b);
            ab->b = nb;
        }
    }
    memcpy(ab->b+ab->len,s,len);
    ab->len = nlen;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/* This function writes the whole screen using VT100 escape characters
 * starting from the logical state of the editor in the global state 'E'. */
void editorRefreshScreen(void) {
    int y;
    erow *r;
    char buf[32];
    struct abuf ab; abInit(&ab);
    int msglen = strlen(E.statusmsg);
    int show_statusmsg = (time(NULL)-E.statusmsg_time < 5);

    if ((msglen)&&(!show_statusmsg)) E.scr_is_dirty = 1; /* Hide message. */

    if (E.scr_is_dirty) {
        abAppend(&ab,"\x1b[?25l\x1b[H",6+3); /* Hide cursor and Go home */
        for (y = 0; y < E.screenrows; y++) {
            int filerow = E.rowoff+y;

            if (filerow >= E.numrows) {
                if (E.numrows == 0 && y == E.screenrows/3) {
                    char welcome[80];
                    int welcomelen = snprintf(welcome,sizeof(welcome),
                        "Kilo editor -- version %s\x1b[0K\r\n", KILO_VERSION);
                    int padding = (E.screencols-welcomelen)/2;
                    if (padding) {
                        abAppend(&ab,"~",1);
                        padding--;
                    }
                    while(padding--) abAppend(&ab," ",1);
                    abAppend(&ab,welcome,welcomelen);
                } else {
                    abAppend(&ab,"~\x1b[0K\r\n",7);
                }
                continue;
            }
            /* For now we ignore the above part since those are short anyways. */
            if (!E.screendirty[y]) {
                abAppend(&ab,"\r\n",2);
                continue;
            }

            r = &E.row[filerow];

            int len = r->rsize - E.coloff;
            int current_color = -1;
            if (len > 0) {
                if (len > E.screencols) len = E.screencols;
                char *c = r->render+E.coloff;
                unsigned char *hl = r->hl+E.coloff;
                int j;
                for (j = 0; j < len; j++) {
                    if (!isprint(c[j])) { /* Nonprinting */
                        char sym;
                        abAppend(&ab,"\x1b[7m",4);
                        if (c[j] <= 26)
                            sym = '@'+c[j];
                        else
                            sym = '?';
                        abAppend(&ab,&sym,1);
                        abAppend(&ab,"\x1b[0m",4);
                    } else if (hl[j] == HL_NORMAL) {
                        if (current_color != -1) {
                            abAppend(&ab,"\x1b[39m",5);
                            current_color = -1;
                        }
                        abAppend(&ab,c+j,1);
                    } else {
                        int color = editorSyntaxToColor(hl[j]);
                        if (color != current_color) {
                            char buf[16];
                            int clen = snprintf(buf,sizeof(buf),"\x1b[%dm",color);
                            current_color = color;
                            abAppend(&ab,buf,clen);
                        }
                        abAppend(&ab,c+j,1);
                    }
                }
            }
            abAppend(&ab,"\x1b[39m\x1b[0K\r\n",5+4+2);
            E.screendirty[y] = 0;
        }

        /* Create a two rows status. First row: */
        abAppend(&ab,"\x1b[0K\x1b[7m",4+4);
        char status[80], rstatus[80];
        int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
            E.filename, E.numrows, E.dirty ? "(modified)" : "");
        int rlen = snprintf(rstatus, sizeof(rstatus),
            "%d/%d",E.rowoff,E.numrows);
        if (len > E.screencols) len = E.screencols;
        abAppend(&ab,status,len);
        while(len < E.screencols) {
            if (E.screencols - len == rlen) {
                abAppend(&ab,rstatus,rlen);
                break;
            } else {
                abAppend(&ab," ",1);
                len++;
            }
        }

        /* Second row depends on E.statusmsg and the status message update time. */
        abAppend(&ab,"\x1b[0m\r\n\x1b[0K",6+4);
        if (msglen) {
            if (show_statusmsg) {
                abAppend(&ab,E.statusmsg,msglen <= E.screencols ? msglen : E.screencols);
            } else {
                E.statusmsg[0] = 0;
            }
        }
    } /* endif scr_is_dirty */
    /* Put cursor at its current position. Note that the horizontal position
     * at which the cursor is displayed may be different compared to 'E.cx'
     * because of TABs. */
    int j;
    int cx = 1;
    int filerow = E.rowoff+E.cy;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    if (row) {
        for (j = E.coloff; j < (E.cx+E.coloff); j++) {
            if (j < row->size && row->chars[j] == TAB) cx += 7-((cx)%8);
            cx++;
        }
    }
    snprintf(buf,sizeof(buf),"\x1b[%d;%dH",E.cy+1,cx);
    abAppend(&ab,buf,strlen(buf));
    abAppend(&ab,"\x1b[?25h",6); /* Show cursor. */
    writeHard(STDOUT_FILENO,ab.b,ab.len);
    abFree(&ab);
    E.scr_is_dirty = 0;
}

/* Set an editor status message for the second line of the status, at the
 * end of the screen. */
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    vsnprintf(E.statusmsg,sizeof(E.statusmsg),fmt,ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
    E.scr_is_dirty = 1;
}

/* =============================== Find mode ================================ */

#define KILO_QUERY_LEN 256
void startOfLine(void) {
     int filerow = E.rowoff + E.cy;
     erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
     if (row)
         E.cx = 0;
 }

void endOfLine(void) {
  int filerow = E.rowoff + E.cy;
  erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
  if (row)
    E.cx = row->size;
}

void editorFind(int fd) {
    char query[KILO_QUERY_LEN+1] = {0};
    int qlen = 0;
    int last_match = -1; /* Last line where a match was found. -1 for none. */
    int find_next = 0; /* if 1 search next, if -1 search prev. */
    int saved_hl_line = -1;  /* No saved HL */
    char *saved_hl = NULL;

#define FIND_RESTORE_HL do { \
    if (saved_hl) { \
        memcpy(E.row[saved_hl_line].hl,saved_hl, E.row[saved_hl_line].rsize); \
        saved_hl = NULL; \
    } \
} while (0)

    /* Save the cursor position in order to restore it later. */
    int saved_cx = E.cx, saved_cy = E.cy;
    int saved_coloff = E.coloff, saved_rowoff = E.rowoff;

    while(1) {
        editorSetStatusMessage(
            "Search: %s (Use ESC/Arrows/Enter)", query);
        editorRefreshScreen();

        int c = editorReadKey(fd);
        if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
            if (qlen != 0) query[--qlen] = '\0';
            last_match = -1;
        } else if (c == ESC || c == ENTER) {
            screenSetDirty(0,1);
            if (c == ESC) {
                E.cx = saved_cx; E.cy = saved_cy;
                E.coloff = saved_coloff; E.rowoff = saved_rowoff;
            }
            FIND_RESTORE_HL;
            editorSetStatusMessage("");
            return;
        } else if (c == ARROW_RIGHT || c == ARROW_DOWN) {
            find_next = 1;
        } else if (c == ARROW_LEFT || c == ARROW_UP) {
            find_next = -1;
        } else if (isprint(c)) {
            if (qlen < KILO_QUERY_LEN) {
                query[qlen++] = c;
                query[qlen] = '\0';
                last_match = -1;
            }
        }

        /* Search occurrence. */
        if (last_match == -1) find_next = 1;
        if (find_next) {
            char *match = NULL;
            int match_offset = 0;
            int i, current = last_match;

            for (i = 0; i < E.numrows; i++) {
                current += find_next;
                if (current == -1) current = E.numrows-1;
                else if (current == E.numrows) current = 0;
                match = strstr(E.row[current].render,query);
                if (match) {
                    match_offset = match-E.row[current].render;
                    break;
                }
            }
            find_next = 0;

            /* Highlight */
            FIND_RESTORE_HL;

            if (match) {
                erow *row = &E.row[current];
                last_match = current;
                if (row->hl) {
                    saved_hl_line = current;
                    saved_hl = malloc(row->rsize);
                    memcpy(saved_hl,row->hl,row->rsize);
                    memset(row->hl+match_offset,HL_MATCH,qlen);
                }
                E.cy = 0;
                E.cx = match_offset;
                E.rowoff = current;
                E.coloff = 0;
                /* Scroll horizontally as needed. */
                if (E.cx > E.screencols) {
                    int diff = E.cx - E.screencols;
                    E.cx -= diff;
                    E.coloff += diff;
                }
                screenSetDirty(0,1);
            }
        }
    }
}

/* ========================= Editor events handling  ======================== */

/* Handle cursor position change because arrow keys were pressed. */
void editorMoveCursor(int key) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    int rowlen;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    switch(key) {
    case ARROW_LEFT:
        if (E.cx == 0) {
            if (E.coloff) {
                E.coloff--;
                screenSetDirty(E.cy,0);
            } else {
                if (filerow > 0) {
                    E.cy--;
                    E.cx = E.row[filerow-1].size;
                    if (E.cx > E.screencols-1) {
                        E.coloff = E.cx-E.screencols+1;
                        E.cx = E.screencols-1;
                        screenSetDirty(E.cy,0);
                    }
                }
            }
        } else {
            E.cx -= 1;
        }
        break;
    case ARROW_RIGHT:
        if (row && filecol < row->size) {
            if (E.cx == E.screencols-1) {
                E.coloff++;
                screenSetDirty(E.cy,0);
            } else {
                E.cx += 1;
            }
        } else if (row && filecol == row->size) {
            E.cx = 0;
            if (E.coloff) screenSetDirty(E.cy,0);
            E.coloff = 0;
            if (E.cy == E.screenrows-1) {
                E.rowoff++;
                screenSetDirty(0,1); /* TODO: Scroll you dunk. */
            } else {
                E.cy += 1;
            }
        }
        break;
    case ARROW_UP:
        if (E.cy == 0) {
            if (E.rowoff) {
                E.rowoff -= (E.screenrows/2);
                if (E.rowoff < 0) E.rowoff = 0;
                E.cy = (filerow - E.rowoff)-1;
                screenSetDirty(0,1);
            }
        } else {
            if (E.coloff) {
                screenSetDirty(E.cy, 0);
                screenSetDirty(E.cy-1, 0);
            }
            E.cy -= 1;
        }
        break;
    case ARROW_DOWN:
        if (filerow < E.numrows) {
            if (E.cy == E.screenrows-1) {
                E.rowoff += E.screenrows/2;
                if (E.rowoff >= E.numrows) E.rowoff = E.numrows-1;
                E.cy = (filerow+1) - E.rowoff;
                screenSetDirty(0,1); /* Scroll. */
            } else {
                if (E.coloff) {
                    screenSetDirty(E.cy, 0);
                    screenSetDirty(E.cy+1, 0);
                }
                E.cy += 1;
            }
        }
        break;
    case PAGE_DOWN:
        E.cx = 0;
        E.cy = 0;
        E.rowoff += (E.screenrows-2);
        if (E.rowoff >= E.numrows) E.rowoff = E.numrows-1;
        screenSetDirty(0,1);
        break;
    case PAGE_UP:
        E.cx = 0;
        E.cy = 0;
        E.rowoff -= (E.screenrows-2);
        if (E.rowoff < 0) E.rowoff = 0;
        screenSetDirty(0,1);
        break;
    }
    /* Fix cx if the current line has not enough chars. */
    filerow = E.rowoff+E.cy;
    filecol = E.coloff+E.cx;
    row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    rowlen = row ? row->size : 0;
    if (filecol > rowlen) {
        E.cx -= filecol-rowlen;
        if (E.cx < 0) {
            E.coloff += E.cx;
            E.cx = 0;
        }
    }
}

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
#define KILO_QUIT_TIMES 3
void editorProcessKeypress(int fd) {
    /* When the file is modified, requires Ctrl-q to be pressed N times
     * before actually quitting. */
    static int quit_times = 0;

    int c = editorReadKey(fd);
    switch(c) {
    case ENTER:         /* Enter */
        editorInsertNewline();
        break;
    case CTRL_C:        /* Ctrl-c */
        /* We ignore ctrl-c, it can't be so simple to lose the changes
         * to the edited file. */
        break;
    case CTRL_E:        /* Ctrl-e */
       editorDelRow(E.rowoff + E.cy);
       break;
    case CTRL_X:
        while (E.dirty) {
            editorSetStatusMessage("Save file Y/N/ESC?");
	        editorRefreshScreen();
            int c = editorReadKey(fd);
            if ((c== CTRL_C)||(c == ESC)) {
                editorSetStatusMessage("");
                return;
            }
            if ((c == 'y')||(c=='Y')) {
                if (editorSave()) return; /* Failed save aborts. */
                break;
            }
            if ((c == 'n')||(c == 'N')) {
                quit_times = KILO_QUIT_TIMES; /* Confirmed no save. */
                break;
            }
        }
        /* FALLTHROUGH */
    case CTRL_Q:        /* Ctrl-q */
        /* Quit if the file was already saved. */
        if (E.dirty && quit_times < KILO_QUIT_TIMES) {
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                "Press Ctrl-Q %d more times to quit.", KILO_QUIT_TIMES - quit_times);
            quit_times++;
            return;
        }
        /*system("clear");*/
        printf("\033[2J\033[1;1H");
        exit(0);
        break;
    case CTRL_K:
        editorCutRow(E.rowoff + E.cy);
        break;
    case CTRL_U:
        editorUncutRows(E.rowoff + E.cy);
        break;
    case CTRL_O:
    case CTRL_S:        /* Ctrl-s */
        editorSave();
        break;
    case CTRL_W:
    case CTRL_F:
        editorFind(fd);
        break;
    case BACKSPACE:     /* Backspace */
    case CTRL_H:        /* Ctrl-h */
    case DEL_KEY:
        editorDelChar();
        break;
    case HOME_KEY:
       startOfLine();
       break;
    case END_KEY:
       endOfLine();
       break;
    case PAGE_UP:
    case PAGE_DOWN:
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;
    case CTRL_L: /* ctrl+l, clear screen */
        screenSetDirty(0,1);
        break;
    case ESC:
        /* Nothing to do for ESC in this mode. */
        break;
    default:
        editorInsertChar(c);
        break;
    }

    quit_times = 0; /* Reset it to the original value. */
}

void updateWindowSize(void) {
    if (getWindowSize(&E.screenrows,&E.screencols) == -1) {
        perror("Unable to query the screen for size (columns / rows)");
        exit(1);
    }
    E.screenrows -= 2; /* Get room for status bar. */
    if (E.screendirty) free(E.screendirty);
    if (!(E.screendirty = malloc(E.screenrows))) oomeExit();
    memset(E.screendirty, 1, E.screenrows); /* All dirty. */
}

void handleSigWinCh(int unused __attribute__((unused))) {
    updateWindowSize();
    if (E.cy > E.screenrows) E.cy = E.screenrows - 1;
    if (E.cx > E.screencols) E.cx = E.screencols - 1;
    editorRefreshScreen();
}

void initEditor(void) {
    /* We're called only once and E is in .bss, dont bother zeroing things. */
    /* If we end up being called more often, memset the struct to zero. */
    E.scr_is_dirty = 1;
    updateWindowSize();
    signal(SIGWINCH, handleSigWinCh);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr,"Usage: kilo <filename>\n");
        exit(1);
    }

    initEditor();
    editorSelectSyntaxHighlight(argv[1]);
    editorOpen(argv[1]);
    enableRawMode(STDIN_FILENO);
    editorSetStatusMessage(
        "HELP: Ctrl-S (^S):Save | ^Q:quit | ^F:find | ^K:Cut Line | ^U: Uncut");
    while(1) {
        editorRefreshScreen();
        editorProcessKeypress(STDIN_FILENO);
    }
    return 0;
}
