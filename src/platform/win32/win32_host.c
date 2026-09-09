/* win32_host.c -- the plain services the core asks for: files, folders, the clock
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
 */

#include "note_win32.h"
#include "../../note_res.h"

void pal_open_rename(note_host *h);      /* win32_palette.c */

static int h_time_date(note_host *h, nchar *buf, int cap)
{
    SYSTEMTIME st;
    int n;
    (void)h;
    GetLocalTime(&st);
    n = GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, (LPWSTR)buf, cap);
    if (n <= 0) return 0;
    buf[n - 1] = (nchar)' ';
    return GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL,
                          (LPWSTR)(buf + n), cap - n) > 0;
}

int h_system_dark(note_host *h)
{
    DWORD v = 1, cb = sizeof(v);
    (void)h;
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &cb) != ERROR_SUCCESS)
        return 0;
    return v ? 0 : 1;
}

static int h_state_dir(note_host *h, nchar *buf, int cap)
{
    DWORD n;
    (void)h;
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", (LPWSTR)buf, (DWORD)cap);
    if (!n || n >= (DWORD)cap) return 0;
    n_cat(buf, N("\\note"), cap);
    return 1;
}

static int h_exe_dir(note_host *h, nchar *buf, int cap)
{
    DWORD n = GetModuleFileNameW(h->inst, (LPWSTR)buf, (DWORD)cap);
    int i;
    if (!n || n >= (DWORD)cap) return 0;
    for (i = (int)n; i > 0; i--)
        if (buf[i - 1] == (nchar)'\\') { buf[i - 1] = 0; return 1; }
    return 0;
}

static int h_dir_make(note_host *h, const nchar *dir)
{
    (void)h;
    return CreateDirectoryW((LPCWSTR)dir, NULL) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

static int h_dir_list(note_host *h, const nchar *dir, const nchar *ext,
                      nchar *out, int cap)
{
    WCHAR pattern[NOTE_PATH_MAX];
    WIN32_FIND_DATAW fd;
    HANDLE find;
    int used = 0, count = 0;
    (void)h;

    pattern[0] = 0;
    n_cat((nchar *)pattern, dir, NOTE_PATH_MAX);
    n_cat((nchar *)pattern, N("\\*."), NOTE_PATH_MAX);
    n_cat((nchar *)pattern, ext, NOTE_PATH_MAX);

    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) { out[0] = 0; return 0; }

    do {
        nchar full[NOTE_PATH_MAX];
        int len;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        full[0] = 0;
        n_cat(full, dir, NOTE_PATH_MAX);
        n_cat(full, N("\\"), NOTE_PATH_MAX);
        n_cat(full, (const nchar *)fd.cFileName, NOTE_PATH_MAX);

        len = n_len(full);
        if (used + len + 2 >= cap) break;
        n_copy(out + used, full, len + 1);
        used += len + 1;
        count++;
    } while (FindNextFileW(find, &fd));

    FindClose(find);
    out[used] = 0;
    return count;
}

static int h_file_read(note_host *h, const nchar *path,
                       unsigned char **bytes, unsigned long *len)
{
    HANDLE f;
    DWORD size, got = 0;
    unsigned char *p;

    f = CreateFileW((LPCWSTR)path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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

    f = CreateFileW((LPCWSTR)path, GENERIC_WRITE, 0, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;

    ok = len ? WriteFile(f, bytes, len, &wrote, NULL) : TRUE;
    CloseHandle(f);
    return (ok && wrote == len) ? 1 : 0;
}

/* ---- the definition packs compiled into the executable -------------------
 *
 * They are stored LZMS-compressed, each behind a four-byte uncompressed size,
 * because a pack is repetitive text and compresses to under a fifth.  The
 * decompressor is Windows' own, from Cabinet.dll, so note carries no
 * decompression code of its own — the whole cost on this side is one call.
 *
 * Resolved at run time rather than imported: an executable that fails to load
 * because a system DLL is missing is worse than one that quietly falls back to
 * the definitions compiled into the C.
 * -------------------------------------------------------------------------- */

#define COMPRESS_ALGORITHM_LZMS 5

typedef BOOL (WINAPI *PFN_CREATEDECOMP)(DWORD, void *, HANDLE *);
typedef BOOL (WINAPI *PFN_DECOMPRESS)(HANDLE, const void *, SIZE_T,
                                      void *, SIZE_T, SIZE_T *);
typedef BOOL (WINAPI *PFN_CLOSEDECOMP)(HANDLE);

static int h_embedded_pack(note_host *h, int which, nchar **out)
{
    static PFN_CREATEDECOMP create;
    static PFN_DECOMPRESS   decomp;
    static PFN_CLOSEDECOMP  closed;
    static int              probed;

    HRSRC   res;
    HGLOBAL blob;
    const unsigned char *p;
    DWORD   res_size, raw_size;
    HANDLE  dec = NULL;
    unsigned char *raw;
    nchar  *text;
    SIZE_T  used = 0;
    int     cap;

    *out = 0;

    if (!probed) {
        HMODULE cab = LoadLibraryW(L"Cabinet.dll");
        probed = 1;
        if (cab) {
            create = (PFN_CREATEDECOMP)GetProcAddress(cab, "CreateDecompressor");
            decomp = (PFN_DECOMPRESS)  GetProcAddress(cab, "Decompress");
            closed = (PFN_CLOSEDECOMP) GetProcAddress(cab, "CloseDecompressor");
        }
    }
    if (!create || !decomp) return 0;

    res = FindResourceW(h->inst,
                        MAKEINTRESOURCEW(which ? IDR_CORE_THEMES : IDR_CORE_SYNTAX),
                        MAKEINTRESOURCEW(RT_NOTEPACK));
    if (!res) return 0;

    res_size = SizeofResource(h->inst, res);
    blob     = LoadResource(h->inst, res);
    if (!blob || res_size <= 4) return 0;
    p = (const unsigned char *)LockResource(blob);
    if (!p) return 0;

    raw_size = (DWORD)p[0] | ((DWORD)p[1] << 8) |
               ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
    if (!raw_size || raw_size > 16u * 1024u * 1024u) return 0;

    raw = (unsigned char *)h_alloc(h, raw_size + 1);
    if (!raw) return 0;

    if (!create(COMPRESS_ALGORITHM_LZMS, NULL, &dec) ||
        !decomp(dec, p + 4, res_size - 4, raw, raw_size, &used) ||
        used != raw_size) {
        if (dec && closed) closed(dec);
        h_free(h, raw);
        return 0;
    }
    if (closed) closed(dec);
    raw[raw_size] = 0;

    /* The pack is UTF-8; the core wants the platform's own character type. */
    cap  = (int)raw_size + 8;
    text = (nchar *)h_alloc(h, (unsigned long)cap * sizeof(nchar));
    if (!text) { h_free(h, raw); return 0; }

    note_decode(ENC_UTF8, raw, raw_size, text, cap);
    h_free(h, raw);

    *out = text;
    return 1;
}

void h_file_delete(note_host *h, const nchar *path)
{
    (void)h;
    if (path && path[0]) DeleteFileW((LPCWSTR)path);
}

static int h_file_exists(note_host *h, const nchar *path)
{
    DWORD attr;
    (void)h;
    if (!path || !path[0]) return 0;
    attr = GetFileAttributesW((LPCWSTR)path);
    return attr != INVALID_FILE_ATTRIBUTES;
}

/* MOVEFILE_COPY_ALLOWED so that moving to another volume works: a plain
 * rename there fails, and the user asked for the file to end up in the new
 * place, not for a lecture about partitions.  MOVEFILE_REPLACE_EXISTING is
 * deliberately absent — the core has already refused a target that exists,
 * and this is the second lock on the same door. */
static int h_file_rename(note_host *h, const nchar *from, const nchar *to)
{
    (void)h;
    if (!from || !from[0] || !to || !to[0]) return 0;
    return MoveFileExW((LPCWSTR)from, (LPCWSTR)to, MOVEFILE_COPY_ALLOWED) ? 1 : 0;
}

/* The palette is the only surface note asks a free-text question on, so it is
 * where an answer of "no, because..." belongs. */
static void h_set_hint(note_host *h, const nchar *text)
{
    n_copy((nchar *)h->pal_msg, text ? text : N(""),
           (int)(sizeof(h->pal_msg) / sizeof(h->pal_msg[0])));
    if (h->pal && h->pal_open) InvalidateRect(h->pal, NULL, FALSE);
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
    h_file_exists, h_file_rename, h_set_hint, h_quit
};

