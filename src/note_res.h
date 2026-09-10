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

/* The curated language pack, in note's own LZSS format -- see
 * src/core/note_pack.h, which owns the description of it.  Embedding it is
 * part of what makes note.exe a complete editor on its own; the full pack on
 * disk still loads afterwards and overrides these by name.
 *
 * There is no IDR_CORE_THEMES beside it any more.  The curated themes live
 * at the end of the file instead, past the last section, because the MS-DOS
 * half of this executable cannot read a PE resource and an overlay is the
 * one region both halves can seek to.  See h_overlay_pack() in
 * src/platform/win32/win32_host.c. */
#define IDR_CORE_SYNTAX     2

/* Custom resource type for the above. */
#define RT_NOTEPACK         256

#endif /* NOTE_RES_H */
