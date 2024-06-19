#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#define CtrlKey(k) (k & 31)
#define TAB_STOP 4
#define MSG_TIME 5

#define ABUF_INIT {NULL, 0}

enum cursorKeys {
    TAB_KEY = 9,
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY
};

enum modes {
    NORMAL = 0,
    INSERT
};

typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *render;
} erow ;

struct config {
    int mode;
    int cx;
    int cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    char* filename;
    char* tmpFileExt;
    _Bool mod;
    char statusmsg[80];
    char* help;
    time_t statusmsg_time;
    struct termios orig_term;
};

struct abuf {
    char *b;
    int len;
};

void setStatusMsg(const char *fmt, ...); 
char *commandPrompt(char *);
void disableRawMode();
void freeEditor();

void abAppend(struct abuf *ab, char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}
struct config E;

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J" , 4);
    write(STDOUT_FILENO, "\x1b[H" , 3);
    freeEditor();
    disableRawMode();
    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_term) == -1) die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_term) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_term;

    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int readKeypress() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if (nread == -1) die("read");
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
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
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
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
    }
    return c;
}

int getWindowSize(int *rows, int *cols){
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return -1;
    } else {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return 0;
    }
}

void updateRow(erow *row) {
    int tabs = 0;
    for (int i = 0; i < row->size; ++i) {
        if (row->chars[i] == '\t') tabs++;
    }
    free(row->render);
    row->render = (char*) malloc(row->size + tabs*(TAB_STOP - 1) + 1);

    int idx = 0;
    for (int i = 0; i < row->size; ++i) {
        if (row->chars[i] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
        } else { 
            row->render[idx++] = row->chars[i];
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;
}

void insertRow(int idx, char *s, size_t len) {
    if (idx < 0 || idx > E.numrows) return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[idx + 1], &E.row[idx], sizeof(erow) * (E.numrows - idx));
    E.row[idx].size = len;
    E.row[idx].chars = (char*) malloc(len + 1);
    memcpy(E.row[idx].chars, s, len);
    E.row[idx].chars[len] = '\0';
    E.row[idx].rsize = 0;
    E.row[idx].render = NULL;
    updateRow(&E.row[idx]);
    E.numrows++;

    E.mod = 1;
}

int cxToRx(erow *row, int cx) {
    int rx = 0;
    for (int i = 0; i < cx; ++i) {
        if (row->chars[i] == '\t') rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        rx++;
    }
    return rx;
}

void rowInsertChar(erow *row, int idx, int c) {
    if (idx < 0 || idx > row->size) idx = row->size;

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[idx + 1], &row->chars[idx], row->size - idx + 1);
    row->chars[idx] = c;
    row->size++;
    updateRow(row);

    E.mod = 1;
}

void rowDelChar(erow *row, int idx) {
    if (idx < 0 || idx > row->size) return;
    memmove(&row->chars[idx], &row->chars[idx + 1], row->size - idx);
    row->size--;
    updateRow(row);
    E.mod = 1;
}

void rowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    updateRow(row);
    E.mod = 1;
}


void freeRow(erow *row) {
    free(row->render);
    free(row->chars);
}

void delRow(int idx) {
    if (idx < 0 || idx >= E.numrows) return;
    freeRow(&E.row[idx]);
    memmove(&E.row[idx], &E.row[idx + 1], sizeof(erow) * (E.numrows - idx - 1));
    E.numrows--;
    E.mod = 1;
}
void delChar() {
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;
    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        rowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        rowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        delRow(E.cy);
        E.cy--;
    }
}

void insertChar(char c) {
    if (E.cy == E.numrows) insertRow(E.numrows, "", 0);
    rowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void insertNewline() {
    if (E.cx == 0) {
        insertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        insertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        updateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

char *rowsToString(int *len) {
    int tmplen = 0;
    for (int i  = 0; i < E.numrows; ++i) {
        tmplen += E.row[i].size + 1;
    }
    *len = tmplen;
    char *s = (char*) malloc(tmplen);
    char *p = s;
    for (int i = 0; i < E.numrows; ++i) {
        memcpy(p, E.row[i].chars, E.row[i].size);
        p += E.row[i].size;
        *p = '\n';
        p++;
    }

    return s;
}
void fileSave() {
    if (E.filename == NULL) {
        E.filename = commandPrompt("Save as: %s [ESC to Cancel]");
        if (E.filename == NULL) {
            setStatusMsg("Save Aborted");
            return;
        }
    }
    int len;
    char *buf = rowsToString(&len);
    int filenameLen = strlen(E.filename);
    int tmpExtLen = strlen(E.tmpFileExt);
    int tmpFilenameLen = filenameLen + tmpExtLen + 1;
    char* tmpfilename = (char*) malloc(tmpFilenameLen);
    memcpy(tmpfilename, E.filename, filenameLen);
    memcpy(&tmpfilename[filenameLen], E.tmpFileExt, tmpExtLen);
    tmpfilename[tmpFilenameLen] = '\0';

    int fd = open(tmpfilename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) { 
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                if (rename(tmpfilename, E.filename) != -1){
                    free(tmpfilename);
                    setStatusMsg("\"%s\" %dL, %dB written", E.filename, E.numrows, len);
                    E.mod = 0;
                    return;
                } else {
                    setStatusMsg("Couldn't overwrite \"%s\": %s", E.filename, strerror(errno));
                }
            } else {
                setStatusMsg("Failed to write changes to temp file: %s", strerror(errno));
            }
        } else {
            setStatusMsg("ftruncate failed: %s", strerror(errno));
        }
        close(fd);
    }
    free(buf);
    free(tmpfilename);
}

void fileOpen(char* filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = NULL;

    if (access(E.filename, 'r') == F_OK) {
        fp = fopen(E.filename, "r");
    } else {
        int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
        close(fd);
        fp = fopen(E.filename, "r");
    }
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1){
        if (linelen != -1) {
            while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) linelen--;
            insertRow(E.numrows, line, linelen);
        }
    }
    free(line);
    fclose(fp);

    E.mod = 0;
}

void scroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = cxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) { 
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) { 
        E.coloff = E.rx - E.screencols + 1;
    }
}

void drawRows(struct abuf *ab) {
    for (int i = 0; i < E.screenrows; ++i) {
        int filerow = i + E.rowoff;
        if (filerow >= E.numrows) { 
            abAppend(ab, "~", 1);
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        abAppend(ab, "\x1b[K\r\n" , 5);
    }
}

void drawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[1;7m", 6);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%s | %.20s %s", E.mode == 0 ? "NORMAL" : "INSERT", E.filename ? E.filename : "[No Name]", E.mod ? "| [modified]" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d%% | %d:%d", E.numrows != 0 ? 100 * (E.cy + 1) / E.numrows : 0, E.cy + 1, E.cx + 1);
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        }
        abAppend(ab, " ", 1);
        len++;
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void drawMsg(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);

    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < MSG_TIME) abAppend(ab, E.statusmsg, msglen);
}
void setStatusMsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}
void updateScreen() {
    scroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H" , 3);
    
    drawRows(&ab);
    drawStatusBar(&ab);
    drawMsg(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1, E.rx - E.coloff + 1);
    abAppend(&ab, buf , strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

char *commandPrompt(char *prompt) {
    size_t bufsize = 128;
    char *buf = (char*) malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';
    
    while (1) {
        setStatusMsg(prompt, buf);
        updateScreen();

        int c = readKeypress();
        if (c == BACKSPACE || c == CtrlKey('h')) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            setStatusMsg("");
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                setStatusMsg("");
                return buf;
            }
        } else if(!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}
void freeEditor() {
    free(E.filename);
    free(E.help);
    /* free(E.statusmsg); */
    free(E.tmpFileExt);
    for (int  i = 0; i < E.numrows; ++i) {
        freeRow(&E.row[i]);
    }
    free(E.row);
}
void quitEditor(){ 
    write(STDOUT_FILENO, "\x1b[2J" , 4);
    write(STDOUT_FILENO, "\x1b[H" , 3);
    freeEditor();
    exit(0);
}
void editorCommand() {
    char *command = NULL;
    command = commandPrompt(":%s");
    if (command == NULL) {
        setStatusMsg("Command Aborted");
        return;
    }
    int commandLen = strlen(command);
    while(command[commandLen - 1] == ' ' || command[commandLen - 1] == '\t') {
        command[commandLen - 1] = '\0';
        commandLen--;
    }
    if (commandLen > 2 && command[0] != 'o') {
        setStatusMsg("Invalid Command");
        free(command);
        return;
    }
    switch (command[0]) {
        case 'w':
            fileSave();
            if ((commandLen > 1) && (command[1] == 'q')) {
                free(command);
                quitEditor();
            }
            break;
        case 'q':
            if (E.mod == 0) {
                free(command);
                quitEditor();
            } else {
                if ((commandLen > 1) && (command[1] == '!')) {
                    free(command);
                    quitEditor();
                } else {
                    setStatusMsg("You have Unsaved Changes. Type :q! to exit without saving.");
                }
            }
            break;
        case 'o':
            if (commandLen >= 3 && command[1] == ' ') {
                if (E.filename == NULL) {
                    if (E.numrows == 0) {
                        fileOpen(&command[2]);
                    } else if (E.numrows > 0){
                        setStatusMsg("Unsaved Changes detected");
                    }
                } else {
                    setStatusMsg("Already a file is open");
                }
            } else {
                setStatusMsg("Invalid Command");
            }
            break;
        default:
            setStatusMsg("Invalid Command");
            break;
    }
    free(command);
}
void moveCursor(int c) {
    erow *currRow = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (c) {
        case ARROW_LEFT:
        case 'h':
            if (E.cx > 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
        case 'l':
            if (currRow && E.cx <= currRow->size - 1) {
                E.cx++;
            } else if (currRow && E.cx == currRow->size && E.mode == INSERT && E.cy != E.numrows - 1) {
                E.cy++;
                E.cx = 0;
            }
            break;
    
        case ARROW_UP:
        case 'k':
            if (E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
        case 'j':
            if (E.cy < E.numrows - 1) E.cy++;
            break;
    }
    currRow = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int currRowLen = currRow ? currRow->size : 0;
    if (E.cx > currRowLen) {
        E.cx = currRowLen;
    }
    if (E.mode == NORMAL && E.cx == currRowLen) E.cx--;
}

void handleKeypress() {
    int c = readKeypress();
    switch (c) {
        case CtrlKey('q'):
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            moveCursor(c);
            break;
        case 'h':
        case 'j':
        case 'k':
        case 'l':
            if (E.mode == NORMAL) {
                moveCursor(c);
            } else if (E.mode == INSERT) {
                insertChar(c);
            }
            break;
        case 'i':
        case 'a':
            if (E.mode == NORMAL) {
                E.mode = INSERT;
                if (E.numrows != 0) {
                    if ((c == 'a') && (E.cx < E.row[E.cy].size)) {
                        moveCursor(ARROW_RIGHT);
                    }
                }
            } else if (E.mode == INSERT) {
                insertChar(c);
            }
            break;
        case ':':
            if (E.mode == NORMAL) {
                editorCommand();
            } else if (E.mode == INSERT) {
                insertChar(c);
            }
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
                int t = E.screenrows;
                while(t--) moveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case CtrlKey('a'):
            setStatusMsg("%s", E.help);
            break;
        case END_KEY:
            if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
            if (E.mode == NORMAL) E.cx--;
            break;
        case '0':
            if (E.mode == NORMAL) {
                E.cx = 0;
            } else if (E.mode == INSERT) {
                insertChar(c);
            }
            break;
        case '$':
            if (E.mode == NORMAL) {
                if (E.cy < E.numrows) E.cx = E.row[E.cy].size - 1;
            } else if (E.mode == INSERT) {
                insertChar(c);
            }
            break;

        case DEL_KEY:
            if ((E.cx < E.row[E.cx].size || E.cy < E.numrows - 1) && E.mode == INSERT) {
                moveCursor(ARROW_RIGHT);
                delChar();
            } 
            break;
        case CtrlKey('h'):
        case BACKSPACE:
            if (E.mode == INSERT) delChar();
            break;
        case 'x':
            if (E.mode == NORMAL && E.row[E.cy].size > 0) {
                E.cx++;
                delChar();
            } else if (E.mode == INSERT) {
                insertChar(c);
            }
            break;
        case '\x1b':
            E.mode = NORMAL;
            if (E.numrows > 0) {
                if (E.cx == E.row[E.cy].size) E.cx--;
            }
            break;
        case '\r':
            if (E.mode == INSERT) insertNewline();
            break;
        case 'o':
            if (E.mode == NORMAL) {
                E.mode = INSERT;
                E.cx = E.row[E.cy].size;
                insertNewline();
                E.cx = 0;
            } else if (E.mode == INSERT) {
                insertChar(c);
            }
            break;
        case 'O':
            if (E.mode == NORMAL) {
                E.mode = INSERT;
                insertRow(E.cy, "", 0);
                E.cx = 0;
            } else if (E.mode == INSERT) {
                insertChar(c);
            }
            break;

        default:
            if ((E.mode == INSERT) && !(iscntrl(c) && c != TAB_KEY)) insertChar(c);
            break;
    }
}

void init() {
    E.mode = NORMAL;
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.filename = NULL;
    E.tmpFileExt = ".ded";
    E.mod = 0;
    E.statusmsg[0] = '\0';
    E.help = "Help | :q  = quit | :w = save | :wq = save and quit | Ctrl-A = help";
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char* argv[]) {
    enableRawMode();
    init();
    if (argc >= 2){
        fileOpen(argv[1]);
    }

    setStatusMsg("%s", E.help);

    while (1) {
        updateScreen();
        handleKeypress();
    }
    return 0;
}
