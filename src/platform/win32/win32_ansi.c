/* win32_ansi.c -- the one place note's UTF-16 meets a Windows that has none
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the boundary exists at all
 * and what it does and does not convert.
 *
 * Every function here is the same shape: on a Windows whose W entry points are
 * real -- which is every Windows anyone is running -- it is the call that used
 * to stand at the call site, unchanged.  Otherwise it converts to the active
 * codepage, calls the A form, and converts whatever comes back.  Nothing above
 * this file knows which of the two happened.
 */

#include "note_win32.h"

int win_wide = 1;

/* Whether this codepage will take WC_NO_BEST_FIT_CHARS.  See wide_probe(). */
static DWORD acp_flags;

void wide_probe(void)
{
    char probe[8];

    /* GetProcAddress would answer the wrong question here.  Windows 95 exports
     * every W entry point and implements almost none of them: the name is in
     * the table and the call fails, so a lookup finds a function that is not
     * there in any sense that matters.  Only the call itself can tell the two
     * apart, and GetModuleHandleW(NULL) is the cheapest one that has no other
     * effect and no legitimate way to return null. */
    win_wide = GetModuleHandleW(NULL) != NULL;

    /* Best-fit mapping is precisely the failure this boundary exists to
     * prevent: left on, it turns a character the codepage has no room for into
     * a plausible-looking different one -- e with an acute into a bare e --
     * rather than into a mark that says something was lost.  The flag that
     * switches it off postdates the Windows that needs the conversion, and it
     * is rejected outright on a UTF-8 codepage, so it is asked for rather than
     * assumed. */
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, L"A", 1,
                            probe, (int)sizeof probe, NULL, NULL) > 0)
        acp_flags = WC_NO_BEST_FIT_CHARS;
}

/* -------------------------------------------------------------------------
 * Conversion
 * ------------------------------------------------------------------------- */

char *acp_text(const nchar *s, char *dst, int cap)
{
    dst[0] = 0;
    if (!s) return dst;
    if (WideCharToMultiByte(CP_ACP, acp_flags, (LPCWSTR)s, -1,
                            dst, cap, NULL, NULL) <= 0)
        dst[0] = 0;
    return dst;
}

char *acp_path(const nchar *s, char *dst, int cap)
{
    BOOL lost = FALSE;

    dst[0] = 0;
    if (!s) return 0;
    if (WideCharToMultiByte(CP_ACP, acp_flags, (LPCWSTR)s, -1,
                            dst, cap, NULL, &lost) <= 0)
        return 0;
    /* A name with a substitution in it is a different name, or no name at all.
     * Refusing is the only answer that cannot lose a file. */
    if (lost) { dst[0] = 0; return 0; }
    return dst;
}

nchar *wide_of(const char *s, nchar *dst, int cap)
{
    dst[0] = 0;
    if (!s) return dst;
    if (MultiByteToWideChar(CP_ACP, 0, s, -1, (LPWSTR)dst, cap) <= 0)
        dst[0] = 0;
    return dst;
}

/* A counted run rather than a NUL-terminated one, for the two drawing calls
 * that are given a length.  Returns the byte count, which is not the character
 * count on any codepage worth having. */
static int acp_run(const nchar *s, int len, char *dst, int cap)
{
    int n;
    if (len < 0) { acp_text(s, dst, cap); len = 0; while (dst[len]) len++; return len; }
    n = WideCharToMultiByte(CP_ACP, acp_flags, (LPCWSTR)s, len,
                            dst, cap - 1, NULL, NULL);
    if (n < 0) n = 0;
    dst[n] = 0;
    return n;
}

/* -------------------------------------------------------------------------
 * kernel32
 * ------------------------------------------------------------------------- */

HMODULE os_module(const nchar *name)
{
    char a[ACP_MAX];
    if (win_wide) return GetModuleHandleW((LPCWSTR)name);
    return GetModuleHandleA(name ? acp_text(name, a, (int)sizeof a) : 0);
}

HMODULE os_library(const nchar *name)
{
    char a[ACP_MAX];
    if (win_wide) return LoadLibraryW((LPCWSTR)name);
    return LoadLibraryA(acp_text(name, a, (int)sizeof a));
}

void os_command_line(nchar *dst, int cap)
{
    dst[0] = 0;
    if (win_wide) {
        const WCHAR *p = GetCommandLineW();
        n_copy(dst, (const nchar *)(p ? p : L""), cap);
    } else {
        const char *p = GetCommandLineA();
        wide_of(p ? p : "", dst, cap);
    }
}

DWORD os_module_file_name(HMODULE mod, nchar *dst, int cap)
{
    char a[ACP_MAX];
    DWORD n;

    if (win_wide) return GetModuleFileNameW(mod, (LPWSTR)dst, (DWORD)cap);

    n = GetModuleFileNameA(mod, a, (DWORD)sizeof a);
    if (!n || n >= sizeof a) { dst[0] = 0; return 0; }
    wide_of(a, dst, cap);
    return (DWORD)n_len(dst);
}

DWORD os_env(const nchar *name, nchar *dst, int cap)
{
    char an[ACP_MAX], av[ACP_MAX];
    DWORD n;

    if (win_wide) return GetEnvironmentVariableW((LPCWSTR)name, (LPWSTR)dst, (DWORD)cap);

    n = GetEnvironmentVariableA(acp_text(name, an, (int)sizeof an),
                                av, (DWORD)sizeof av);
    if (!n || n >= sizeof av) { dst[0] = 0; return 0; }
    wide_of(av, dst, cap);
    return (DWORD)n_len(dst);
}

DWORD os_current_dir(nchar *dst, int cap)
{
    char a[ACP_MAX];
    DWORD n;

    if (win_wide) return GetCurrentDirectoryW((DWORD)cap, (LPWSTR)dst);

    n = GetCurrentDirectoryA((DWORD)sizeof a, a);
    if (!n || n >= sizeof a) { dst[0] = 0; return 0; }
    wide_of(a, dst, cap);
    return (DWORD)n_len(dst);
}

/* A relative name made absolute against the current directory.  Two callers
 * need it and both need it early: a path handed to another instance is read
 * in that instance's directory rather than this one's, and a path kept in a
 * tab outlives whatever directory note was started in.  Falls back to copying
 * the name through, which is what the editor did with it before. */
void os_full_path(const nchar *path, nchar *dst, int cap)
{
    char a[ACP_MAX], full[ACP_MAX];
    DWORD n = 0;

    if (win_wide) {
        n = GetFullPathNameW((LPCWSTR)path, (DWORD)cap, (LPWSTR)dst, NULL);
        if (n && n < (DWORD)cap) return;
    } else if (acp_path(path, a, (int)sizeof a)) {
        n = GetFullPathNameA(a, (DWORD)sizeof full, full, NULL);
        if (n && n < sizeof full) { wide_of(full, dst, cap); return; }
    }
    n_copy(dst, path, cap);
}

HANDLE os_create_file(const nchar *path, DWORD access, DWORD share, DWORD disp)
{
    char a[ACP_MAX];

    if (win_wide)
        return CreateFileW((LPCWSTR)path, access, share, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (!acp_path(path, a, (int)sizeof a)) return INVALID_HANDLE_VALUE;
    return CreateFileA(a, access, share, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
}

int os_delete_file(const nchar *path)
{
    char a[ACP_MAX];
    if (win_wide) return DeleteFileW((LPCWSTR)path) ? 1 : 0;
    if (!acp_path(path, a, (int)sizeof a)) return 0;
    return DeleteFileA(a) ? 1 : 0;
}

DWORD os_file_attrs(const nchar *path)
{
    char a[ACP_MAX];
    if (win_wide) return GetFileAttributesW((LPCWSTR)path);
    if (!acp_path(path, a, (int)sizeof a)) return INVALID_FILE_ATTRIBUTES;
    return GetFileAttributesA(a);
}

/* MOVEFILE_COPY_ALLOWED is what makes a rename across volumes work, and
 * MoveFileEx is the only call that offers it -- but it is one of the entry
 * points Windows 95 has neither half of, so it is resolved by name and the
 * plain rename stands in where it is missing.  A cross-volume rename then
 * fails, which is what it did on that Windows anyway. */
int os_move_file(const nchar *from, const nchar *to)
{
    typedef BOOL (WINAPI *PFN_MOVEEXW)(LPCWSTR, LPCWSTR, DWORD);
    typedef BOOL (WINAPI *PFN_MOVEEXA)(LPCSTR, LPCSTR, DWORD);
    static PFN_MOVEEXW move_w;
    static PFN_MOVEEXA move_a;
    static int probed;

    char af[ACP_MAX], at[ACP_MAX];

    if (!probed) {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        probed = 1;
        if (k) {
            move_w = (PFN_MOVEEXW)GetProcAddress(k, "MoveFileExW");
            move_a = (PFN_MOVEEXA)GetProcAddress(k, "MoveFileExA");
        }
    }

    if (win_wide) {
        if (move_w)
            return move_w((LPCWSTR)from, (LPCWSTR)to, MOVEFILE_COPY_ALLOWED) ? 1 : 0;
        return MoveFileW((LPCWSTR)from, (LPCWSTR)to) ? 1 : 0;
    }

    if (!acp_path(from, af, (int)sizeof af)) return 0;
    if (!acp_path(to,   at, (int)sizeof at)) return 0;
    if (move_a) return move_a(af, at, MOVEFILE_COPY_ALLOWED) ? 1 : 0;
    return MoveFileA(af, at) ? 1 : 0;
}

int os_create_dir(const nchar *path)
{
    char a[ACP_MAX];
    if (win_wide) return CreateDirectoryW((LPCWSTR)path, NULL) ? 1 : 0;
    if (!acp_path(path, a, (int)sizeof a)) return 0;
    return CreateDirectoryA(a, NULL) ? 1 : 0;
}

/* The A find data is kept alongside the handle rather than converted eagerly:
 * FindNextFile writes into the same struct on every step, so it has to outlive
 * the call that started the walk. */
static WIN32_FIND_DATAW find_w;
static WIN32_FIND_DATAA find_a;

static void find_take(os_find *f)
{
    if (win_wide) {
        f->attrs = find_w.dwFileAttributes;
        n_copy(f->name, (const nchar *)find_w.cFileName, NOTE_PATH_MAX);
    } else {
        f->attrs = find_a.dwFileAttributes;
        wide_of(find_a.cFileName, f->name, NOTE_PATH_MAX);
    }
}

int os_find_open(const nchar *pattern, os_find *f)
{
    char a[ACP_MAX];

    f->handle = INVALID_HANDLE_VALUE;
    f->attrs  = 0;
    f->name[0] = 0;

    if (win_wide) {
        f->handle = FindFirstFileW((LPCWSTR)pattern, &find_w);
    } else {
        /* A pattern that will not fit the codepage cannot match anything on a
         * volume whose names came from it, so an empty walk is the answer. */
        if (!acp_path(pattern, a, (int)sizeof a)) return 0;
        f->handle = FindFirstFileA(a, &find_a);
    }
    if (f->handle == INVALID_HANDLE_VALUE) return 0;
    find_take(f);
    return 1;
}

int os_find_step(os_find *f)
{
    BOOL more = win_wide ? FindNextFileW(f->handle, &find_w)
                         : FindNextFileA(f->handle, &find_a);
    if (!more) return 0;
    find_take(f);
    return 1;
}

void os_find_close(os_find *f)
{
    if (f->handle != INVALID_HANDLE_VALUE) FindClose(f->handle);
    f->handle = INVALID_HANDLE_VALUE;
}

int os_time_date(const SYSTEMTIME *st, nchar *dst, int cap)
{
    int n;

    if (win_wide) {
        n = GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, st, NULL,
                           (LPWSTR)dst, cap);
        if (n <= 0) return 0;
        dst[n - 1] = (nchar)' ';
        return GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, st, NULL,
                              (LPWSTR)(dst + n), cap - n) > 0;
    } else {
        char a[128];
        n = GetTimeFormatA(LOCALE_USER_DEFAULT, TIME_NOSECONDS, st, NULL,
                           a, (int)sizeof a);
        if (n <= 0) return 0;
        a[n - 1] = ' ';
        if (GetDateFormatA(LOCALE_USER_DEFAULT, DATE_SHORTDATE, st, NULL,
                           a + n, (int)sizeof a - n) <= 0)
            return 0;
        wide_of(a, dst, cap);
        return 1;
    }
}

HRSRC os_find_resource(HINSTANCE inst, int id, int type)
{
    if (win_wide)
        return FindResourceW(inst, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(type));
    return FindResourceA(inst, MAKEINTRESOURCEA(id), MAKEINTRESOURCEA(type));
}

int os_run(nchar *line, const nchar *dir)
{
    STARTUPINFOW  siw;
    STARTUPINFOA  sia;
    PROCESS_INFORMATION pi;
    char al[ACP_MAX], ad[ACP_MAX];
    BOOL ok;

    memset(&pi, 0, sizeof(pi));

    if (win_wide) {
        memset(&siw, 0, sizeof(siw));
        siw.cb = sizeof(siw);
        ok = CreateProcessW(NULL, (LPWSTR)line, NULL, NULL, FALSE,
                            CREATE_NEW_CONSOLE, NULL,
                            (dir && dir[0]) ? (LPCWSTR)dir : NULL, &siw, &pi);
    } else {
        /* The command line is what the user typed, so it is text and not a
         * path: a character the codepage has no room for becomes a visible
         * mark and the shell says it cannot find that command, which is a
         * better answer than refusing to try. */
        acp_text(line, al, (int)sizeof al);
        if (dir && dir[0] && !acp_path(dir, ad, (int)sizeof ad)) ad[0] = 0;
        else if (!dir) ad[0] = 0;
        memset(&sia, 0, sizeof(sia));
        sia.cb = sizeof(sia);
        ok = CreateProcessA(NULL, al, NULL, NULL, FALSE,
                            CREATE_NEW_CONSOLE, NULL,
                            ad[0] ? ad : NULL, &sia, &pi);
    }

    if (!ok) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* RegGetValue would be the short way to do this and arrived with Windows
 * Vista in both halves of the API, so there is nothing to fall back to; open
 * and query have been there since the beginning. */
int os_reg_dword(HKEY root, const nchar *sub, const nchar *value, DWORD *out)
{
    HKEY  k;
    DWORD type = 0, cb = (DWORD)sizeof(DWORD);
    LONG  rc;

    if (win_wide) {
        if (RegOpenKeyExW(root, (LPCWSTR)sub, 0, KEY_QUERY_VALUE, &k)
            != ERROR_SUCCESS)
            return 0;
        rc = RegQueryValueExW(k, (LPCWSTR)value, NULL, &type,
                              (BYTE *)out, &cb);
    } else {
        char as[ACP_MAX], av[ACP_MAX];
        if (RegOpenKeyExA(root, acp_text(sub, as, (int)sizeof as), 0,
                          KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
            return 0;
        rc = RegQueryValueExA(k, acp_text(value, av, (int)sizeof av), NULL,
                              &type, (BYTE *)out, &cb);
    }

    RegCloseKey(k);
    return (rc == ERROR_SUCCESS && type == REG_DWORD) ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * user32
 *
 * Which half of the API a window belongs to is decided once, by the call that
 * registers its class, and everything the window then does has to agree:
 * DefWindowProc, the message loop and the subclass hook all come in W and A
 * forms and a window that mixes them gets its text translated twice.  That is
 * why the wrappers below cover calls that carry no text of their own.
 * ------------------------------------------------------------------------- */

ATOM os_register_class(WNDCLASSEXW *wc)
{
    /* Class styles that arrived after this Windows did.  RegisterClassEx
     * does not ignore a bit it does not know -- it fails, and a caller
     * that does not check is left with a class that never existed and a
     * window that can never be created.  That is how CS_DROPSHADOW cost
     * the command palette both of its shortcuts on Windows 95 while the
     * menus and accelerators around it worked perfectly.
     *
     * The shadow is what the style buys, and a palette without one is
     * simply the flat card it was before Windows XP. */
    if (!win_wide) wc->style &= ~(UINT)0x00020000;  /* CS_DROPSHADOW, XP */

    WNDCLASSEXA a;
    char cls[128];

    if (win_wide) return RegisterClassExW(wc);

    memset(&a, 0, sizeof(a));
    a.cbSize        = sizeof(a);
    a.style         = wc->style;
    a.lpfnWndProc   = wc->lpfnWndProc;
    a.cbClsExtra    = wc->cbClsExtra;
    a.cbWndExtra    = wc->cbWndExtra;
    a.hInstance     = wc->hInstance;
    a.hIcon         = wc->hIcon;
    a.hCursor       = wc->hCursor;
    a.hbrBackground = wc->hbrBackground;
    a.hIconSm       = wc->hIconSm;
    /* Every class note registers is named by an ASCII literal of its own, and
     * none of them has a menu. */
    a.lpszClassName = acp_text((const nchar *)wc->lpszClassName, cls,
                               (int)sizeof cls);
    return RegisterClassExA(&a);
}

HWND os_create_window(DWORD ex, const nchar *cls, const nchar *title,
                      DWORD style, int x, int y, int w, int h,
                      HWND parent, HMENU menu, HINSTANCE inst, void *param)
{
    char acls[128], atitle[ACP_MAX];

    if (win_wide)
        return CreateWindowExW(ex, (LPCWSTR)cls, (LPCWSTR)title, style,
                               x, y, w, h, parent, menu, inst, param);

    /* Extended styles that arrived after this Windows did.  A bit it does not
     * know is not ignored -- creation fails, and a caller that checks the
     * handle and gives up quietly looks exactly like a feature that does
     * nothing: the command palette opened by neither of its two shortcuts,
     * with the menus and the accelerators around it working perfectly.
     *
     * Dropping WS_EX_NOACTIVATE costs the palette nothing that matters.  It
     * asks for it so that showing itself does not take focus from the editor
     * -- and it is shown with SW_SHOWNOACTIVATE and never given focus, which
     * is how the same thing was done before the style existed. */
    ex &= ~(DWORD)0x08000000;   /* WS_EX_NOACTIVATE, Windows 2000 */

    return CreateWindowExA(ex, acp_text(cls, acls, (int)sizeof acls),
                           acp_text(title, atitle, (int)sizeof atitle), style,
                           x, y, w, h, parent, menu, inst, param);
}

LRESULT os_defproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return win_wide ? DefWindowProcW(wnd, msg, wp, lp)
                    : DefWindowProcA(wnd, msg, wp, lp);
}

LRESULT os_send(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return win_wide ? SendMessageW(wnd, msg, wp, lp)
                    : SendMessageA(wnd, msg, wp, lp);
}

BOOL os_post(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return win_wide ? PostMessageW(wnd, msg, wp, lp)
                    : PostMessageA(wnd, msg, wp, lp);
}

BOOL os_get_message(MSG *msg)
{
    return win_wide ? GetMessageW(msg, NULL, 0, 0)
                    : GetMessageA(msg, NULL, 0, 0);
}

LRESULT os_dispatch(MSG *msg)
{
    return win_wide ? DispatchMessageW(msg) : DispatchMessageA(msg);
}

BOOL os_is_dialog_message(HWND dlg, MSG *msg)
{
    return win_wide ? IsDialogMessageW(dlg, msg) : IsDialogMessageA(dlg, msg);
}

int os_translate_accel(HWND wnd, HACCEL accel, MSG *msg)
{
    return win_wide ? TranslateAcceleratorW(wnd, accel, msg)
                    : TranslateAcceleratorA(wnd, accel, msg);
}

HACCEL os_accel_table(ACCEL *a, int n)
{
    return win_wide ? CreateAcceleratorTableW(a, n)
                    : CreateAcceleratorTableA(a, n);
}

WNDPROC os_set_wndproc(HWND wnd, WNDPROC proc)
{
    if (win_wide)
        return (WNDPROC)SetWindowLongPtrW(wnd, GWLP_WNDPROC, (LONG_PTR)proc);
    return (WNDPROC)SetWindowLongPtrA(wnd, GWLP_WNDPROC, (LONG_PTR)proc);
}

/* The other half of os_set_wndproc, and it has to agree with it: the procedure
 * that was displaced belongs to whichever form installed the subclass, and
 * calling the W form into an A one hands the old procedure a WM_CHAR it will
 * read as a codepage byte and translate a second time. */
LRESULT os_call_wndproc(WNDPROC proc, HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return win_wide ? CallWindowProcW(proc, wnd, msg, wp, lp)
                    : CallWindowProcA(proc, wnd, msg, wp, lp);
}

void os_set_window_text(HWND wnd, const nchar *s)
{
    char a[ACP_MAX];
    if (win_wide) SetWindowTextW(wnd, (LPCWSTR)s);
    else          SetWindowTextA(wnd, acp_text(s, a, (int)sizeof a));
}

int os_message_box(HWND owner, const nchar *text, const nchar *title, UINT flags)
{
    char at[ACP_MAX], ai[128];
    if (win_wide) return MessageBoxW(owner, (LPCWSTR)text, (LPCWSTR)title, flags);
    return MessageBoxA(owner, acp_text(text, at, (int)sizeof at),
                       acp_text(title, ai, (int)sizeof ai), flags);
}

/* A send that cannot wedge this process behind a wedged one.  Only the
 * hand-off uses it: everything else here talks to windows this thread owns,
 * where a send is a call.  Returns 0 if the other side never answered, and
 * the caller then does the work itself. */
LRESULT os_send_timeout(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT ms)
{
    DWORD_PTR r = 0;
    if (win_wide) {
        if (!SendMessageTimeoutW(wnd, msg, wp, lp,
                                 SMTO_ABORTIFHUNG, ms, &r)) return 0;
    } else {
        if (!SendMessageTimeoutA(wnd, msg, wp, lp,
                                 SMTO_ABORTIFHUNG, ms, &r)) return 0;
    }
    return (LRESULT)r;
}

HWND os_find_window(const nchar *cls)
{
    char a[128];
    if (win_wide) return FindWindowW((LPCWSTR)cls, NULL);
    return FindWindowA(acp_text(cls, a, (int)sizeof a), NULL);
}

UINT os_register_message(const nchar *name)
{
    char a[128];
    if (win_wide) return RegisterWindowMessageW((LPCWSTR)name);
    return RegisterWindowMessageA(acp_text(name, a, (int)sizeof a));
}

HICON os_icon(HINSTANCE inst, int id)
{
    if (win_wide) return LoadIconW(inst, MAKEINTRESOURCEW(id));
    return LoadIconA(inst, MAKEINTRESOURCEA(id));
}

HCURSOR os_cursor(int id)
{
    /* The number rather than the IDC_ macro, because the macro carries the W
     * form of the cast with it and only half of this function wants that. */
    if (win_wide) return LoadCursorW(NULL, MAKEINTRESOURCEW(id));
    return LoadCursorA(NULL, MAKEINTRESOURCEA(id));
}

HCURSOR os_arrow_cursor(void)
{
    return os_cursor(OS_CURSOR_ARROW);
}

HICON os_class_icon(HWND wnd, int which)
{
    if (win_wide) return (HICON)GetClassLongPtrW(wnd, which);
    return (HICON)GetClassLongPtrA(wnd, which);
}

BOOL os_append_menu(HMENU m, UINT flags, UINT_PTR id, const void *data)
{
    /* MF_OWNERDRAW, always: the last argument is note's own item data and not
     * a string, so there is nothing here to convert -- but the menu still
     * belongs to whichever half of the API created it. */
    if (win_wide) return AppendMenuW(m, flags, id, (LPCWSTR)data);
    return AppendMenuA(m, flags, id, (LPCSTR)data);
}

ULONG_PTR os_menu_item_data(HMENU m, UINT pos, UINT *state)
{
    UINT mask = MIIM_DATA | (state ? MIIM_STATE : 0);

    if (state) *state = 0;

    if (win_wide) {
        MENUITEMINFOW mii;
        memset(&mii, 0, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask  = mask;
        if (!GetMenuItemInfoW(m, pos, TRUE, &mii)) return 0;
        if (state) *state = mii.fState;
        return mii.dwItemData;
    } else {
        MENUITEMINFOA mii;
        memset(&mii, 0, sizeof(mii));
        /* The size the structure was before hbmpItem was added to the end of
         * it.  Windows 95 checks cbSize against the shape it knows and refuses
         * anything else outright, and every Windows since accepts the older
         * size -- so the shorter one is the only value both will take.  Nothing
         * read here lives past dwItemData anyway. */
        mii.cbSize = FIELD_OFFSET(MENUITEMINFOA, hbmpItem);
        mii.fMask  = mask;
        if (!GetMenuItemInfoA(m, pos, TRUE, &mii)) return 0;
        if (state) *state = mii.fState;
        return mii.dwItemData;
    }
}

int os_draw_text(HDC dc, const nchar *s, int len, RECT *rc, UINT fmt)
{
    char a[ACP_MAX];
    int  n;
    if (win_wide) return DrawTextW(dc, (LPCWSTR)s, len, rc, fmt);
    n = acp_run(s, len, a, (int)sizeof a);
    return DrawTextA(dc, a, n, rc, fmt);
}

unsigned os_wm_char(unsigned wp)
{
    char  a[2];
    nchar w[4];

    if (win_wide) return wp;

    /* On an ANSI window this is a codepage byte.  A lead byte of a double-byte
     * pair arrives on its own and is dropped rather than half-decoded: note's
     * query lines are Latin, and half a character is worse than none. */
    a[0] = (char)(wp & 0xFF);
    a[1] = 0;
    wide_of(a, w, 4);
    return (unsigned)w[0];
}

/* How many rows one detent of the wheel scrolls, or 0 when this Windows will
 * not say -- which is the answer on a Windows 95 that never met a wheel, since
 * the setting arrived with the mouse that had one. */
UINT os_wheel_lines(void)
{
    UINT lines = 0;
    BOOL ok = win_wide
        ? SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0)
        : SystemParametersInfoA(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    return ok ? lines : 0;
}

/* NONCLIENTMETRICS gained iPaddedBorderWidth in Windows Vista, and
 * SystemParametersInfo refuses a size it does not recognise rather than
 * ignoring the tail.  Asking with the modern size therefore fails outright on
 * anything older, and the caller falls back to a stock font -- which on
 * Windows 95 is the chunky bitmap System face, not the MS Sans Serif the
 * system actually asked for.  Nothing reports an error; the menus and the tab
 * strip simply come out in the wrong typeface, larger than every other window
 * on the desktop.
 *
 * So: ask with the real size, and on refusal ask again without the field that
 * postdates the machine.  Both sizes are tried on both paths because the
 * question is which Windows this is, not which character set it speaks.
 *
 * This is the fourth structure whose size had to be walked back for Windows
 * 95, after OPENFILENAME, MENUITEMINFO and the CS_DROPSHADOW class style.
 * None of the four is visible to the compiler or to dumpbin. */
#define NCM_PRE_VISTA(t) ((UINT)(sizeof(t) - sizeof(int)))

HFONT os_menu_font(void)
{
    if (win_wide) {
        NONCLIENTMETRICSW ncm;
        memset(&ncm, 0, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            return CreateFontIndirectW(&ncm.lfMenuFont);

        memset(&ncm, 0, sizeof(ncm));
        ncm.cbSize = NCM_PRE_VISTA(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0))
            return CreateFontIndirectW(&ncm.lfMenuFont);
    } else {
        NONCLIENTMETRICSA ncm;
        memset(&ncm, 0, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            return CreateFontIndirectA(&ncm.lfMenuFont);

        memset(&ncm, 0, sizeof(ncm));
        ncm.cbSize = NCM_PRE_VISTA(ncm);
        if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0))
            return CreateFontIndirectA(&ncm.lfMenuFont);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * gdi32
 *
 * Only the calls that name a font or a document come through here.  Drawing
 * and measuring a run of text does not: TextOutW, ExtTextOutW,
 * GetTextExtentPoint32W and GetCharWidthW are implemented for real everywhere
 * note runs -- see note_win32.h -- so the pixels stay Unicode even where the
 * file names do not.  The font itself is a different matter, which is why
 * CreateFontIndirect and GetTextMetrics are here and TextOut is not.
 * ------------------------------------------------------------------------- */

static void logfont_to_a(const LOGFONTW *w, LOGFONTA *a)
{
    memset(a, 0, sizeof(*a));
    a->lfHeight         = w->lfHeight;
    a->lfWidth          = w->lfWidth;
    a->lfEscapement     = w->lfEscapement;
    a->lfOrientation    = w->lfOrientation;
    a->lfWeight         = w->lfWeight;
    a->lfItalic         = w->lfItalic;
    a->lfUnderline      = w->lfUnderline;
    a->lfStrikeOut      = w->lfStrikeOut;
    a->lfCharSet        = w->lfCharSet;
    a->lfOutPrecision   = w->lfOutPrecision;
    a->lfClipPrecision  = w->lfClipPrecision;
    a->lfQuality        = w->lfQuality;
    a->lfPitchAndFamily = w->lfPitchAndFamily;
    acp_text((const nchar *)w->lfFaceName, a->lfFaceName, LF_FACESIZE);
}

static void logfont_to_w(const LOGFONTA *a, LOGFONTW *w)
{
    nchar face[LF_FACESIZE];
    memset(w, 0, sizeof(*w));
    w->lfHeight         = a->lfHeight;
    w->lfWidth          = a->lfWidth;
    w->lfEscapement     = a->lfEscapement;
    w->lfOrientation    = a->lfOrientation;
    w->lfWeight         = a->lfWeight;
    w->lfItalic         = a->lfItalic;
    w->lfUnderline      = a->lfUnderline;
    w->lfStrikeOut      = a->lfStrikeOut;
    w->lfCharSet        = a->lfCharSet;
    w->lfOutPrecision   = a->lfOutPrecision;
    w->lfClipPrecision  = a->lfClipPrecision;
    w->lfQuality        = a->lfQuality;
    w->lfPitchAndFamily = a->lfPitchAndFamily;
    n_copy((nchar *)w->lfFaceName, wide_of(a->lfFaceName, face, LF_FACESIZE),
           LF_FACESIZE);
}

HFONT os_font(const LOGFONTW *lf)
{
    LOGFONTA a;
    if (win_wide) return CreateFontIndirectW(lf);
    logfont_to_a(lf, &a);
    return CreateFontIndirectA(&a);
}

void os_text_metrics(HDC dc, TEXTMETRICW *tm)
{
    TEXTMETRICA a;
    nchar c[4];

    if (win_wide) { GetTextMetricsW(dc, tm); return; }

    memset(tm, 0, sizeof(*tm));
    if (!GetTextMetricsA(dc, &a)) return;
    tm->tmHeight           = a.tmHeight;
    tm->tmAscent           = a.tmAscent;
    tm->tmDescent          = a.tmDescent;
    tm->tmInternalLeading  = a.tmInternalLeading;
    tm->tmExternalLeading  = a.tmExternalLeading;
    tm->tmAveCharWidth     = a.tmAveCharWidth;
    tm->tmMaxCharWidth     = a.tmMaxCharWidth;
    tm->tmWeight           = a.tmWeight;
    tm->tmOverhang         = a.tmOverhang;
    tm->tmDigitizedAspectX = a.tmDigitizedAspectX;
    tm->tmDigitizedAspectY = a.tmDigitizedAspectY;
    tm->tmItalic           = a.tmItalic;
    tm->tmUnderlined       = a.tmUnderlined;
    tm->tmStruckOut        = a.tmStruckOut;
    tm->tmPitchAndFamily   = a.tmPitchAndFamily;
    tm->tmCharSet          = a.tmCharSet;

    { char b[2]; b[0] = (char)a.tmFirstChar; b[1] = 0;
      tm->tmFirstChar = wide_of(b, c, 4)[0]; }
    { char b[2]; b[0] = (char)a.tmLastChar;  b[1] = 0;
      tm->tmLastChar  = wide_of(b, c, 4)[0]; }
    { char b[2]; b[0] = (char)a.tmDefaultChar; b[1] = 0;
      tm->tmDefaultChar = wide_of(b, c, 4)[0]; }
    { char b[2]; b[0] = (char)a.tmBreakChar; b[1] = 0;
      tm->tmBreakChar = wide_of(b, c, 4)[0]; }
}

/* The callback GDI hands a face name to.  A file-scope pointer rather than the
 * LPARAM, because the LPARAM is the only channel and both trampolines want it
 * for the same thing; the enumeration is synchronous and the UI is one thread,
 * so there is never a second walk in flight. */
static os_font_fn enum_fn;

static int CALLBACK enum_face_w(const LOGFONTW *lf, const TEXTMETRICW *tm,
                                DWORD type, LPARAM lp)
{
    (void)type; (void)lp;
    return enum_fn((const nchar *)lf->lfFaceName, tm->tmPitchAndFamily);
}

static int CALLBACK enum_face_a(const LOGFONTA *lf, const TEXTMETRICA *tm,
                                DWORD type, LPARAM lp)
{
    nchar face[LF_FACESIZE];
    (void)type; (void)lp;
    return enum_fn(wide_of(lf->lfFaceName, face, LF_FACESIZE),
                   tm->tmPitchAndFamily);
}

void os_enum_fonts(HDC dc, os_font_fn fn)
{
    enum_fn = fn;
    if (win_wide) {
        LOGFONTW want;
        memset(&want, 0, sizeof(want));
        want.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(dc, &want, enum_face_w, 0, 0);
    } else {
        LOGFONTA want;
        memset(&want, 0, sizeof(want));
        want.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExA(dc, &want, enum_face_a, 0, 0);
    }
    enum_fn = 0;
}

int os_start_doc(HDC dc, const nchar *name)
{
    if (win_wide) {
        DOCINFOW di;
        memset(&di, 0, sizeof(di));
        di.cbSize = sizeof(di);
        di.lpszDocName = (LPCWSTR)name;
        return StartDocW(dc, &di);
    } else {
        DOCINFOA di;
        char a[ACP_MAX];
        memset(&di, 0, sizeof(di));
        di.cbSize = sizeof(di);
        di.lpszDocName = acp_text(name, a, (int)sizeof a);
        return StartDocA(dc, &di);
    }
}

/* -------------------------------------------------------------------------
 * comdlg32
 *
 * Each of these converts the fields note actually sets and reads back, not the
 * whole structure: the rest are templates, hooks and instance handles that are
 * null on both sides, and pretending to convert them would only hide which
 * ones matter.
 * ------------------------------------------------------------------------- */

/* A filter is a run of NUL-separated strings closed by an empty one, so it
 * cannot go through acp_text as a single string. */
static void acp_filter(const nchar *s, char *dst, int cap)
{
    int used = 0;

    dst[0] = 0;
    while (s && *s && used < cap - 1) {
        int n = WideCharToMultiByte(CP_ACP, acp_flags, (LPCWSTR)s, -1,
                                    dst + used, cap - used - 1, NULL, NULL);
        if (n <= 0) break;
        used += n;
        s    += n_len(s) + 1;
    }
    dst[used] = 0;
}

int os_choose_file(OPENFILENAMEW *ofn, int save)
{
    OPENFILENAMEA a;
    char filter[512], file[ACP_MAX], ext[16];
    int  ok;

    if (win_wide)
        return (save ? GetSaveFileNameW(ofn) : GetOpenFileNameW(ofn)) ? 1 : 0;

    memset(&a, 0, sizeof(a));
    /* Not sizeof: three fields were appended to OPENFILENAME after Windows 95,
     * and its comdlg32 answers CDERR_STRUCTSIZE to a size it does not
     * recognise rather than ignoring the tail -- so a dialog built with the
     * current header never opens there.  The SDK keeps the older size under a
     * name for exactly this. */
    a.lStructSize = OPENFILENAME_SIZE_VERSION_400A;
    a.hwndOwner   = ofn->hwndOwner;
    a.Flags       = ofn->Flags;
    acp_filter((const nchar *)ofn->lpstrFilter, filter, (int)sizeof filter);
    a.lpstrFilter = filter;
    /* Save is given a name to start from; open is given an empty buffer. */
    acp_text((const nchar *)ofn->lpstrFile, file, (int)sizeof file);
    a.lpstrFile = file;
    a.nMaxFile  = (DWORD)sizeof file;
    if (ofn->lpstrDefExt) {
        acp_text((const nchar *)ofn->lpstrDefExt, ext, (int)sizeof ext);
        a.lpstrDefExt = ext;
    }

    ok = (save ? GetSaveFileNameA(&a) : GetOpenFileNameA(&a)) ? 1 : 0;
    if (ok) wide_of(file, (nchar *)ofn->lpstrFile, (int)ofn->nMaxFile);
    return ok;
}

int os_choose_font(CHOOSEFONTW *cf)
{
    CHOOSEFONTA a;
    LOGFONTA    lf;
    int         ok;

    if (win_wide) return ChooseFontW(cf) ? 1 : 0;

    logfont_to_a(cf->lpLogFont, &lf);
    memset(&a, 0, sizeof(a));
    a.lStructSize = sizeof(a);
    a.hwndOwner   = cf->hwndOwner;
    a.lpLogFont   = &lf;
    a.Flags       = cf->Flags;

    ok = ChooseFontA(&a) ? 1 : 0;
    if (ok) {
        logfont_to_w(&lf, cf->lpLogFont);
        cf->iPointSize = a.iPointSize;
    }
    return ok;
}

/* The find dialog is modeless, so its A-side buffers outlive the call that put
 * it up and every message it sends back points at them.  os_find_msg converts
 * the other way at the one place that reads them. */
static FINDREPLACEA fr_a;
static char         fr_a_find[NOTE_FIND_MAX * 2 + 2];
static char         fr_a_repl[NOTE_FIND_MAX * 2 + 2];
static FINDREPLACEW fr_w;
static nchar        fr_w_find[NOTE_FIND_MAX];
static nchar        fr_w_repl[NOTE_FIND_MAX];

HWND os_find_dlg(FINDREPLACEW *fr, int replace)
{
    if (win_wide) return replace ? ReplaceTextW(fr) : FindTextW(fr);

    memset(&fr_a, 0, sizeof(fr_a));
    fr_a.lStructSize      = sizeof(fr_a);
    fr_a.hwndOwner        = fr->hwndOwner;
    fr_a.Flags            = fr->Flags;
    fr_a.lpstrFindWhat    = fr_a_find;
    fr_a.wFindWhatLen     = (WORD)sizeof fr_a_find;
    fr_a.lpstrReplaceWith = fr_a_repl;
    fr_a.wReplaceWithLen  = (WORD)sizeof fr_a_repl;
    acp_text((const nchar *)fr->lpstrFindWhat,    fr_a_find, (int)sizeof fr_a_find);
    acp_text((const nchar *)fr->lpstrReplaceWith, fr_a_repl, (int)sizeof fr_a_repl);

    return replace ? ReplaceTextA(&fr_a) : FindTextA(&fr_a);
}

FINDREPLACEW *os_find_msg(void *lp)
{
    if (win_wide) return (FINDREPLACEW *)lp;

    memset(&fr_w, 0, sizeof(fr_w));
    fr_w.lStructSize      = sizeof(fr_w);
    fr_w.hwndOwner        = fr_a.hwndOwner;
    fr_w.Flags            = fr_a.Flags;
    fr_w.lpstrFindWhat    = (LPWSTR)wide_of(fr_a_find, fr_w_find, NOTE_FIND_MAX);
    fr_w.wFindWhatLen     = NOTE_FIND_MAX;
    fr_w.lpstrReplaceWith = (LPWSTR)wide_of(fr_a_repl, fr_w_repl, NOTE_FIND_MAX);
    fr_w.wReplaceWithLen  = NOTE_FIND_MAX;
    return &fr_w;
}

int os_page_setup(PAGESETUPDLGW *ps)
{
    PAGESETUPDLGA a;
    int ok;

    if (win_wide) return PageSetupDlgW(ps) ? 1 : 0;

    memset(&a, 0, sizeof(a));
    a.lStructSize = sizeof(a);
    a.hwndOwner   = ps->hwndOwner;
    a.Flags       = ps->Flags;
    a.hDevMode    = ps->hDevMode;
    a.hDevNames   = ps->hDevNames;
    a.ptPaperSize = ps->ptPaperSize;
    a.rtMargin    = ps->rtMargin;
    a.rtMinMargin = ps->rtMinMargin;

    ok = PageSetupDlgA(&a) ? 1 : 0;
    /* The margins are what the next print reads, and the device handles are
     * what the next dialog opens on. */
    ps->Flags       = a.Flags;
    ps->hDevMode    = a.hDevMode;
    ps->hDevNames   = a.hDevNames;
    ps->ptPaperSize = a.ptPaperSize;
    ps->rtMargin    = a.rtMargin;
    return ok;
}

int os_print_dlg(PRINTDLGW *pd)
{
    PRINTDLGA a;
    int ok;

    if (win_wide) return PrintDlgW(pd) ? 1 : 0;

    memset(&a, 0, sizeof(a));
    a.lStructSize = sizeof(a);
    a.hwndOwner   = pd->hwndOwner;
    a.Flags       = pd->Flags;
    a.nCopies     = pd->nCopies;

    ok = PrintDlgA(&a) ? 1 : 0;
    pd->Flags     = a.Flags;
    pd->hDC       = a.hDC;
    pd->hDevMode  = a.hDevMode;
    pd->hDevNames = a.hDevNames;
    return ok;
}

/* -------------------------------------------------------------------------
 * shell32
 * ------------------------------------------------------------------------- */

UINT os_drag_query(HDROP drop, UINT i, nchar *dst, int cap)
{
    char a[ACP_MAX];
    UINT n;

    /* The count, which is the same question in either half. */
    if (!dst)
        return win_wide ? DragQueryFileW(drop, i, NULL, 0)
                        : DragQueryFileA(drop, i, NULL, 0);

    if (win_wide) return DragQueryFileW(drop, i, (LPWSTR)dst, (UINT)cap);

    n = DragQueryFileA(drop, i, a, (UINT)sizeof a);
    if (!n) { dst[0] = 0; return 0; }
    wide_of(a, dst, cap);
    return (UINT)n_len(dst);
}
