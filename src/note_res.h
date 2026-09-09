/* note_res.h — resource identifiers, shared by note.rc and the backend.
 *
 * Kept in one place so the .rc and the code that calls FindResource cannot
 * drift apart.  This header is included by the resource compiler as well as
 * by C, so it must hold nothing but #defines.
 */
#ifndef NOTE_RES_H
#define NOTE_RES_H

/* The application icon.  Explorer reads this out of the file without running
 * it, which is why it has to be a resource rather than something loaded at
 * startup from a system DLL. */
#define IDI_NOTE            1

/* The curated definition packs, compressed with LZMS and each prefixed with a
 * 4-byte little-endian uncompressed size.  Embedding them is what makes
 * note.exe a complete editor on its own; the full packs on disk still load
 * afterwards and override these by name. */
#define IDR_CORE_SYNTAX     2
#define IDR_CORE_THEMES     3

/* Custom resource type for the two above. */
#define RT_NOTEPACK         256

#endif /* NOTE_RES_H */
