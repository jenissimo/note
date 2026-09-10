/* note_pack.h — the compressed definition packs, and reading one a piece at
 * a time.
 *
 * The packs used to be LZMS, decompressed by Cabinet.dll.  That shipped no
 * decompression code in note at all, which was the appeal, and it cost the
 * editor every machine older than Windows 8: on Windows 95 the call is not
 * there, the packs never open, and since no languages are compiled in any
 * more there is no highlighting whatsoever.  Nineteen kilobytes of resource
 * the binary carries and cannot read.  So the format is note's own now, and
 * the price of that is the two hundred lines next door.
 *
 * The reader is a pull, not a buffer.  A pack is 45 KB of text and the 16-bit
 * MS-DOS build has 28 KB for a document and a few thousand bytes of DGROUP to
 * spare; it can never hold one decompressed, and neither can a 6502.  So
 * note_pack_get() hands back one byte at a time and the only memory the whole
 * thing needs is the window, which a caller walks past and forgets.
 *
 * FORMAT
 *
 * LZSS with a 4 KB window, byte-aligned.  A flag byte carries eight items,
 * low bit first; a 1 bit means a literal byte follows, a 0 bit means two more
 * bytes describing a match:
 *
 *     b0 = (distance - 1) & 0xFF
 *     b1 = ((distance - 1) >> 8) << 4 | code
 *
 * so distance is 1..4096 and `code` is a length: 0..14 mean 3..17, and 15
 * means a third byte follows and the length is 18 + it, up to 273.  Long
 * matches earn their escape byte here — a pack is the same five-line comment
 * header in front of twenty-nine definitions and the same keywords across
 * whole families of languages, so the runs available are hundreds of bytes,
 * not tens.
 *
 * Byte-aligned rather than bit-packed on purpose.  Packing the offset and
 * length across byte boundaries buys a few hundred bytes and costs every
 * target a shift-and-mask on the hot path; a 6502 pays for that in a way an
 * x86 does not, and the point of this format is that the small machines can
 * read it.
 *
 * WINDOW SIZE
 *
 * 4096 is a budget, not a tuning result.  8192 compresses 484 bytes better
 * and does not fit: the DOS build has about 5,800 bytes of DGROUP slack, so
 * an 8 KB window is not affordable on the target this exists to serve, and
 * 2048 gives back 1,588 bytes to save two kilobytes nothing is asking for.
 *
 * The blob begins with an eight-byte header: 'N','P','K','1' and then the
 * uncompressed length, four bytes little-endian.  The length has to be told
 * rather than derived — nothing in the stream announces the end — and the
 * magic is there for the MS-DOS half, which can find a pack by scanning its
 * own file for those four bytes instead of walking a PE resource directory.
 * The '1' is the version: a later format is 'NPK2' and an old reader stops
 * instead of decoding nonsense.
 *
 * The payload is ASCII, guaranteed by tools/compress_packs.ps1, which refuses
 * to build a blob containing a byte over 0x7F.  That is what lets this widen
 * a byte to an nchar with a cast and lets the MS-DOS half read a pack without
 * carrying a UTF-8 decoder.  The packs on disk beside the executable are
 * still UTF-8 and still go through note_decode; this rule is only about the
 * copy compiled in.
 */
#ifndef NOTE_PACK_H
#define NOTE_PACK_H

#include "note_core.h"

#define NOTE_PACK_WINDOW 4096
#define NOTE_PACK_HEADER 8

/* WHERE THE WINDOW AND THE BLOB LIVE
 *
 * On every target but one they are ordinary memory and this is nothing.  The
 * 16-bit real-mode build is the one.  Its near data segment has about a
 * kilobyte spare once the document, the undo ring and the regex engine have
 * had theirs -- note16.map is the number -- and the window alone is four; the
 * compressed catalogue is another twenty-seven.  Neither can be in DGROUP,
 * and that is not the same as neither fitting in the machine: a real-mode
 * program can ask DOS for memory and address it __far, which is what the
 * console backend does.
 *
 * So on that target the blob is a __far pointer and the window is one the
 * caller allocates and hands over, rather than four kilobytes carried inside
 * this struct.  The cost is a segment override on every byte of the inner
 * loop, which is a byte fetch that used to be two bytes of encoding and is
 * now three; the alternative was three themes instead of the catalogue.
 *
 * note_pack_find() is not compiled in that configuration.  It owns a reader
 * at file scope and so would own a window it has nowhere to put, and the one
 * target that wants it this way has no caller for it -- see the streaming
 * catalogue reader in src/platform/console/console_main.c, which needs every
 * definition rather than one.
 */
#ifndef NOTE_PACK_FARMEM
#  if defined(__WATCOMC__) && defined(M_I86)
#    define NOTE_PACK_FARMEM 1
#  else
#    define NOTE_PACK_FARMEM 0
#  endif
#endif

#if NOTE_PACK_FARMEM
#  define NOTE_PACK_FAR __far
#else
#  define NOTE_PACK_FAR
#endif

/* Everything the reader needs, and nothing that outlives a call.  The window
 * dominates it, which is the whole point: this struct is the memory cost of
 * reading a pack of any size. */
typedef struct {
    const unsigned char NOTE_PACK_FAR *src;
    unsigned long        srclen;
    unsigned long        srcpos;
    unsigned long        outlen;    /* what the stream will produce in all */
    unsigned long        outpos;    /* what it has produced so far        */
    unsigned int         flags;     /* flag bits not yet spent            */
    unsigned int         nflags;
    unsigned int         wpos;      /* where the next byte lands          */
    unsigned int         run;       /* bytes still owed by a match        */
    unsigned int         rpos;      /* where that match is reading from   */
#if NOTE_PACK_FARMEM
    unsigned char NOTE_PACK_FAR *win;   /* the caller's, set before open  */
#else
    unsigned char        win[NOTE_PACK_WINDOW];
#endif
} note_pack;

/* Binds a blob to a reader.  `len` is the whole blob, header included.
 * Returns 0 if it is not an NPK1 pack, in which case the reader is not
 * usable and note_pack_get() will report end of stream immediately.
 *
 * Where NOTE_PACK_FARMEM is 1, `z->win` must already point at
 * NOTE_PACK_WINDOW writable bytes; this returns 0 if it does not. */
int note_pack_open(note_pack *z, const unsigned char NOTE_PACK_FAR *blob,
                   unsigned long len);

/* The next byte of the decompressed pack, or -1 at the end of it.  Also -1 on
 * a truncated or corrupt stream: a caller that has read a definition it
 * understands does not need to be told which of the two happened. */
int note_pack_get(note_pack *z);

/* The uncompressed size, from the header, before reading anything. */
unsigned long note_pack_size(const note_pack *z);

/* Finding one definition ---------------------------------------------------
 *
 * A pack is definitions separated by a line of dashes, and opening one .c
 * file used to mean decompressing all 45 KB of them to keep a few hundred
 * bytes.  These find the single definition that claims something and copy
 * only that one out, which is what makes the MS-DOS build possible and is
 * the better answer on Windows too.
 *
 * The scan runs twice over the stream rather than buffering as it goes.  It
 * cannot know a definition is the one it wants until it has read the line
 * that says so, and the definition that claims an extension last is the one
 * that wins, so it must reach the end before it can commit; holding a
 * candidate meanwhile would cost a second document-sized buffer, and the
 * whole reason this exists is that such a buffer is what the small targets do
 * not have.  Decompressing twice costs microseconds and no memory at all.
 */

/* What a caller should size the scratch buffer it passes note_pack_find() by,
 * when it has no better idea.  Guarded like the bounds in note_config.h,
 * because a small target does have a better idea: the shipped languages reach
 * 6,373 characters, but a pack curated for a machine with 28 KB of document
 * space would not be carrying those in the first place. */
#ifndef NOTE_PACK_DEF_MAX
#define NOTE_PACK_DEF_MAX 8192
#endif

/* How `want` is matched against the value of `key`. */
#define NOTE_PACK_WORD  0    /* one space-separated word of it: "c h cpp"  */
#define NOTE_PACK_WHOLE 1    /* the whole value: a theme's name           */

/* Copies the definition whose `key` claims `want` into `out`, NUL-terminated,
 * and returns its length in nchars.  Returns -1 when nothing claims it, and
 * -2 when something does but `out` is too small to hold it — a caller that
 * cannot grow its buffer can still tell "no such language" from "this one
 * does not fit here", which are different bugs.
 *
 * `key` is ASCII because every key in the format is.  Matching is
 * case-insensitive, as note_lang_from_path's is: a file called README.MD is
 * not a different language from one called readme.md.
 */
#if !NOTE_PACK_FARMEM
long note_pack_find(const unsigned char *blob, unsigned long len,
                    const char *key, const nchar *want, int mode,
                    nchar *out, long cap);
#endif

#endif /* NOTE_PACK_H */
