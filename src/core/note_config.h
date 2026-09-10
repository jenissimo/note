/* note_config.h — compile-time size and feature profile.
 *
 * Every buffer bound and optional subsystem in the core is named here rather
 * than scattered through the code, so a port to a machine with a few tens of
 * kilobytes (MS-DOS real mode, or a 6502 with cc65) is a matter of picking a
 * profile instead of editing the core.
 *
 * Define NOTE_PROFILE_TINY to build the small profile.
 *
 * Rules the core follows so those ports stay possible:
 *   - C89 only; no // comments, no declarations after statements.
 *   - Never assume int is wider than 16 bits.  Anything that must hold a file
 *     offset uses long.
 *   - No CRT, no OS headers, no floating point.
 */
#ifndef NOTE_CONFIG_H
#define NOTE_CONFIG_H

#ifdef NOTE_PROFILE_TINY

  #define NOTE_PATH_MAX        64
  #define NOTE_FIND_MAX        32
  #define NOTE_MAX_DOCS         1
  #ifndef NOTE_ARENA_CHARS
  #define NOTE_ARENA_CHARS    512
  #endif
  #ifndef NOTE_MAX_LANGS
  #define NOTE_MAX_LANGS        2
  #endif

  #define NOTE_PALETTE_MAX     24   /* rows one palette list can hold     */
  #define NOTE_PALETTE_LABEL   32   /* "File: Save As...", nchars         */
  #define NOTE_PALETTE_ACCEL   16   /* "Ctrl+Shift+S", nchars             */
  #define NOTE_PALETTE_POOL   256   /* nchars of labels the palette owns  */
  #define NOTE_PALETTE_QUERY   32   /* what has been typed into it        */

  #define NOTE_ENABLE_TABS      0
  #define NOTE_ENABLE_SESSION   0
  #define NOTE_ENABLE_SYNTAX    0
  #define NOTE_ENABLE_THEMES    0
  #define NOTE_ENABLE_UNICODE   0   /* bytes are the platform's own charset */
  #define NOTE_ENABLE_PRINT     0

#else   /* desktop */

  #define NOTE_PATH_MAX       520
  #define NOTE_FIND_MAX       128
  #define NOTE_MAX_DOCS        32
  #ifndef NOTE_ARENA_CHARS
  #define NOTE_ARENA_CHARS 196608
  #endif
  #ifndef NOTE_MAX_LANGS
  #define NOTE_MAX_LANGS      192
  #endif

  /* Big enough for the longest list a palette mode shows, which is the
   * palettes themselves: the shipped pack holds 338. */
  #define NOTE_PALETTE_MAX    512
  #define NOTE_PALETTE_LABEL   64
  #define NOTE_PALETTE_ACCEL   24
  /* A directory listing goes in here a name at a time, so the pool is sized
   * for one rather than for the handful of labels a menu-driven mode builds. */
  #define NOTE_PALETTE_POOL 16384
  /* And the query has to hold a path, not a few words of filter. */
  #define NOTE_PALETTE_QUERY  260

  #define NOTE_ENABLE_TABS      1
  #define NOTE_ENABLE_SESSION   1
  #define NOTE_ENABLE_SYNTAX    1
  #define NOTE_ENABLE_THEMES    1
  #define NOTE_ENABLE_UNICODE   1
  #define NOTE_ENABLE_PRINT     1

#endif

/* The bounds below are guarded rather than plain, so a port that knows its
 * machine better than this file does can set one before including it.  The
 * console build does exactly that: on a C64 the regex program alone, times
 * NOTE_MAX_RULES, is more memory than the machine has.  A definition that
 * arrives from outside wins; everything unset falls back to the profile. */

/* How the line index is built.
 *
 * 1 keeps a checkpoint every few lines and widens the spacing as the document
 * grows, so a ten-megabyte file costs the same per keystroke as a small one.
 * That machinery is about four kilobytes of 6502, which is more than a C64
 * has spare and more than a document of eight kilobytes can ever repay.
 *
 * 0 keeps one entry per line and rescans forward from the edited line when a
 * line break moves.  Both answer the same questions with the same numbers;
 * they differ in what they spend to stay correct, and the right side of that
 * trade depends on how large a document the machine can hold at all. */
/* Defaulted from the compiler rather than left to each backend, because
 * note_buffer.c is its own translation unit on the small targets: a backend
 * that set this after including its own headers would change the shape of
 * note_buffer for itself and not for the buffer, which is a struct layout
 * disagreement rather than an honest error. */
/* Open Watcom in a 16-bit memory model is the third machine that answers the
 * same way, and for the second reason as well as the first: a real-mode
 * document is capped at a few tens of kilobytes by the one data segment it
 * lives in, and the four kilobytes of code the sparse index costs come out of
 * a 64 KB _TEXT that has to fit in front of a PE header. */
#ifndef NOTE_LINE_CHECKPOINTS
#  if defined(__CC65__) || defined(__SDCC) || \
      (defined(__WATCOMC__) && defined(M_I86))
#    define NOTE_LINE_CHECKPOINTS 0
#  else
#    define NOTE_LINE_CHECKPOINTS 1
#  endif
#endif

/* Whether this target can read a definition pack out of its own executable.
 *
 * 1 brings in src/core/note_pack.c: the LZSS reader and the scan that pulls a
 * single definition out of a compressed pack without ever holding the rest.
 * 0 leaves the core with no reference to that file at all, so a build whose
 * source list does not carry it still links.
 *
 * Off on cc65 and SDCC because the machines cannot pay for it and would gain
 * nothing if they could: the C64 links against a hard ceiling with a few
 * hundred bytes to spare, holds eight kilobytes of text and two compiled-in
 * languages, and will never see a 45 KB pack; the Game Boy is smaller again
 * and has no filesystem to read one from.
 *
 * An embedded pack needs somewhere in the file to live.  For the Win32 half
 * that is the PE resource directory.  For DJGPP it is the end of the file:
 * build-retro.bat appends the theme catalogue to note-dos.exe and the backend
 * finds it again by scanning back from the end for the NPK1 magic, which is
 * what that magic is in the format for.  There is nothing PE-specific about
 * that trick, and it is the answer for the MS-DOS stub too whenever someone
 * wires the shared blob up.
 *
 * Deliberately not folded into NOTE_LINE_CHECKPOINTS, tempting as the shared
 * target list looks.  They are different axes and the 16-bit MS-DOS build is
 * the proof: it wants packs and does not want the sparse line index, so
 * keying them together would hand it the wrong half of each.
 */
#ifndef NOTE_EMBEDDED_PACKS
#  if defined(__CC65__) || defined(__SDCC)
#    define NOTE_EMBEDDED_PACKS 0
#  else
#    define NOTE_EMBEDDED_PACKS 1
#  endif
#endif

/* Whether this build carries the theme registry: note_theme.c, the #RRGGBB
 * parsing under it, and a catalogue of definitions read at run time.
 *
 * A theme is one thing everywhere -- a note_theme of RGB triples, written in
 * the note_conf format, mapped onto whatever the display can actually show by
 * note_theme_reduce.  What differs between targets is not what a theme is but
 * how many of them a machine can hold and when the mapping happens.  This
 * switch is that line: 1 means the definitions arrive as text and are parsed
 * and reduced while the program runs; 0 means the same themes, reduced by
 * tools/reduce_themes.py at build time, arrive as a table of palette indices
 * with no parser and no RGB anywhere in the image.
 *
 * Off for cc65, SDCC and 16-bit Watcom, which is a memory answer rather than a
 * taste one.  The registry alone is NOTE_MAX_THEMES note_themes -- twenty-five
 * kilobytes -- against a C64 that has eight for the document and a real-mode
 * DGROUP with about a kilobyte spare once the document, the undo ring and the
 * regex engine have had theirs.  note_theme.c does not compile for the 6502
 * besides.  On for DJGPP and for the Win32 build, both of which have flat
 * memory and do not have to think about the number.
 *
 * Keyed on the compiler for the same reason as everything else here: the
 * console targets build note_buffer.c as a separate translation unit, and a
 * switch that arrived from a backend rather than from this file would be seen
 * by one half of the program and not the other. */
/* Whether the lexer carries pattern rules -- the `rule = number \b0[Xx]...`
 * lines a definition may hold, and the Pike VM that runs them.
 *
 * 0 leaves note_syntax.c with no reference to note_regex at all: no rule is
 * parsed out of a definition, no expression is compiled, and the scanner does
 * not ask at every character whether one matched.  Word lists, comment
 * delimiters, quotes and numbers are the lexer's own and cover most of what a
 * language looks like, which is what the 6502 has been highlighting with all
 * along -- it has never had the engine, only the machinery that drives one.
 * That machinery is about 1.8 KB of dead 6502, which on a machine with a
 * hundred-odd bytes of headroom is not a rounding error.
 *
 * Separate from NOTE_THEME_CATALOGUE despite naming the same two compilers.
 * They are different questions and 16-bit Watcom answers them differently: it
 * wants the regex engine at reduced size -- see NOTE_REGEX_PROG in the console
 * backend -- and cannot afford the theme registry at any size. */
#ifndef NOTE_ENABLE_REGEX
#  if defined(__CC65__) || defined(__SDCC)
#    define NOTE_ENABLE_REGEX 0
#  else
#    define NOTE_ENABLE_REGEX 1
#  endif
#endif

#ifndef NOTE_THEME_CATALOGUE
#  if defined(__CC65__) || defined(__SDCC) || \
      (defined(__WATCOMC__) && defined(M_I86))
#    define NOTE_THEME_CATALOGUE 0
#  else
#    define NOTE_THEME_CATALOGUE 1
#  endif
#endif

/* Whether anything in this build turns "#RRGGBB" into a colour at run time.
 *
 * This used to be the same question as NOTE_THEME_CATALOGUE, and note_conf.c
 * was written that way: only a theme says #RRGGBB, and only the registry read
 * themes.  The 16-bit MS-DOS build separated the two.  It has no registry --
 * NOTE_MAX_THEMES note_themes is twenty-six kilobytes against a real-mode
 * DGROUP with about one to spare -- and it does parse themes, streaming the
 * catalogue out of its own executable and reducing each definition to sixteen
 * palette indices as it passes, so what it keeps is the answer rather than
 * the theme.  The backend defines this to 1 for itself; see CAT16 in
 * src/platform/console/console_main.c. */
#ifndef NOTE_THEME_PARSE
#define NOTE_THEME_PARSE NOTE_THEME_CATALOGUE
#endif

#ifndef NOTE_MAX_THEMES
#define NOTE_MAX_THEMES 384
#endif

/* The largest built-in definition, in characters. */
#ifndef NOTE_BUILTIN_MAX
#define NOTE_BUILTIN_MAX 4096
#endif

/* The buffer one key's value is parsed through, which has to hold the longest
 * single line a definition can carry -- a keyword list.  Deliberately not tied
 * to NOTE_BUILTIN_MAX: a port that compiles in no built-ins at all still has
 * to parse full-size definitions coming from a pack. */
#ifndef NOTE_CONF_VALUE_MAX
#define NOTE_CONF_VALUE_MAX 4096
#endif

/* Syntax rules are regular expressions compiled on demand for the language
 * of the active document only, so the cache is one language's worth. */
#ifndef NOTE_MAX_RULES
#define NOTE_MAX_RULES 24
#endif

/* Longer programs than the engine's own default: a number-literal pattern
 * lifted from a real syntax file runs to well over a hundred instructions. */
#ifndef NOTE_REGEX_PROG
#define NOTE_REGEX_PROG 256
#endif

#endif /* NOTE_CONFIG_H */
