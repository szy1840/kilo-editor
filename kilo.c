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
#include<regex.h>

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
enum editorHighlight{
    HL_NORMAL=0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,/* actual keywords in one color and common type names in the other color */
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

typedef void (*pfuncPtr)(void);
/* ***data*** */
struct editorSyntax{
    char* filetype;
    char** filematch;
    char** keywords;
    char* singleline_comment_start;
    char* multiline_comment_start;
    char* multiline_comment_end;
    int flags;
    /* flags is a bit field that will contain flags for
    whether to highlight numbers and whether to highlight strings for that filetype */
};
typedef struct erow{
    int idx;
    int size;
    int rsize;
    char* chars;
    char* render;
    unsigned char* hl;/* highlight info */
    int hl_open_comment;
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
    struct editorSyntax* syntax;
    pfuncPtr* processfuncs;
    char** pfuncname;
    int pfuncnum;
    struct termios orig_termios;
};
struct editorConfig E;

/* ***filetypes*** */
char* C_HL_extensions[]={".c",".h",".cpp",NULL};/* the array must be terminated with NULL */
char* C_HL_keywords[]={
    "switch", "if", "while", "for", "break", "continue", "return", "else",
  "struct", "union", "typedef", "static", "enum", "class", "case",
  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|","const|","volatile|",NULL
};

struct editorSyntax HLDB[]={    /* HLDB, highlight data base */
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//","/*","*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};
#define HLDB_ENTRIES (sizeof(HLDB)/sizeof(HLDB[0]))/* store the length of the HLDB array */

/* ***prototypes*** */
void editorSetStatusMessage(const char* fmt,...);
void editorRefreshScreen();
char* editorPrompt(char* prompt,void (*callback)(char*,int));
void editorInsertRow(int at,char* s,size_t len);
void editorDelRow(int at);

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

/* **syntax highlighting** */
int is_seperator(int c){
    return isspace(c) || c=='\0' || strchr(",.()+-/*=~%<>[];",c)!=NULL;
}
void editorUpdateSyntax(erow* row){
    row->hl=realloc(row->hl,row->rsize);
    memset(row->hl,HL_NORMAL,row->rsize);

    if(E.syntax==NULL) return;

    char** keywords=E.syntax->keywords;/* easier typing */

    char* scs=E.syntax->singleline_comment_start;/* easier typing */
    char* mcs=E.syntax->multiline_comment_start;
    char* mce=E.syntax->multiline_comment_end;

    int scs_len=scs ? strlen(scs) : 0; 
    int mcs_len=mcs ? strlen(mcs) : 0;
    int mce_len=mce ? strlen(mce) : 0;

    int prev_sep=1;/* 1 means true here, and we consider the beginning of a line a seperator */
    int in_string=0;/* store either a double-quote (") or a single-quote (') character as the value of in_string */
    int in_comment=(row->idx > 0 && E.row[row->idx-1].hl_open_comment);
    /* means multicomment here */
    
    int i=0;
    while(i<row->rsize){
        char c=row->render[i];
        unsigned char prev_hl=(i>0) ? row->hl[i-1] : HL_NORMAL;/* if it's the first char in the row */

        if(scs_len && !in_string && !in_comment){/* not in the multipleline comment */
            if(!strncmp(&row->render[i],scs,scs_len)){
                memset(&row->hl[i],HL_COMMENT,row->rsize-i);
                break;
            }
        }
        if(mcs_len && mce_len && !in_string){
            if(in_comment){
                row->hl[i]=HL_MLCOMMENT;
                if(!strncmp(&row->render[i],mce,mce_len)){
                    memset(&row->hl[i],HL_MLCOMMENT,mce_len);
                    in_comment=0;
                    prev_sep=1;
                    i+=mce_len;
                    continue;
                }else{
                    i++;
                    continue;
                }
            }else if(!strncmp(&row->render[i],mcs,mcs_len)){
                memset(&row->hl[i],HL_MLCOMMENT,mcs_len);
                in_comment=1;
                i+=mcs_len;
                continue;
            }
        }

        if(E.syntax->flags & HL_HIGHLIGHT_STRINGS){
            if(in_string){
                row->hl[i]=HL_STRING;
                if(c=='\\' && i+1 < row->rsize){
                    row->hl[i+1]=HL_STRING;
                    i+=2;
                    continue;
                }
                if(c==in_string) in_string=0;
                i++;
                prev_sep=1;
                continue;
            }else{
                if(c=='"' || c=='\''){
                    in_string=c;
                    row->hl[i]=HL_STRING;
                    i++;
                    continue;
                }
            } 
        }

        if(E.syntax->flags & HL_HIGHLIGHT_NUMBERS){
            if((isdigit(c) && (prev_sep || prev_hl==HL_NUMBER)) || 
            (c=='.' && prev_hl==HL_NUMBER)){
                row->hl[i]=HL_NUMBER;
                prev_sep=0;
                i++;
                continue;
            }
        }

        if(prev_sep){
            int j;
            for(j=0;keywords[j];j++){
                int klen=strlen(keywords[j]);
                int kw2=keywords[j][klen-1]=='|';
                if(kw2) klen--;

                if(!strncmp(keywords[j],&row->render[i],klen) && is_seperator(row->render[i+klen])){
                    memset(&row->hl[i],kw2 ? HL_KEYWORD2 : HL_KEYWORD1,klen);
                    i+=klen;
                    break;
                }
            }

            /* in the for loop, we check if keywords[j]!=NULL after every increment of j ,
            so if none of the keywords match, we will stop when keywords[j]==NULL, 
            so we use this to check if we've broken out from the loop*/
            if(keywords[j]!=NULL){
                prev_sep=0;/* in the next loop, c will be the seperator after the keyword, so prev_sep=0 */
                /* but why not just go to the char after the seperator??? */
                continue;
            }

        }

        prev_sep=is_seperator(c);
        i++;

    }
    int changed=(row->hl_open_comment!=in_comment);
    row->hl_open_comment=in_comment;
    if(changed && row->idx+1 < E.numrows)
        editorUpdateSyntax(&E.row[row->idx+1]);
}
void editorSelectSyntaxHighlight(){
    E.syntax=NULL;
    if(E.filename==NULL) return;

    char* ext=strchr(E.filename,'.');
    for(unsigned int j=0;j<HLDB_ENTRIES;j++){
        struct editorSyntax* s=&HLDB[j];
        unsigned int i=0;
        while(s->filematch[i]){
            int is_ext=(s->filematch[i][0]=='.');

            if((is_ext && ext && !strcmp(ext,s->filematch[i])) ||
             (!is_ext && strstr(E.filename,s->filematch[i]))){
                /* strcmp() returns 0 if two given strings are equal */
                E.syntax=s;

                int filerow;
                for(filerow=0;filerow<E.numrows;filerow++){/* but???? seems only editorSave() need to do this */
                    editorUpdateSyntax(&E.row[filerow]);
                }
                return;
            }
            i++;
        } 
    }
}
int editorSyntaxToColor(int hl){
    switch (hl)
    {
    case HL_COMMENT:
    case HL_MLCOMMENT: return 36;//cyan
    case HL_KEYWORD1: return 33;//yellow
    case HL_KEYWORD2: return 32;//green
    case HL_STRING: return 35;//magenta
    case HL_NUMBER: return 31;
    case HL_MATCH: return 34;
    default: return 37;
    }
}

/*process functions*/
void Pfunc_C_AutoPrototype(){
    regex_t regex;
    int reti;
    reti=regcomp(&regex,"^(struct |enum |union )?([a-zA-Z0-9_]+\\**\\s)[a-zA-Z0-9_]+\\(.*\\)\\s?\\{.*",REG_EXTENDED);
    if(reti){
        editorSetStatusMessage("regex fail to compile.");
        return;
    }

    regex_t pt;
    reti=regcomp(&pt,"^(//\\s*prototypes?|/\\*\\s*prototypes?\\s*\\*/)",REG_EXTENDED);
    if(reti){
        editorSetStatusMessage("regex fail to compile.");
        return;
    }

    regex_t fmain;
    reti=regcomp(&fmain,"^(void|int)\\s+main",REG_EXTENDED);
    if(reti){
        editorSetStatusMessage("regex fail to compile.");
        return;
    }

    int count=0;
    int pt_pos=-1;
    for (int i = 0; i < E.numrows; i++){
        reti=regexec(&pt,E.row[i].render,0,NULL,0);
        if(!reti){
            pt_pos=i;
            break;
        }
    }
    if(pt_pos==-1){
        editorInsertRow(0,"/* prototype */",strlen("/* prototype */"));
        editorInsertRow(1,"",0);
        pt_pos=0;
    }

    int i=pt_pos+1;
    while(strcmp(E.row[i].render,"")!=0){
        editorDelRow(i);
    }

    int record_rownum=E.numrows;
    for(int i=record_rownum-1;i>pt_pos;i--){
        reti = regexec(&regex,E.row[i+count].render,0,NULL,0);
            if(!reti){
                char rowbuf[256];
                strncpy(rowbuf,E.row[i+count].render,sizeof(rowbuf));
                if(!regexec(&fmain,rowbuf,0,NULL,0)) continue;
                if(E.row[i+count].hl_open_comment){
                    strncat(rowbuf,"*/",3);
                }
                rowbuf[255]='\0';
                for(int j=strlen(rowbuf);j>=0;j--){
                    if(E.row[i+count].hl[j]==HL_NORMAL){
                        if(rowbuf[j]=='{'){
                            rowbuf[j]=';';
                            break;
                        }
                        if(rowbuf[j]=='}'){
                            int find_bracket=j-1;
                            while(rowbuf[find_bracket]!='{') find_bracket--;
                            rowbuf[find_bracket]=';';
                            rowbuf[find_bracket+1]='\0';
                            break;
                        }
                    }
                }
                editorInsertRow(pt_pos+1,rowbuf,strlen(rowbuf));
                count++;
            }else if(reti!=REG_NOMATCH){
                char errbuf[64];
                regerror(reti, &regex, errbuf, sizeof(errbuf));
                editorSetStatusMessage(errbuf);
                return;
            }
    }
    editorSetStatusMessage("AutoPrototype: %d added/updated.",count);
    editorRefreshScreen();
	regfree(&regex);
    regfree(&pt);
    regfree(&fmain);
}
void hey(){
    editorSetStatusMessage("hello Miss Chen :D");
}
void editorLoadProcessFunc(char* filetype){
    if(strcmp(filetype,"c")==0){
        E.pfuncnum=2;
        E.processfuncs=malloc(sizeof(pfuncPtr)*E.pfuncnum);
        E.processfuncs[0]=Pfunc_C_AutoPrototype;
        E.processfuncs[1]=hey;
        E.pfuncname=malloc(sizeof(char*)*E.pfuncnum);
        E.pfuncname[0]="autopt";
        E.pfuncname[1]="hey";
        return;
    }

    return;
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

    editorUpdateSyntax(row);
}
void editorInsertRow(int at,char* s,size_t len){
    if(at<0 || at>E.numrows) return;

    E.row=realloc(E.row,sizeof(erow)*(E.numrows+1));/* we need (current row number +1) rows */
    memmove(&E.row[at+1],&E.row[at],sizeof(erow)*(E.numrows-at));
    for(int j=at+1;j<=E.numrows;j++) E.row[j].idx++;

    E.row[at].idx=at;
    E.row[at].size=len;/* so size doesn't include the nul byte */
    E.row[at].chars=malloc(len+1);
    memcpy(E.row[at].chars,s,len);
    E.row[at].chars[len]='\0';/* and the row.chars has (size+1) bytes */
    E.numrows++;

    E.row[at].rsize=0;
    E.row[at].render=NULL;
    E.row[at].hl=NULL;
    E.row[at].hl_open_comment=0;
    editorUpdateRow(&E.row[at]);

    E.dirty++;/* editorInsertChar() will call this if we need a new row. But why not put it in editorInsertChar()?? */
}
void editorFreeRow(erow* row){
    free(row->chars);
    free(row->render);
    free(row->hl);
}
void editorDelRow(int at){
    if(at<0 || at>=E.numrows) return;/* at (E.cy) counts from 0 and E.numrows counts from 1 */
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at],&E.row[at+1],sizeof(erow)*(E.numrows-at-1));
    for(int j=at;j<E.numrows-1;j++) E.row[j].idx--;
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

    editorSelectSyntaxHighlight();
    editorLoadProcessFunc(E.syntax->filetype);

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
        editorSelectSyntaxHighlight();
        editorLoadProcessFunc(E.syntax->filetype);
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

    static int saved_hl_line;
    static unsigned char* saved_hl=NULL;
    if(saved_hl){
        memcpy(E.row[saved_hl_line].hl,saved_hl,E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl=NULL;
    }

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

    if(last_match==-1) direction=1;/* if there isn't a last_match, search from the beginning and search forward */
    int current=last_match;/* start from last_match and when we enter the for loop, it will point to the next row */
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

            saved_hl_line=current;
            saved_hl=malloc(row->rsize);
            memcpy(saved_hl,row->hl,row->rsize);
            memset(&row->hl[match-row->render],HL_MATCH,strlen(query));
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
                int welcomelen=snprintf(welcome,sizeof(welcome),"welcome -- version %s",KILO_VERSION);
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

            char* c=&E.row[filerow].render[E.coloff];
            unsigned char* hl=&E.row[filerow].hl[E.coloff];
            int current_color=-1;
            int j;
            for(j=0;j<len;j++){
                if(iscntrl(c[j])){
                    char sym=(c[j]<=26) ? '@'+c[j] : '?';//'@'=64,'A'=65
                    abAppend(ab,"\x1b[7m",4);
                    abAppend(ab,&sym,1);
                    abAppend(ab,"\x1b[m",3);//this turns off all text formatting,including colors.so we check corrent_color
                    if(current_color!=-1){//because we didn't use the strategy that print \x1b before every char 
                        char buf[16];
                        int clen=snprintf(buf,sizeof(buf),"\x1b[%dm",current_color);
                        abAppend(ab,buf,clen);
                    }
                }else if(hl[j]==HL_NORMAL){
                    if(current_color!=-1){
                        abAppend(ab,"\x1b[39m",5);
                        current_color=-1;
                    }
                }else{
                    int color=editorSyntaxToColor(hl[j]);
                    if(current_color!=color){
                        current_color=color;
                        char buf[16];
                        int clen=snprintf(buf,sizeof(buf),"\x1b[%dm",color);
                        abAppend(ab,buf,clen);
                    }
                }
                abAppend(ab,&c[j],1);
            }
            abAppend(ab,"\x1b[39m",5);
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

    int rlen=snprintf(rstatus,sizeof(status),"%s | %d/%d",
    E.syntax ? E.syntax->filetype : "no ft",E.cy+1,E.numrows);

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
    if(msglen && time(NULL)-E.statusmsg_time<3)
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

/* ***process functions*** */
void editorProcess(){
    if(E.pfuncnum){
        int choice=0;
        char* prefix="Available process: ";
        while(1){
            char buf[128];
            memset(buf,'\0',sizeof(buf));
            strcpy(buf,prefix);
            for (int i = 0; i < E.pfuncnum; i++){
                char temp[32];
                if(i==choice){
                    snprintf(temp,sizeof(temp),"\x1b[1m[%s] \x1b[m",E.pfuncname[i]);
                }else{
                    snprintf(temp,sizeof(temp),"[%s] ",E.pfuncname[i]);
                }
                strcat(buf,temp);
            }
            strcat(buf,"(Arrows | Enter | ESC)");
            editorSetStatusMessage(buf);
            editorRefreshScreen();

            int c=editorReadKey();
            switch (c){
                case '\r':
                    E.processfuncs[choice]();
                    return;
                case '\x1b':
                    editorSetStatusMessage("Process: aborted.");
                    return;
                case ARROW_LEFT:
                    choice==0 ? choice=E.pfuncnum-1 :choice--;
                    break;
                case ARROW_RIGHT:
                    choice==E.pfuncnum-1 ? choice=0 : choice++;
                default:
                    break;
            }
        }

    }else{
        editorSetStatusMessage("Process: no available function, process aborted.");
    }
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
void editorMoveCursor(int key){
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
    case CTRL_KEY('p'):
        editorProcess();
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
        if(c==DEL_KEY)editorMoveCursor(ARROW_RIGHT);//here.if you keep press delete,you will erase everything!!!
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
                editorMoveCursor(c==PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
        }
        break;
        
    case ARROW_LEFT:
    case ARROW_RIGHT:
    case ARROW_UP:
    case ARROW_DOWN:
        editorMoveCursor(c);
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
    E.syntax=NULL;
    E.processfuncs=NULL;
    E.pfuncnum=0;
}

int main(int argc, char* argv[]){
    enableRawMode();
    initEditor();
    if(argc>=2){
        editorOpen(argv[1]);
    }
    editorSetStatusMessage("HELP: Ctrl-S=save | Ctrl-Q=quit | Ctrl-F=find | Ctrl-P=Process");

    while(1){
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
