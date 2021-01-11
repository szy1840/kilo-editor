/* ***includes*** */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include<unistd.h>
#include<termios.h>
#include<stdlib.h>
#include<stdio.h>
#include<stdarg.h>
#include<ctype.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/ioctl.h>
#include<sys/types.h>
#include<time.h>
#include<string.h>

/* ***define*** */
#define CTRL_KEY(k) ((k)&0x1f)  //make it more readable,compared to use ascii representation directly
#define KILO_VERSION "0.0.1"    // use the KILO prefix, lest it collides with something defined in the libiaries
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

enum editorKey{
    BACKSPACE=127,
    ARROW_LEFT=1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/* ***data*** */
typedef struct erow{
    int size;
    int rsize;
    char* chars;
    char* render;
}erow;
struct editorConfig{
    int cx,cy;/* E.cy now refers to the position of the cursor within the text file */
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow* row;
    int dirty;/* We call a text buffer “dirty” if it has been modified since opening or saving the file. */
    char* filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};
struct editorConfig E;

/* ***prototypes*** */
void editorSetStatusMessage(const char* fmt,...);
void editorRefreshScreen();
char* editorPrompt(char* prompt,void (*callback)(char*,int));

/* ***terminal*** */
void die(const char* s){
    write(STDOUT_FILENO,"\x1b[2J",4);
    write(STDOUT_FILENO,"\x1b[H",3);
    perror(s);
    write(STDOUT_FILENO,"\r",2);/* add by myself! */
    exit(1);
}
void disableRawMode(){
    if(tcsetattr(STDIN_FILENO,TCSAFLUSH,&E.orig_termios)==-1)
        die("tcsetattr");
}
void enableRawMode(){
    if(tcgetattr(STDIN_FILENO,&E.orig_termios)==-1) die("tcgetattr");
    atexit(disableRawMode);
    struct termios raw=E.orig_termios;
    raw.c_iflag &=~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /*ICRNL comes from <termios.h>. 
    The I stands for “input flag”, CR stands for “carriage return”, and NL stands for “new line”. 
    */

    raw.c_oflag &=~(OPOST);
    /*I assume POST stands for “post-processing of output”. 
    */

    raw.c_cflag |=CS8;
    /* set the character size to 8 bits per byte,which is done by default */

    raw.c_lflag &=~(ECHO | ICANON | IEXTEN | ISIG);
    /*The c_lflag field is for “local flags”. 
    A comment in macOS’s <termios.h> describes it as a “dumping ground for other state”. 
    So perhaps it should be thought of as “miscellaneous flags”.
    The other flag fields are c_iflag (input flags), c_oflag (output flags), 
    and c_cflag (control flags), all of which we will have to modify to enable raw mode.

    ICANON comes from <termios.h>. 
    Input flags (the ones in the c_iflag field) generally start with I like ICANON does. 
    However, ICANON is not an input flag, it’s a “local” flag in the c_lflag field.
    So that’s confusing.
    */

    raw.c_cc[VMIN]=0;
    raw.c_cc[VTIME]=1;
    /*VMIN and VTIME come from <termios.h>. They are indexes into the c_cc field, which stands for “control characters”, 
    an array of bytes that control various terminal settings.

    The VMIN value sets the minimum number of bytes of input needed before read() can return.
    We set it to 0 so that read() returns as soon as there is any input to be read. 

    The VTIME value sets the maximum amount of time to wait before read() returns.
    It is in tenths of a second, so we set it to 1/10 of a second, or 100 milliseconds. 
    If read() times out, it will return 0, 
    */

    if(tcsetattr(STDIN_FILENO,TCSAFLUSH,&raw)==-1) die("tcsetattr");
    /*TCSAFLUSH argument specifies when to apply the change: 
    in this case, it waits for all pending output to be written to the terminal,
    and also discards any input that hasn’t been read.
    */
}
int editorReadKey(){
    int nread;
    char c;
    while((nread=read(STDIN_FILENO,&c,1))!=1){
        if(nread==-1 && errno!=EAGAIN) die("read");
        /* In Cygwin, when read() times out it returns -1 with an errno of EAGAIN, 
        instead of just returning 0 like it’s supposed to.*/
    }
    if(c=='\x1b'){
        char seq[3];
        if(read(STDIN_FILENO,&seq[0],1)!=1) return '\x1b';
        if(read(STDIN_FILENO,&seq[1],1)!=1) return '\x1b';
        
        if(seq[0]=='['){
            if(seq[1]>='0' && seq[1]<='9'){
                if(read(STDIN_FILENO,&seq[2],1)!=1) return '\x1b';
                if(seq[2]=='~'){
                    switch(seq[1]){
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }else{
                switch (seq[1]){
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }else if(seq[0]=='O'){
            switch(seq[1]){
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    }else{
        return c;
    }
}
int getCursorPosition(int* rows,int* cols){
    char buf[32];
    unsigned int i=0;
    if(write(STDOUT_FILENO,"\x1b[6n",4)!=4) return -1;

    while(i<sizeof(buf)-1){
        if(read(STDIN_FILENO,&buf[i],1)!=1) break;
        if(buf[i]=='R') break;
        i++;
    }
    buf[i]='\0';
    if(buf[0]!='\x1b' || buf[1]!='[') return -1;
    if(sscanf(&buf[2],"%d;%d",rows,cols)!=2) return -1;

    return 0;
}
int getWindowSize(int* rows, int* cols){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==-1 || ws.ws_col==0){ /* why need check again ? */
        if(write(STDOUT_FILENO,"\x1b[999C\x1b[999B",12)!=12) return -1;
        return getCursorPosition(rows,cols);
        /*We are sending two escape sequences one after the other. 
        The C command (Cursor Forward) moves the cursor to the right, 
        and the B command (Cursor Down) moves the cursor down. 
        The argument says how much to move it right or down by. We use a very large value, 999, 
        which should ensure that the cursor reaches the right and bottom edges of the screen.

        The C and B commands are specifically documented to stop the cursor from going past the edge of the screen.
        The reason we don’t use the <esc>[999;999H command is that the documentation doesn’t specify what happens 
        when you try to move the cursor off-screen. 
        */
    }else{
        *rows=ws.ws_row;
        *cols=ws.ws_col;
        return 0;
    }
}

/* ***row operation*** */
int editorRowCxToRx(erow* row,int cx){
    int rx=0;
    int j;
    for(j=0;j<cx;j++){
        if(row->chars[j]=='\t')
            rx+=(KILO_TAB_STOP-1)-(j%KILO_TAB_STOP);
        rx++;
    }
    return rx;
}
int editorRowRxToCx(erow* row,int rx){
    int cur_rx=0;
    int cx;
    for(cx=0;cx<row->size;cx++){
        if(row->chars[cx]=='\t')
            cur_rx+=(KILO_TAB_STOP-1)-(cur_rx%KILO_TAB_STOP);
        cur_rx++;
        if(cur_rx>rx) return cx;/* if cur_rx==rx,we need to wait for the increment of cx in the for loop */
    }
    return cx;/* this line is normally useless */
}
void editorUpdateRow(erow *row){
    int tabs=0;
    int j;
    for(j=0;j<row->size;j++){
        if(row->chars[j]=='\t') tabs++;
    }

    free(row->render);
    row->render=malloc(row->size + tabs*(KILO_TAB_STOP-1) +1);

    int idx=0;
    for(j=0;j<row->size;j++){
        if(row->chars[j]=='\t'){
            row->render[idx++]=' ';
            while(idx%KILO_TAB_STOP!=0) row->render[idx++]=' ';
        }else{
            row->render[idx++]=row->chars[j];
        }
    }
    row->render[idx]='\0';
    row->rsize=idx;
}
void editorInsertRow(int at,char* s,size_t len){
    if(at<0 || at>E.numrows) return;

    E.row=realloc(E.row,sizeof(erow)*(E.numrows+1));/* we need (currrent row number +1) rows */
    memmove(&E.row[at+1],&E.row[at],sizeof(erow)*(E.numrows-at));

    E.row[at].size=len;
    E.row[at].chars=malloc(len+1);
    memcpy(E.row[at].chars,s,len);
    E.row[at].chars[len]='\0';
    E.numrows++;

    E.row[at].rsize=0;
    E.row[at].render=NULL;
    editorUpdateRow(&E.row[at]);

    E.dirty++;/* editorInsertChar() will call this if we need a new row. But why not put it in editorInsertChar()?? */
}
void editorFreeRow(erow* row){
    free(row->chars);
    free(row->render);
}
void editorDelRow(int at){
    if(at<0 || at>=E.numrows) return;/* at (E.cy) counts from 0 and E.numrows counts from 1 */
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at],&E.row[at+1],sizeof(erow)*(E.numrows-at-1));
    E.numrows--;
    E.dirty++;
}
void editorRowInsertChar(erow* row,int at,int c){
    if(at<0||at>row->size) at=row->size;       //but at will never be negative. why check here?
    row->chars=realloc(row->chars,row->size+2);//row->size doesn't count the nul byte
    memmove(&row->chars[at+1],&row->chars[at],row->size-at+1);
    row->size++;
    row->chars[at]=c;
    editorUpdateRow(row);

    E.dirty++;
}
void editorRowAppendString(erow* row,char* s,size_t len){
    row->chars=realloc(row->chars,row->size+len+1);
    memcpy(&row->chars[row->size],s,len);
    row->size+=len;
    row->chars[row->size]='\0';
    editorUpdateRow(row);
    E.dirty++;
}
void editorRowDelChar(erow* row,int at){
    if(at<0 || at>=row->size) return;
    memmove(&row->chars[at],&row->chars[at+1],row->size-at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/* ***editor operations*** */
void editorInsertChar(int c){
    if(E.cy==E.numrows){
        editorInsertRow(E.numrows,"",0);
    }
    editorRowInsertChar(&E.row[E.cy],E.cx,c);
    E.cx++;
}
void editorInsertNewLine(){
    if(E.cx==0){
        editorInsertRow(E.cy,"",0);
    }else{
        erow* row=&E.row[E.cy];
        editorInsertRow(E.cy+1,&row->chars[E.cx],row->size-E.cx);
        row=&E.row[E.cy];/* !!editorInsertRow() calls realloc(),which may change E.row */
        row->size=E.cx;
        row->chars[row->size]='\0';/* do not forget this */
        editorUpdateRow(row);
    }
    E.cx=0;
    E.cy++;
}
void editorDelChar(){/* is actually backspace,and delete is based on backspace*/
    if(E.cy==E.numrows) return;
    if(E.cx==0 && E.cy==0) return;
    erow *row=&E.row[E.cy];
    if(E.cx>0){
        editorRowDelChar(row,E.cx-1);
        E.cx--;
    }else{
        E.cx=E.row[E.cy-1].size;
        editorRowAppendString(&E.row[E.cy-1],row->chars,row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/* ***file i/o*** */
char* editorRowsToString(int* buflen){
    int totlen=0;
    int j;
    for(j=0;j<E.numrows;j++){
        totlen+=E.row[j].size+1;
    }
    *buflen=totlen;
    
    char* buf=malloc(totlen);
    char* p=buf;
    for(j=0;j<E.numrows;j++){
        memcpy(p,E.row[j].chars,E.row[j].size);
        p+=E.row[j].size;
        *p='\n';
        p++;
    }
    return buf;
}
void editorOpen(char* filename){
    free(E.filename);
    E.filename=strdup(filename);
    /* strdup() comes from <string.h>. It makes a copy of the given string,
    allocating the required memory and assuming you will free() that memory. */
    FILE* fp=fopen(filename,"r");
    if(!fp) die("fopen");

    char* line=NULL;
    size_t linecap=0;/* line capacity.getline set the value to tell you how many bytes it allocated */
    ssize_t linelen;
    while((linelen=getline(&line,&linecap,fp))!=-1){
        while(linelen>0 && (line[linelen-1]=='\r' || line[linelen-1]=='\n'))/* truncate the terminal \r and \n  */
            linelen--;                                              /* because we will add them in editorDrawRows()*/
        editorInsertRow(E.numrows,line,linelen);
    }
    free(line);/* get line allocate a piece of memeory,and set `line` to point to it */
    fclose(fp);

    E.dirty=0;
}
void editorSave(){
    if(E.filename==NULL){
        E.filename=editorPrompt("Save as: %s (ESC to cancel)",NULL);
        if(E.filename==NULL){
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char* buf=editorRowsToString(&len);
    /* fd:filedescriptor */
    int fd=open(E.filename,O_RDWR | O_CREAT,0644);
    /* create a new file if it doesn’t already exist (O_CREAT), and open it for reading and writing (O_RDWR).
    Because we used the O_CREAT flag, we have to pass an extra argument containing the mode (the permissions):
    0644 is the standard permissions you usually want for text files. 
    It gives the owner of the file permission to read and write the file, 
    and everyone else only gets permission to read the file.  */

    if(fd!=-1){
        if(ftruncate(fd,len)!=-1){
            if(write(fd,buf,len)==len){
                close(fd);
                free(buf);
                E.dirty=0;
                editorSetStatusMessage("%d bytes written to disk",len);
                return;
            };
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error:%s",strerror(errno));


    /* ftruncate() sets the file’s size to the specified length. 
    If the file is larger than that, it will cut off any data at the end of the file to make it that length. 
    If the file is shorter, it will add 0 bytes at the end to make it that length. */
}

/* ***find*** */
void editorFindCallBack(char* query,int key){
    static int last_match=-1;
    static int direction=1;

    if(key=='\r' || key=='\x1b'){
        last_match=-1;/* search aborted and we need to reset to prepare for the next search */
        direction=1;
        return;
    }else if(key==ARROW_RIGHT || key==ARROW_DOWN){
        direction=1;
    }else if(key==ARROW_LEFT || key==ARROW_UP){
        direction=-1;
    }else{
        last_match=-1;/* query changed and we need to reset */
        direction=1;
    }

    if(last_match==-1) direction=1;
    int current=last_match;
    int i;
    for(i=0;i<E.numrows;i++){
        current+=direction;

        /* wrap around and continue from the top (or bottom) */
        if(current==-1){
            current=E.numrows-1;
        }else if(current==E.numrows){
            current=0;
        }

        erow* row=&E.row[current];
        char* match=strstr(row->render,query);
        if(match){
            last_match=current;
            E.cy=current;
            E.cx=editorRowRxToCx(row,match-row->render);
            /* since they are all addresses of strings, this will be the index */
            /* match will be bigger(in most case) */
            E.rowoff=E.cy;
            /* the tutorial use E.numrows and wait for editorScroll() to convert it to E.cy */
            /* seems not intuitive to me */
            break;
        }
    }
}
void editorFind(){
    int saved_cx=E.cx;
    int saved_cy=E.cy;
    int saved_coloff=E.coloff;
    int saved_rowoff=E.rowoff;
    /* otherwise, when ESC is pressed, the cursor will go to cx=0;cy=0; 
        because match will return the exact address of row->render,(the first row's),because "" will match any string
    */
    char *query=editorPrompt("Search: %s (ESC to cancel | Arrows to go to next match)",editorFindCallBack);
    if(query){
        free(query);
    }else{
        E.cx=saved_cx;
        E.cy=saved_cy;
        E.coloff=saved_coloff;
        E.rowoff=saved_rowoff;
    }
}

/* ***append buffer*** */
struct abuf{
    char *b;
    int len;
};
#define ABUF_INIT {NULL,0}
void abAppend(struct abuf *ab, const char* s,int len){
    char* new=realloc(ab->b,ab->len+len);
    if(new==NULL) return;
    memcpy(&new[ab->len],s,len);
    ab->b=new;
    ab->len+=len;
}
void abFree(struct abuf *ab){
    free(ab->b);
}

/* ***output*** */
void editorScroll(){
    if(E.cy<E.numrows){
        E.rx=editorRowCxToRx(&E.row[E.cy],E.cx);
    }
    if(E.cy<E.rowoff){
        E.rowoff=E.cy;
    }
    if(E.cy>=E.rowoff+E.screenrows){
        E.rowoff=E.cy-E.screenrows+1;
    }
    if(E.rx<E.coloff){
        E.coloff=E.rx;
    }
    if(E.rx>=E.coloff+E.screencols){
        E.coloff=E.rx-E.screencols+1;
    }
}
void editorDrawRows(struct abuf *ab){
    int y;
    for(y=0;y<E.screenrows;y++){
        int filerow=y+E.rowoff;
        if(filerow>=E.numrows){
            if(E.numrows==0 && y==E.screenrows/3){
                char welcome[80];
                int welcomelen=snprintf(welcome,sizeof(welcome),"Kilo editor -- version %s",KILO_VERSION);
                if(welcomelen>E.screencols) welcomelen=E.screencols;
                int padding=(E.screencols-welcomelen)/2;
                if(padding){
                    abAppend(ab,"~",1);
                    padding--;
                }
                while(padding--) abAppend(ab," ",1);
                abAppend(ab,welcome,welcomelen);
            }else{
            abAppend(ab,"~",1);
            }
        }else{
            int len=E.row[filerow].rsize-E.coloff;
            if(len<0) len=0;
            if(len>E.screencols) len=E.screencols;
            abAppend(ab,&E.row[filerow].render[E.coloff],len);
        }
        abAppend(ab,"\x1b[K",3);
        abAppend(ab,"\r\n",2);
    }
}
void editorDrawStatusBar(struct abuf *ab){
    abAppend(ab,"\x1b[7m",4);
    /* The m command (Select Graphic Rendition) causes the text printed 
    after it to be printed with various possible attributes including bold (1), underscore (4), blink (5), 
    and inverted colors (7). For example, you could specify all of these attributes using the command <esc>[1;4;5;7m.
    An argument of 0 clears all attributes, and is the default argument.*/
    char status[80],rstatus[80];
    int len=snprintf(status,sizeof(status),"%.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]",E.numrows,
    E.dirty ? "(modified)" :"");

    int rlen=snprintf(rstatus,sizeof(status),"%d/%d",E.cy+1,E.numrows);
    if(len>E.screencols) len=E.screencols;
    abAppend(ab,status,len);
    while(len<E.screencols){
        if(E.screencols-len==rlen){
            abAppend(ab,rstatus,rlen);
            break;
        }else{
            abAppend(ab," ",1);
            len++;
        }
    }
    abAppend(ab,"\x1b[m",3);
    abAppend(ab,"\r\n",2);/* go to next row: message bar */
}
void editorDrawMessageBar(struct abuf *ab){
    abAppend(ab,"\x1b[K",3);
    int msglen=strlen(E.statusmsg);
    if(msglen>E.screencols) msglen=E.screencols;
    if(msglen && time(NULL)-E.statusmsg_time<4)
        abAppend(ab,E.statusmsg,msglen);
}
void editorRefreshScreen(){
    editorScroll();
    struct abuf ab=ABUF_INIT;
    abAppend(&ab,"\x1b[?25l",6);/* hide cursor */

    abAppend(&ab,"\x1b[H",3);
    /* This escape sequence is only `3` bytes long, and uses the `H` command ([Cursor Position])to position the cursor. 
    The `H` command actually takes two arguments: the row number and the column number at which to position the cursor. 
    */
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf,sizeof(buf),"\x1b[%d;%dH",(E.cy-E.rowoff)+1,(E.rx-E.coloff)+1);
    abAppend(&ab,buf,strlen(buf));

    abAppend(&ab,"\x1b[?25h",6);/* reset cursor */

    write(STDOUT_FILENO,ab.b,ab.len);
    abFree(&ab);
}
void editorSetStatusMessage(const char* fmt,...){
    va_list ap;
    va_start(ap,fmt);
    vsnprintf(E.statusmsg,sizeof(E.statusmsg),fmt,ap);
    va_end(ap);
    E.statusmsg_time=time(NULL);
}

/* ***input*** */
char* editorPrompt(char* prompt,void (*callback)(char*,int)){
    size_t bufsize=128;
    char* buf=malloc(bufsize);
    size_t buflen=0;
    buf[0]='\0';/* !! otherwise when buf is empty,editorSetStatusMessage() will have no idea where the string will stop */

    while(1){
        editorSetStatusMessage(prompt,buf);
        editorRefreshScreen();

        int c=editorReadKey();

        if(c==BACKSPACE || c==DEL_KEY || c==CTRL_KEY('h')){
            if(buflen>0) buf[--buflen]='\0';
        }else if(c=='\x1b'){
            editorSetStatusMessage("");/* seems useless to me..anyway this is a good habit */
            if(callback) callback(buf,c);
            free(buf);
            return NULL;
        }else if(c=='\r'){
            if(buflen!=0){
                editorSetStatusMessage("");
                if(callback) callback(buf,c);
                return buf;
            }
        }else if(!iscntrl(c) && c<128){
            if(buflen==bufsize-1){
                bufsize*=2;
                buf=realloc(buf,bufsize);
            }
            buf[buflen++]=c;
            buf[buflen]='\0';
        }

        if(callback) callback(buf,c);
    }
}
void editorMoveCorsor(int key){
    erow *row=(E.cy>=E.numrows) ? NULL : &E.row[E.cy];
    switch (key)
    {
    case ARROW_LEFT:
        if(E.cx!=0){
            E.cx--;
        }else if(E.cy>0){
            E.cy--;
            E.cx=E.row[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
        if(row && E.cx < row->size){/* cx is initialized to 0 */
            E.cx++;
        }else if(row && E.cx==row->size){
            E.cy++;
            E.cx=0;
        }
        break;
    case ARROW_UP:
        if(E.cy!=0){
        E.cy--;
        }
        break;
    case ARROW_DOWN:
        if(E.cy<E.numrows){
        E.cy++;
        }
        break;
    }

    row=(E.cy>=E.numrows) ? NULL : &E.row[E.cy];
    int rowlen= row ? row->size : 0;
    if(E.cx>rowlen){
        E.cx=rowlen;
    }
}
void editorProcessKeypress(){
    static int quit_times=KILO_QUIT_TIMES;
    int c=editorReadKey();
    switch (c)
    {
    case '\r':
        editorInsertNewLine();
        break;
    case CTRL_KEY('q'):
        if(E.dirty && quit_times>0){
            editorSetStatusMessage("WARNING! File has unsaved changes. "
            "Press Ctrl-Q %d more times to quit.",quit_times);
            quit_times--;
            return;
        }
        write(STDOUT_FILENO,"\x1b[2J",4);
        write(STDOUT_FILENO,"\x1b[H",3);
        exit(0);
        break;

    case CTRL_KEY('s'):
        editorSave();
        break;
    case CTRL_KEY('f'):
        editorFind();
        break;
    case HOME_KEY:
        E.cx=0;
        break;
    case END_KEY:
        if(E.cy<E.numrows){
            E.cx=E.row[E.cy].size;
        }
        break;
    
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        /* Ctrl-H key combination, which sends the control code 8, 
        which is originally what the Backspace character would send back in the day. 
        If you look at the ASCII table, you’ll see that ASCII code 8 is named BS for “backspace”, 
        and ASCII code 127 is named DEL for “delete”. But for whatever reason, 
        in modern computers the Backspace key is mapped to 127 and the Delete key is mapped to the escape sequence <esc>[3~ 
        */
        if(c==DEL_KEY)editorMoveCorsor(ARROW_RIGHT);//here.if you keep press delete,you will erase everything!!!
        editorDelChar();
        break;
    
    case PAGE_UP:  //To scroll up or down a page, we position the cursor either at the top or bottom of the screen,
    case PAGE_DOWN://and then simulate an entire screen’s worth of ↑ or ↓ keypresses.
        {
            if(c==PAGE_UP){
                E.cy=E.rowoff;
            }else if(c==PAGE_DOWN){
                E.cy=E.rowoff+E.screenrows-1;
                if(E.cy>E.numrows) E.cy=E.numrows;
            }
            int times=E.screenrows;
            while(times--){
                editorMoveCorsor(c==PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
        }
        break;
        
    case ARROW_LEFT:
    case ARROW_RIGHT:
    case ARROW_UP:
    case ARROW_DOWN:
        editorMoveCorsor(c);
        break;
    
    case CTRL_KEY('l'):
    case '\x1b':
        break;
    
    default:
        editorInsertChar(c);
        break;
    }

    quit_times=KILO_QUIT_TIMES;
}

/* ***init*** */
void initEditor(){
    E.cx=0;
    E.cy=0;
    E.rx=0;
    E.rowoff=0;
    E.coloff=0;
    E.numrows=0;
    E.row=NULL;
    E.filename=NULL;
    E.dirty=0;
    E.statusmsg[0]='\0';
    E.statusmsg_time=0;
    if(getWindowSize(&E.screenrows,&E.screencols)==-1) die("getWindowSize");
    E.screenrows-=2;
}

int main(int argc, char* argv[]){
    enableRawMode();
    initEditor();
    if(argc>=2){
        editorOpen(argv[1]);
    }
    editorSetStatusMessage("HELP: Ctrl-S=save | Ctrl-Q=quit | Ctrl-F=find");

    while(1){
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
