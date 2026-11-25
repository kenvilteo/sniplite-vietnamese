/* Upload a capture to the user's own server (server/worker.js in this repository).
 *
 * POST <upload_url>/upload, the PNG as the body, "Authorization: Bearer <upload_token>".
 * The answer {"id": "...", "url": "..."} gives the link: it is copied to the clipboard and
 * written, with the time, to uploads.log next to sniplite.ini. Both settings are read
 * from sniplite.ini, [sniplite], each time; nothing is sent while upload_url is empty.
 *
 * The PNG is written on the daemon's thread (image_save: WIC, like every other save),
 * the request runs on a thread of its own, and the outcome comes back to the daemon
 * window as WM_APP_UPLOAD_DONE. WinHTTP is loaded on demand, with the system proxy. */
#include "share.h"
#include "image.h"
#include "lang.h"
#include "ocr.h"
#include <math.h>
#include <winhttp.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

typedef HINTERNET(WINAPI *PFN_Open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET(WINAPI *PFN_Connect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET(WINAPI *PFN_OpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL(WINAPI *PFN_SetTimeouts)(HINTERNET, int, int, int, int);
typedef BOOL(WINAPI *PFN_Send)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL(WINAPI *PFN_Receive)(HINTERNET, LPVOID);
typedef BOOL(WINAPI *PFN_QueryHeaders)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL(WINAPI *PFN_ReadData)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL(WINAPI *PFN_Close)(HINTERNET);
typedef BOOL(WINAPI *PFN_Crack)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
typedef HINSTANCE(WINAPI *PFN_ShellExecuteW)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);

static struct {
    PFN_Open open;
    PFN_Connect connect;
    PFN_OpenRequest request;
    PFN_SetTimeouts timeouts;
    PFN_Send send;
    PFN_Receive receive;
    PFN_QueryHeaders headers;
    PFN_ReadData read;
    PFN_Crack crack;
    PFN_Close close; /* last: set once all the others are */
} WH;

#define URL_CAP 1024
#define TOKEN_CAP 512
#define BODY_MAX (32u << 20) /* the server takes 10 MB; a screenshot is far less */

typedef struct {
    HWND daemon;
    HWND win; /* the upload window, or NULL */
    POINT at; /* the mouse when the user asked: the notices show there */
    WCHAR path[MAX_PATH];
    WCHAR url[URL_CAP];
    WCHAR token[TOKEN_CAP];
} Job;

typedef struct {
    bool ok;
    HWND win;
    POINT at;
    WCHAR text[URL_CAP]; /* the link, or what failed */
} Done;

/* on the daemon's thread, before any upload thread starts */
static bool wh_load(void)
{
    if (WH.close)
        return true;
    HMODULE h = LoadLibraryExW(L"winhttp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h)
        return false;
#define GET(f, name)                                                   \
    if (!(WH.f = (__typeof__(WH.f))(void *)GetProcAddress(h, name))) \
        return false;
    GET(open, "WinHttpOpen");
    GET(connect, "WinHttpConnect");
    GET(request, "WinHttpOpenRequest");
    GET(timeouts, "WinHttpSetTimeouts");
    GET(send, "WinHttpSendRequest");
    GET(receive, "WinHttpReceiveResponse");
    GET(headers, "WinHttpQueryHeaders");
    GET(read, "WinHttpReadData");
    GET(crack, "WinHttpCrackUrl");
    GET(close, "WinHttpCloseHandle");
#undef GET
    return true;
}

static void cfg_get(const WCHAR *key, WCHAR *out, int cap)
{
    WCHAR ini[MAX_PATH];
    ini_path(ini, MAX_PATH);
    out[0] = 0;
    if (ini[0])
        GetPrivateProfileStringW(L"sniplite", key, L"", out, (DWORD)cap, ini);
}

/* upload_url without its trailing slashes, if it is an http(s) address; its length, or 0 */
static int base_url(WCHAR *out, int cap)
{
    cfg_get(L"upload_url", out, cap);
    int n = lstrlenW(out);
    while (n && (out[n - 1] == L'/' || out[n - 1] == L' '))
        out[--n] = 0;
    if (wcsncmp(out, L"https://", 8) && wcsncmp(out, L"http://", 7))
        return out[0] = 0;
    return n;
}

/* uploads.log next to sniplite.ini: "yyyy-mm-dd hh:mm:ss<TAB>link", UTF-8 */
static void log_append(const WCHAR *link)
{
    WCHAR path[MAX_PATH], line[URL_CAP + 32];
    ini_path(path, MAX_PATH);
    WCHAR *sl = path[0] ? wcsrchr(path, L'\\') : NULL;
    if (!sl)
        return;
    lstrcpyW(sl + 1, L"uploads.log"); /* shorter than sniplite.ini */
    SYSTEMTIME t;
    GetLocalTime(&t);
    wsprintfW(line, L"%04u-%02u-%02u %02u:%02u:%02u\t", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    int n = lstrlenW(line);
    lstrcpynW(line + n, link, URL_CAP);
    lstrcatW(line, L"\r\n");
    char utf8[(URL_CAP + 32) * 3];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8, NULL, NULL) - 1;
    if (bytes <= 0)
        return;
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD put;
    WriteFile(f, utf8, (DWORD)bytes, &put, NULL);
    CloseHandle(f);
}

/* the string value of "key" in a flat JSON object; false if absent or escaped */
static bool json_str(const char *s, const char *key, WCHAR *out, int cap)
{
    int k = lstrlenA(key);
    for (; *s; s++) {
        if (*s != '"' || memcmp(s + 1, key, (size_t)k) || s[k + 1] != '"')
            continue;
        const char *p = s + k + 2;
        while (*p == ' ' || *p == ':')
            p++;
        if (*p++ != '"')
            return false;
        const char *e = p;
        while (*e && *e != '"' && *e != '\\')
            e++;
        if (*e != '"' || e == p)
            return false;
        int n = MultiByteToWideChar(CP_UTF8, 0, p, (int)(e - p), out, cap - 1);
        out[n > 0 ? n : 0] = 0;
        return n > 0;
    }
    return false;
}

static bool send_png(const Job *j, const BYTE *body, DWORD size, WCHAR *out, int cap)
{
    WCHAR host[256], path[URL_CAP + 16];
    URL_COMPONENTS u;
    memset(&u, 0, sizeof u);
    u.dwStructSize = sizeof u;
    u.lpszHostName = host;
    u.dwHostNameLength = COUNTOF(host);
    u.lpszUrlPath = path;
    u.dwUrlPathLength = URL_CAP;
    if (!WH.crack(j->url, 0, 0, &u)) {
        lstrcpynW(out, j->url, cap);
        return false;
    }
    lstrcatW(path, L"/upload");
    bool ok = false;
    DWORD status = 0;
    HINTERNET s = WH.open(L"sniplite/" SNIPLITE_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) /* before Windows 8.1 */
        s = WH.open(L"sniplite/" SNIPLITE_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = NULL, r = NULL;
    if (s) {
        WH.timeouts(s, 8000, 8000, 30000, 30000);
        c = WH.connect(s, host, u.nPort, 0);
    }
    if (c)
        r = WH.request(c, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                       u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (r) {
        WCHAR hdr[TOKEN_CAP + 80];
        lstrcpyW(hdr, L"Content-Type: image/png\r\nAuthorization: Bearer ");
        lstrcatW(hdr, j->token);
        lstrcatW(hdr, L"\r\n");
        DWORD sz = sizeof status;
        if (WH.send(r, hdr, (DWORD)-1, (LPVOID)body, size, size, 0) && WH.receive(r, NULL) &&
            WH.headers(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                       &sz, WINHTTP_NO_HEADER_INDEX)) {
            char buf[4096];
            DWORD total = 0, got = 0;
            while (total < sizeof buf - 1 && WH.read(r, buf + total, sizeof buf - 1 - total, &got) && got)
                total += got;
            buf[total] = 0;
            ok = status == 200 && json_str(buf, "url", out, cap);
        }
    }
    if (!ok) {
        if (status)
            wsprintfW(out, L"HTTP %lu", status);
        else
            wsprintfW(out, L"%s (%lu)", host, GetLastError());
    }
    if (r)
        WH.close(r);
    if (c)
        WH.close(c);
    if (s)
        WH.close(s);
    return ok;
}

static DWORD WINAPI upload_thread(void *arg)
{
    Job *j = arg;
    BYTE *body = NULL;
    DWORD size = 0;
    HANDLE f = CreateFileW(j->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER n;
        if (GetFileSizeEx(f, &n) && n.QuadPart > 0 && n.QuadPart <= BODY_MAX && (body = mem_alloc((size_t)n.QuadPart))) {
            DWORD got = 0;
            if (ReadFile(f, body, (DWORD)n.QuadPart, &got, NULL) && got == (DWORD)n.QuadPart)
                size = got;
        }
        CloseHandle(f);
    }
    DeleteFileW(j->path);
    Done *r = mem_alloc(sizeof *r);
    if (r) {
        r->at = j->at;
        r->win = j->win;
        if (!size)
            lstrcpyW(r->text, L"PNG");
        else if ((r->ok = send_png(j, body, size, r->text, COUNTOF(r->text))))
            log_append(r->text);
        if (!PostMessageW(j->daemon, WM_APP_UPLOAD_DONE, 0, (LPARAM)r))
            mem_free(r);
    }
    mem_free(body);
    mem_free(j);
    return 0;
}

/* ---- the upload window: Open, Copy and the link, like Lightshot's ---- */

#define UPWIN_CLASS L"sniplite_upload"
enum { ID_OPEN = 1, ID_COPY, ID_LINK };

typedef struct {
    HWND daemon, open, copy, link;
    HFONT font;
    WCHAR url[URL_CAP];
} UpWin;

static bool is_web(const WCHAR *u) { return !wcsncmp(u, L"https://", 8) || !wcsncmp(u, L"http://", 7); }

static void open_url(const WCHAR *url)
{
    if (!is_web(url))
        return;
    HMODULE sh = LoadLibraryW(L"shell32.dll");
    PFN_ShellExecuteW run = sh ? (PFN_ShellExecuteW)(void *)GetProcAddress(sh, "ShellExecuteW") : NULL;
    if (run)
        run(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK upwin_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    UpWin *u = (UpWin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_COMMAND:
        if (!u)
            break;
        if (LOWORD(wp) == ID_OPEN) {
            open_url(u->url);
            PostMessageW(h, WM_CLOSE, 0, 0);
        } else if (LOWORD(wp) == ID_COPY && u->url[0]) {
            if (clip_put_text(u->daemon, u->url))
                SetWindowTextW(u->copy, T(PIN_COPIED));
        }
        return 0;
    case WM_CTLCOLORSTATIC: /* the read-only link: white like an edit box */
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_NCDESTROY:
        if (u) {
            if (u->font)
                DeleteObject(u->font);
            mem_free(u);
        }
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* the window under the mouse, buttons off until the link comes */
static HWND upwin_create(HWND daemon, POINT at)
{
    static bool registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = upwin_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = UPWIN_CLASS;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        registered = RegisterClassW(&wc) != 0;
        if (!registered)
            return NULL;
    }
    UpWin *u = mem_alloc(sizeof *u);
    if (!u)
        return NULL;
    u->daemon = daemon;
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP, ex = WS_EX_TOPMOST | WS_EX_DLGMODALFRAME;
    HWND h = CreateWindowExW(ex, UPWIN_CLASS, T(UP_TITLE), style, at.x, at.y, 100, 100, NULL, NULL, inst, NULL);
    if (!h) {
        mem_free(u);
        return NULL;
    }
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)u);
    UINT dpi = GetDpiForWindow(h);
    float s = dpi ? dpi / 96.f : 1.f;
#define S(v) ((int)lroundf((v) * s))
    u->font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, T(FONT));
    u->open = CreateWindowExW(0, L"BUTTON", T(UP_OPEN), WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED | BS_DEFPUSHBUTTON,
                              S(12), S(14), S(110), S(30), h, (HMENU)(INT_PTR)ID_OPEN, inst, NULL);
    u->copy = CreateWindowExW(0, L"BUTTON", T(SC_COPY), WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED,
                              S(132), S(14), S(110), S(30), h, (HMENU)(INT_PTR)ID_COPY, inst, NULL);
    u->link = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", T(UP_WORKING), WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
                              S(252), S(16), S(250), S(26), h, (HMENU)(INT_PTR)ID_LINK, inst, NULL);
    HWND kids[] = {u->open, u->copy, u->link};
    for (int i = 0; i < 3; i++)
        if (kids[i] && u->font)
            SendMessageW(kids[i], WM_SETFONT, (WPARAM)u->font, FALSE);
    RECT rc = {0, 0, S(514), S(58)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, ex, dpi ? dpi : 96);
    int w = rc.right - rc.left, hh = rc.bottom - rc.top;
#undef S
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
    int x = CLAMPI(at.x - w / 2, mi.rcWork.left, MAXI(mi.rcWork.left, mi.rcWork.right - w));
    int y = CLAMPI(at.y + 16, mi.rcWork.top, MAXI(mi.rcWork.top, mi.rcWork.bottom - hh));
    SetWindowPos(h, HWND_TOPMOST, x, y, w, hh, SWP_SHOWWINDOW);
    SetForegroundWindow(h);
    return h;
}

/* the link (ok) or what failed */
static void upwin_result(HWND h, bool ok, const WCHAR *text)
{
    UpWin *u = (UpWin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!u)
        return;
    if (ok) {
        lstrcpynW(u->url, text, URL_CAP);
        SetWindowTextW(u->link, text);
        EnableWindow(u->open, TRUE);
        EnableWindow(u->copy, TRUE);
        SetFocus(u->open);
    } else {
        WCHAR t[URL_CAP + 64];
        lstrcpynW(t, T(UP_FAILED), 64);
        lstrcatW(t, L": ");
        lstrcatW(t, text);
        SetWindowTextW(u->link, t);
    }
}

void upload_start(const uint32_t *px, int w, int h, HWND daemon)
{
    POINT at = {0, 0};
    GetCursorPos(&at);
    Job *j = mem_alloc(sizeof *j);
    if (!j)
        return;
    j->daemon = daemon;
    j->at = at;
    if (!base_url(j->url, COUNTOF(j->url))) {
        mem_free(j);
        MessageBoxW(NULL, T(UP_NOT_SET), L"sniplite", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return;
    }
    cfg_get(L"upload_token", j->token, COUNTOF(j->token));
    const WCHAR *why = NULL;
    WCHAR tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n > MAX_PATH - 48)
        why = L"TEMP";
    else {
        wsprintfW(j->path, L"%ssniplite_up_%lu_%lu.png", tmp, GetCurrentProcessId(), GetTickCount());
        if (!image_save(j->path, px, w, w, h))
            why = L"PNG";
        else if (!wh_load())
            why = L"winhttp.dll";
        else {
            j->win = upwin_create(daemon, at);
            HWND win = j->win;
            HANDLE th = CreateThread(NULL, 0, upload_thread, j, 0, NULL);
            if (th) {
                CloseHandle(th);
                if (!win)
                    notice_near(T(UP_WORKING), NULL, at, 30000); /* replaced by the outcome */
                return;
            }
            if (win)
                DestroyWindow(win);
            why = L"CreateThread";
        }
        DeleteFileW(j->path);
    }
    notice_near(T(UP_FAILED), why, at, 4000);
    mem_free(j);
}

void upload_done(HWND daemon, LPARAM lp)
{
    Done *r = (Done *)lp;
    if (!r)
        return;
    if (r->ok)
        clip_put_text(daemon, r->text);
    if (r->win && IsWindow(r->win)) /* still open: the outcome goes there */
        upwin_result(r->win, r->ok, r->text);
    else if (r->ok)
        notice_near(T(UP_DONE), r->text, r->at, 2500);
    else
        notice_near(T(UP_FAILED), r->text, r->at, 4000);
    mem_free(r);
}

void upload_open_gallery(void)
{
    WCHAR url[URL_CAP + 2];
    if (!base_url(url, URL_CAP)) {
        MessageBoxW(NULL, T(UP_NOT_SET), L"sniplite", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return;
    }
    lstrcatW(url, L"/");
    open_url(url);
}
