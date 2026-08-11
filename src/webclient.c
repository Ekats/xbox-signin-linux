/*
 * webclient.c -- drop-in replacement for AoE2DE's WebClient.exe
 *
 * WebClient.exe is Microsoft's XAL "TestWebClient": a small WPF program the
 * game spawns to run the Xbox Live OAuth flow. The game passes it three
 * arguments, reads its stdout over a pipe, and expects either the redirect URL
 * or the literal string USER_CANCEL.
 *
 *   WebClient.exe <url> <targetUrl> <showUrlType>
 *
 * It fails under Wine/Proton for two independent reasons:
 *
 *   1. It hosts System.Windows.Controls.WebBrowser and watches the
 *      WebBrowser.Navigating event for the redirect. wine-mono's WPF never
 *      raises that event, so the target is never detected -- sign-in cannot
 *      complete even when the page renders correctly.
 *
 *   2. Wine's Gecko (2.47.4, Firefox 60 era) cannot render Microsoft's
 *      current login SPA. The page loads and then blanks.
 *
 * Neither is fixable from config, so this replacement hands the sign-in to the
 * real desktop browser. It runs inside the prefix and talks to xal-helper.py
 * on the host through two files placed next to this executable:
 *
 *   xal-request.txt   written here:   line 1 = auth url, line 2 = target url
 *   xal-result.txt    written there:  the landing URL, or USER_CANCEL
 *
 * Paths are derived from GetModuleFileName, so nothing is hardcoded.
 *
 * The stdout contract below is taken from the original's IL, not guessed:
 *
 *   target reached  -> the landing URL, bare, NO trailing newline, exit 0
 *   cancelled       -> "USER_CANCEL", bare
 *   bad arg count   -> usage line on stderr, exit 1
 *
 * The original used Console.Out.Write (not WriteLine); emitting a trailing
 * newline here breaks the game's parsing, so don't "fix" that.
 *
 * Build:  make
 */

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <string.h>
#include <wchar.h>

#define POLL_MS   300
#define ID_CANCEL 1001
#define BUFSZ     8192

static char g_request[MAX_PATH];
static char g_result[MAX_PATH];
static char g_gamedir[MAX_PATH];
static HWND g_wnd;

static void write_handle(DWORD which, const char *s)
{
    HANDLE h = GetStdHandle(which);
    DWORD n = 0;

    if (h && h != INVALID_HANDLE_VALUE) {
        WriteFile(h, s, (DWORD)strlen(s), &n, NULL);
        FlushFileBuffers(h);
    }
}

/* Emit the token the game reads, clean up, and leave. */
static void emit_and_exit(const char *s)
{
    DeleteFileA(g_request);
    DeleteFileA(g_result);
    write_handle(STD_OUTPUT_HANDLE, s);
    ExitProcess(0);
}

/* Build "<directory of this exe>\<name>". */
static void beside_exe(char *out, size_t n, const char *name)
{
    char path[MAX_PATH];
    char *slash;

    GetModuleFileNameA(NULL, path, sizeof(path));
    slash = strrchr(path, '\\');
    if (slash)
        slash[1] = 0;
    lstrcpynA(out, path, (int)n);
    lstrcatA(out, name);
}

static void write_request(const char *url, const char *target)
{
    HANDLE h = CreateFileA(g_request, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n;

    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, url, (DWORD)strlen(url), &n, NULL);
    WriteFile(h, "\n", 1, &n, NULL);
    WriteFile(h, target, (DWORD)strlen(target), &n, NULL);
    WriteFile(h, "\n", 1, &n, NULL);
    CloseHandle(h);
}

typedef char *(CDECL *wine_get_unix_file_name_t)(const WCHAR *);

/*
 * This executable's directory as a *Unix* path, which is what is needed to
 * hand anything to the host side.
 *
 * Wine exports wine_get_unix_file_name() from kernel32 for exactly this, and
 * it honours whatever drive mapping the prefix actually uses. The fallback
 * covers the ordinary case where Z: is mapped to /.
 */
static int unix_dir_of_exe(char *out, size_t n)
{
    WCHAR wpath[MAX_PATH];
    char apath[MAX_PATH];
    wine_get_unix_file_name_t to_unix;
    WCHAR *wslash;
    char *aslash, *p;

    GetModuleFileNameW(NULL, wpath, MAX_PATH);
    wslash = wcsrchr(wpath, '\\');
    if (wslash)
        *wslash = 0;

    to_unix = (wine_get_unix_file_name_t)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name");
    if (to_unix) {
        char *unix_path = to_unix(wpath);

        if (unix_path) {
            lstrcpynA(out, unix_path, (int)n);
            HeapFree(GetProcessHeap(), 0, unix_path);
            return out[0] != 0;
        }
    }

    GetModuleFileNameA(NULL, apath, sizeof(apath));
    aslash = strrchr(apath, '\\');
    if (aslash)
        *aslash = 0;
    if ((apath[0] == 'Z' || apath[0] == 'z') && apath[1] == ':') {
        lstrcpynA(out, apath + 2, (int)n);
        for (p = out; *p; p++)
            if (*p == '\\')
                *p = '/';
        return out[0] != 0;
    }
    return 0;
}

/*
 * Start the host-side helper so the player does not have to.
 *
 * Wine can execute Unix programs through `start.exe /unix`, which is how this
 * reaches out of the prefix. The target is xal-launch.sh, which deals with
 * escaping Steam's container and with not starting a second helper.
 *
 * It is run through /bin/sh rather than directly so the script does not need
 * the executable bit -- copying these files into the game directory by hand
 * is enough, no installer required.
 *
 * Best effort throughout: if any of it fails, the window still explains how
 * to start the helper manually.
 */
static void try_start_helper(void)
{
    char cmd[MAX_PATH * 2];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    if (!unix_dir_of_exe(g_gamedir, sizeof(g_gamedir)))
        return;

    wsprintfA(cmd, "start.exe /unix /bin/sh \"%s/xal-launch.sh\"", g_gamedir);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

/* Non-zero once the helper has written a result. */
static int read_result(char *out, size_t n)
{
    HANDLE h = CreateFileA(g_result, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD got = 0;
    size_t len;

    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ReadFile(h, out, (DWORD)n - 1, &got, NULL);
    CloseHandle(h);
    out[got] = 0;

    len = strlen(out);
    while (len && (out[len - 1] == '\n' || out[len - 1] == '\r' ||
                   out[len - 1] == ' '))
        out[--len] = 0;
    return len > 0;
}

static LRESULT CALLBACK wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER: {
        char res[BUFSZ];

        if (read_result(res, sizeof(res)))
            emit_and_exit(res);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_CANCEL)
            emit_and_exit("USER_CANCEL");
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        emit_and_exit("USER_CANCEL");
        return 0;
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

static const char *TEXT_BODY =
    "Signing in to Xbox Live through your desktop browser.\r\n"
    "\r\n"
    "Wine's built-in browser engine cannot render Microsoft's login page, so "
    "the sign-in runs in your real browser instead.\r\n"
    "\r\n"
    "A browser window should open shortly. If none appears, the helper could "
    "not be started automatically -- run it in a terminal and it will pick "
    "this up:\r\n"
    "\r\n"
    "    python3 xal-helper.py\r\n"
    "\r\n"
    "This window closes by itself once you finish signing in.";

static void build_ui(void)
{
    WNDCLASSA wc;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "XalShimWnd";
    RegisterClassA(&wc);

    g_wnd = CreateWindowExA(0, "XalShimWnd", "Xbox Live sign-in",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                            CW_USEDEFAULT, CW_USEDEFAULT, 560, 340,
                            NULL, NULL, wc.hInstance, NULL);

    CreateWindowExA(0, "STATIC", TEXT_BODY, WS_CHILD | WS_VISIBLE,
                    16, 16, 520, 230, g_wnd, NULL, wc.hInstance, NULL);
    CreateWindowExA(0, "BUTTON", "Cancel",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    220, 258, 110, 30, g_wnd, (HMENU)ID_CANCEL,
                    wc.hInstance, NULL);

    ShowWindow(g_wnd, SW_SHOW);
    UpdateWindow(g_wnd);
}

int main(int argc, char **argv)
{
    MSG msg;

    /* argv[0] is the exe, so a valid call has four entries. The original
     * prints this exact sentence; keep it for familiarity in logs. */
    if (argc < 4) {
        write_handle(STD_ERROR_HANDLE,
                     "TestWebClient requires exactly 3 arguments: "
                     "url, targetUrl, and showUrlType\n");
        return 1;
    }

    beside_exe(g_request, sizeof(g_request), "xal-request.txt");
    beside_exe(g_result,  sizeof(g_result),  "xal-result.txt");

    /* Clear any stale result before advertising a new request, or we would
     * hand the game the previous stage's URL. */
    DeleteFileA(g_result);
    write_request(argv[1], argv[2]);
    try_start_helper();

    build_ui();
    SetTimer(g_wnd, 1, POLL_MS, NULL);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    emit_and_exit("USER_CANCEL");
    return 0;
}
