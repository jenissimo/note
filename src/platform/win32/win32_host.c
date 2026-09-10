/* win32_host.c -- the plain services the core asks for: files, folders, the clock
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
 */

#include "note_win32.h"
#include "../../note_res.h"
#include "../../core/note_pack.h"

void pal_open_rename(note_host *h);      /* win32_palette.c */
void pal_open_run(note_host *h);         /* win32_palette.c */

static int h_time_date(note_host *h, nchar *buf, int cap)
{
    SYSTEMTIME st;
    (void)h;
    GetLocalTime(&st);
    return os_time_date(&st, buf, cap);
}

int h_system_dark(note_host *h)
{
    DWORD v = 1;
    (void)h;
    if (!os_reg_dword(HKEY_CURRENT_USER,
            N("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
            N("AppsUseLightTheme"), &v))
        return 0;
    return v ? 0 : 1;
}

static int h_state_dir(note_host *h, nchar *buf, int cap)
{
    DWORD n;
    (void)h;
    n = os_env(N("LOCALAPPDATA"), buf, cap);
    if (!n || n >= (DWORD)cap) return 0;
    n_cat(buf, N("\\note"), cap);
    return 1;
}

static int h_exe_dir(note_host *h, nchar *buf, int cap)
{
    DWORD n = os_module_file_name(h->inst, buf, cap);
    int i;
    if (!n || n >= (DWORD)cap) return 0;
    for (i = (int)n; i > 0; i--)
        if (buf[i - 1] == (nchar)'\\') { buf[i - 1] = 0; return 1; }
    return 0;
}

static int h_dir_make(note_host *h, const nchar *dir)
{
    (void)h;
    return os_create_dir(dir) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int h_dir_list(note_host *h, const nchar *dir, const nchar *ext,
                      nchar *out, int cap)
{
    nchar pattern[NOTE_PATH_MAX];
    os_find f;
    int used = 0, count = 0;
    (void)h;

    pattern[0] = 0;
    n_cat(pattern, dir, NOTE_PATH_MAX);
    n_cat(pattern, N("\\*."), NOTE_PATH_MAX);
    n_cat(pattern, ext, NOTE_PATH_MAX);

    if (!os_find_open(pattern, &f)) { out[0] = 0; return 0; }

    do {
        nchar full[NOTE_PATH_MAX];
        int len;
        if (f.attrs & FILE_ATTRIBUTE_DIRECTORY) continue;

        full[0] = 0;
        n_cat(full, dir, NOTE_PATH_MAX);
        n_cat(full, N("\\"), NOTE_PATH_MAX);
        n_cat(full, f.name, NOTE_PATH_MAX);

        len = n_len(full);
        if (used + len + 2 >= cap) break;
        n_copy(out + used, full, len + 1);
        used += len + 1;
        count++;
    } while (os_find_step(&f));

    os_find_close(&f);
    out[used] = 0;
    return count;
}

static int h_file_read(note_host *h, const nchar *path,
                       unsigned char **bytes, unsigned long *len)
{
    HANDLE f;
    DWORD size, got = 0;
    unsigned char *p;

    f = os_create_file(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    if (f == INVALID_HANDLE_VALUE) return 0;

    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE) { CloseHandle(f); return 0; }

    p = (unsigned char *)h_alloc(h, size + 1);
    if (!p) { CloseHandle(f); return 0; }

    if (size && !ReadFile(f, p, size, &got, NULL)) {
        h_free(h, p); CloseHandle(f); return 0;
    }
    CloseHandle(f);

    p[got] = 0;
    *bytes = p;
    *len   = got;
    return 1;
}

static int h_file_write(note_host *h, const nchar *path,
                        const unsigned char *bytes, unsigned long len)
{
    HANDLE f;
    DWORD  wrote = 0;
    BOOL   ok;
    (void)h;

    f = os_create_file(path, GENERIC_WRITE, 0, CREATE_ALWAYS);
    if (f == INVALID_HANDLE_VALUE) return 0;

    ok = len ? WriteFile(f, bytes, len, &wrote, NULL) : TRUE;
    CloseHandle(f);
    return (ok && wrote == len) ? 1 : 0;
}

/* ---- the definition packs compiled into the executable -------------------
 *
 * These used to be LZMS, decompressed by Cabinet.dll, which meant note shipped
 * no decompression code at all.  That was a good trade until it was run on
 * Windows 95: the API arrived in Windows 8, the call is simply not there, and
 * since no languages are compiled in any more the editor came up with no
 * highlighting whatsoever while carrying 19 KB of definitions it could not
 * read.  The format is note's own now -- see src\core\note_pack.h -- so this
 * path has nothing version-dependent left in it and no library to look up.
 *
 * The whole pack is materialised here because the registry is what the theme
 * and language pickers list; a backend with no pickers wants
 * note_pack_find() instead, which reads one definition and never holds the
 * rest.
 *
 * TWO PLACES IN THE FILE, AND WHY THE THEMES ARE NOT IN .rsrc
 *
 * The syntax pack is a PE resource.  The theme catalogue is not: it is
 * appended to the end of the executable, past the last section, and read by
 * opening the file.  That is not a preference, it is the only region both
 * halves of this file can reach.  note.exe is a dual binary -- the PE for
 * Windows and, in the MZ stub at the front of it, a 16-bit real-mode editor
 * for MS-DOS -- and a real-mode program cannot walk a PE resource directory,
 * nor run the loader that would expand one for it.  What it can do is open
 * its own executable and seek, so an overlay is what the two can agree on.
 *
 * One physical copy, and this is the half with the easier job of finding it.
 * Both halves use the same rule: the last NPK1 in the last 64 KB of the file.
 * The last, because four bytes of magic occur by chance in a couple of
 * hundred kilobytes of code and the build appends this after everything else.
 * -------------------------------------------------------------------------- */

/* The appended blob, moved to the front of its own allocation so the caller
 * has one pointer to free.  Zero if this executable carries no overlay, which
 * is what a build without tools/compress_packs.ps1 produces and is not an
 * error -- the built-in light and dark palettes are still a theme list. */
static unsigned char *h_overlay_pack(note_host *h, DWORD *len)
{
    nchar  path[NOTE_PATH_MAX];
    HANDLE f;
    DWORD  size, start, want, got = 0, i;
    long   found = -1;
    unsigned char *tail;

    *len = 0;
    if (!os_module_file_name(h->inst, path, NOTE_PATH_MAX)) return 0;

    f = os_create_file(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    if (f == INVALID_HANDLE_VALUE) return 0;

    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size <= NOTE_PACK_HEADER) {
        CloseHandle(f);
        return 0;
    }

    /* The same 64 KB window the MS-DOS half scans, so the two cannot
     * disagree about which NPK1 is the last one. */
    start = size > 65536uL ? size - 65536uL : 0uL;
    want  = size - start;

    tail = (unsigned char *)h_alloc(h, want);
    if (!tail) { CloseHandle(f); return 0; }

    SetFilePointer(f, (LONG)start, NULL, FILE_BEGIN);
    if (!ReadFile(f, tail, want, &got, NULL)) {
        h_free(h, tail); CloseHandle(f); return 0;
    }
    CloseHandle(f);

    for (i = 0; i + NOTE_PACK_HEADER <= got; i++)
        if (tail[i] == 0x4E && tail[i + 1] == 0x50 &&
            tail[i + 2] == 0x4B && tail[i + 3] == 0x31) found = (long)i;

    if (found < 0) { h_free(h, tail); return 0; }

    got -= (DWORD)found;
    for (i = 0; i < got; i++) tail[i] = tail[i + (DWORD)found];
    *len = got;
    return tail;
}

static int h_embedded_pack(note_host *h, int which, nchar **out)
{
    note_pack *z;
    const unsigned char *p;
    unsigned char *owned = 0;
    DWORD   res_size = 0;
    unsigned long raw_size, i;
    nchar  *text;

    *out = 0;

    if (which) {
        /* Themes: the overlay, which is the curated pack and the only copy
         * of it in the file.  A themes.pack found on disk afterwards adds
         * the rest of the catalogue and overrides these by name. */
        owned = h_overlay_pack(h, &res_size);
        p = owned;
    } else {
        HRSRC   res;
        HGLOBAL blob;

        res = os_find_resource(h->inst, IDR_CORE_SYNTAX, RT_NOTEPACK);
        if (!res) return 0;
        res_size = SizeofResource(h->inst, res);
        blob     = LoadResource(h->inst, res);
        if (!blob) return 0;
        p = (const unsigned char *)LockResource(blob);
    }
    if (!p || res_size <= NOTE_PACK_HEADER) {
        if (owned) h_free(h, owned);
        return 0;
    }

    /* The reader is mostly its 4 KB window, which is more than this frame
     * should carry, and it is finished with before the call returns. */
    z = (note_pack *)h_alloc(h, sizeof(note_pack));
    if (!z) { if (owned) h_free(h, owned); return 0; }

    if (!note_pack_open(z, p, res_size)) {
        h_free(h, z); if (owned) h_free(h, owned); return 0;
    }

    raw_size = note_pack_size(z);
    if (!raw_size || raw_size > 16uL * 1024uL * 1024uL) {
        h_free(h, z); if (owned) h_free(h, owned); return 0;
    }

    text = (nchar *)h_alloc(h, (raw_size + 1) * sizeof(nchar));
    if (!text) { h_free(h, z); if (owned) h_free(h, owned); return 0; }

    /* A byte is an nchar: the embedded pack is ASCII by construction, which
     * tools\compress_packs.ps1 refuses to let stop being true. */
    for (i = 0; i < raw_size; i++) {
        int c = note_pack_get(z);
        if (c < 0) {
            h_free(h, text); h_free(h, z);
            if (owned) h_free(h, owned);
            return 0;
        }
        text[i] = (nchar)(unsigned char)c;
    }
    text[raw_size] = 0;
    h_free(h, z);
    if (owned) h_free(h, owned);

    *out = text;
    return 1;
}

void h_file_delete(note_host *h, const nchar *path)
{
    (void)h;
    if (path && path[0]) os_delete_file(path);
}

static int h_file_exists(note_host *h, const nchar *path)
{
    DWORD attr;
    (void)h;
    if (!path || !path[0]) return 0;
    attr = os_file_attrs(path);
    return attr != INVALID_FILE_ATTRIBUTES;
}

/* os_move_file moves across volumes where it can: a plain rename fails there,
 * and the user asked for the file to end up in the new place, not for a
 * lecture about partitions.  Replacing an existing target is deliberately not
 * asked for — the core has already refused a name that exists, and this is the
 * second lock on the same door. */
static int h_file_rename(note_host *h, const nchar *from, const nchar *to)
{
    (void)h;
    if (!from || !from[0] || !to || !to[0]) return 0;
    return os_move_file(from, to);
}

/* The palette is the only surface note asks a free-text question on, so it is
 * where an answer of "no, because..." belongs. */
void h_set_hint_text(note_host *h, const nchar *text)
{
    n_copy((nchar *)h->pal_msg, text ? text : N(""),
           (int)(sizeof(h->pal_msg) / sizeof(h->pal_msg[0])));
    if (h->pal && h->pal_open) InvalidateRect(h->pal, NULL, FALSE);
}

static void h_set_hint(note_host *h, const nchar *text)
{
    h_set_hint_text(h, text);
}

static void h_quit(note_host *h)
{
    h->quitting = 1;
    DestroyWindow(h->wnd);
}

/* The core names a list and stops there; on this backend every one of them is
 * a mode of the command palette rather than a dialog, so the answer arrives
 * later, from the overlay's commit callback. */
static void h_pick(note_host *h, int what)
{
    switch (what) {
    case PICK_THEME: pal_open_mode(h, PAL_MODE_THEME); break;
    case PICK_FONT:  pal_open_mode(h, PAL_MODE_FONT);  break;
    case PICK_LINE:  pal_open_mode(h, PAL_MODE_LINE);  break;
    /* Rename's mode number is private to win32_palette.c for now; see the
     * comment there.  This is the door it opens instead. */
    case PICK_RENAME: pal_open_rename(h); break;
    case PICK_PATH:   pal_open_path(h);   break;
    case PICK_RUN:    pal_open_run(h);    break;
    default: break;
    }
}

const note_host_ops kOps = {
    h_alloc, h_free,
    h_text_len, h_text_get, h_text_set,
    h_sel_get, h_sel_set, h_sel_replace,
    h_edit_op, h_can_undo, h_set_modified,
    h_tab_create, h_tab_destroy, h_tab_select, h_tab_title,
    h_set_title, h_set_status, h_show_status, h_set_wrap, h_set_linenums,
    h_set_zoom, h_set_theme, h_rehighlight,
    h_dlg_open, h_dlg_save, h_dlg_font, h_dlg_find, h_pick,
    h_dlg_pagesetup, h_dlg_print, h_ask_save, h_message,
    h_find_text, h_goto_line, h_time_date, h_system_dark,
    h_state_dir, h_exe_dir, h_dir_make, h_dir_list,
    h_file_read, h_file_write, h_file_delete, h_embedded_pack,
    h_file_exists, h_file_rename, h_set_hint, help_show, h_quit
};

