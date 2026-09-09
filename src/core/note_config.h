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
  #define NOTE_ARENA_CHARS    512
  #define NOTE_MAX_LANGS        2

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
  #define NOTE_ARENA_CHARS 196608
  #define NOTE_MAX_LANGS      192

  /* Big enough for the longest list a palette mode shows, which is the
   * palettes themselves: the shipped pack holds 338. */
  #define NOTE_PALETTE_MAX    512
  #define NOTE_PALETTE_LABEL   64
  #define NOTE_PALETTE_ACCEL   24
  #define NOTE_PALETTE_POOL  3072
  #define NOTE_PALETTE_QUERY   64

  #define NOTE_ENABLE_TABS      1
  #define NOTE_ENABLE_SESSION   1
  #define NOTE_ENABLE_SYNTAX    1
  #define NOTE_ENABLE_THEMES    1
  #define NOTE_ENABLE_UNICODE   1
  #define NOTE_ENABLE_PRINT     1

#endif

#define NOTE_MAX_THEMES 384

/* Syntax rules are regular expressions compiled on demand for the language
 * of the active document only, so the cache is one language's worth. */
/* The largest built-in definition, in characters. */
#define NOTE_BUILTIN_MAX 4096

#define NOTE_MAX_RULES 24

/* Longer programs than the engine's own default: a number-literal pattern
 * lifted from a real syntax file runs to well over a hundred instructions. */
#define NOTE_REGEX_PROG 256

#endif /* NOTE_CONFIG_H */
