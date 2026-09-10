/* gb_basic.h -- the tokeniser and integer evaluator behind note/gb's BASIC.
 *
 * Nothing in here touches the Game Boy.  It allocates nothing, holds no
 * hardware headers, draws nothing, and never assumes an int is wider than
 * sixteen bits -- which means it also compiles and runs on a desktop, which is
 * where its tests live.  Output leaves through one function pointer; the
 * caller decides whether that is a tile map, a console or a string.
 *
 * Programs are stored tokenised, the way every eight-bit BASIC did it, because
 * on a machine with 8 KB the saving is not stylistic.  A keyword is one byte
 * instead of up to six, an integer literal is two bytes instead of up to six
 * characters, and the interpreter dispatches on a byte rather than comparing
 * strings.  The cost is a detokeniser for LIST, which is fifty lines.
 */
#ifndef GB_BASIC_H
#define GB_BASIC_H

/* Sixteen bits on both targets: `int` on the Game Boy, and deliberately not
 * `int` on the host, so a test cannot pass by having more range than the
 * hardware will. */
typedef short bas_int;

/* ---- tokens -------------------------------------------------------------
 *
 * Everything below 0x80 is literal text: identifiers, operators, punctuation
 * and the contents of string literals, all kept as they were typed.  0x80 and
 * up are keywords, and 0xFF introduces a two-byte integer literal.
 */
enum {
    T_LET = 0x80, T_PRINT, T_INPUT, T_IF, T_THEN, T_GOTO, T_GOSUB, T_RETURN,
    T_FOR, T_TO, T_STEP, T_NEXT, T_REM, T_END, T_STOP, T_CLS, T_PAUSE,
    T_AND, T_OR, T_NOT, T_MOD,
    T_RND, T_ABS, T_SGN, T_JOY,
    T_LE, T_GE, T_NE,               /* <=  >=  <> */
    T_NUM = 0xFF                    /* followed by two bytes, low first */
};

/* ---- errors ------------------------------------------------------------- */
enum {
    BAS_OK = 0,
    BAS_SYNTAX,          /* the line does not parse                     */
    BAS_NO_LINE,         /* GOTO or GOSUB naming a line that is not there */
    BAS_DIV_ZERO,
    BAS_OVERFLOW,        /* the token buffer or a stack ran out          */
    BAS_STOPPED          /* END, STOP, or the user interrupting          */
};

/* ---- the machine --------------------------------------------------------
 *
 * One struct, supplied by the caller, so there is no static state and the
 * tests can run several of these side by side.
 */
typedef struct {
    bas_int  var[26];               /* A..Z; strings are not here yet */
    unsigned char *prog;            /* tokenised lines, see below     */
    int      prog_len;
    int      prog_cap;

    /* Where output goes.  One character at a time is enough: the callers that
     * want a line at a time can buffer, and the Game Boy's does. */
    void   (*out)(char c);

    /* Reads the joypad, for JOY().  May be null, in which case JOY() is 0. */
    bas_int (*joy)(void);

    int      err;
    bas_int  err_line;
} bas_vm;

/* ---- lines --------------------------------------------------------------
 *
 * A stored line is:  [number low] [number high] [length] [length bytes]
 * held in ascending order of number, which makes GOTO a scan and editing an
 * insert.  There is no line index: on a program of a few hundred lines a scan
 * costs less than the memory an index would take away from the program.
 */

/* Turns one line of text into tokens.  Returns the number of bytes written, or
 * -1 if the line does not fit.  The leading line number, if any, is *not*
 * included: it is returned through `number`, and is -1 when the line has none
 * (a direct command).
 *
 * Passing a null `number` says the text is an expression rather than a program
 * line, and nothing is stripped from the front of it.  That is the difference
 * between "7 MOD 3" meaning line 7, and it meaning one. */
int bas_tokenise(const char *text, unsigned char *out, int cap, bas_int *number);

/* The other direction, for LIST and for the editor.  Returns the length
 * written, not counting the terminating NUL. */
int bas_detokenise(const unsigned char *tok, int len, char *out, int cap);

/* Evaluates an expression starting at tok[*at], leaving *at just past it.
 * Sets vm->err and returns 0 on a bad expression. */
bas_int bas_eval(bas_vm *vm, const unsigned char *tok, int len, int *at);

/* Runs one already-tokenised line as an immediate command.  Statement
 * execution for a whole program comes next; this is what makes the editor
 * useful before it exists. */
int bas_exec_line(bas_vm *vm, const unsigned char *tok, int len);

/* Program storage. */
void bas_prog_init(bas_vm *vm, unsigned char *storage, int cap);
/* Inserts, replaces or (with len == 0) deletes a numbered line.  Returns 0 if
 * there is no room. */
int  bas_prog_set(bas_vm *vm, bas_int number, const unsigned char *tok, int len);
/* Byte offset of a line, or -1.  The offset is of the header, not the tokens. */
int  bas_prog_find(bas_vm *vm, bas_int number);

#endif /* GB_BASIC_H */
