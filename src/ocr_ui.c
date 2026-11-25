/* Text recognition as the user meets it.
 *
 * Everything the user sees of it lives in a helper process (sniplite.exe --ocr): the
 * module's 20 MB, the text panel with its edit control, the notice. The background
 * program only hands over pictures, so nothing of this is ever loaded into it, and
 * all of it is gone when the helper leaves.
 *
 * The background program's side (the first half of this file):
 *   ocr_warm()   the mouse is over a text button: start the helper, so the module
 *                is loaded by the click
 *   ocr_panel()  "recognize text": show the panel beside the selection, or hide it
 *   ocr_follow() called by the capture's refresh while the panel shows: a selection
 *                that changed is read again
 *   ocr_copy()   "copy text": the selection's text straight to the clipboard
 *   ocr_hide()   the capture ends, or does something else
 * Each is a small message down the pipe to the helper's standard input; a picture
 * travels as a file mapping.
 *
 * The helper's side (the second half): a thread reads the pipe, a thread reads the
 * text, the main thread owns the windows. The panel is owned by the capture window,
 * so it stays above it; it does not take the keyboard until it is clicked, so the
 * capture's own keys keep working. A helper with nothing on the screen and nothing
 * to do leaves after a while.
 *
 * sniplite.exe --ocr-file <picture> [--tab] prints a picture's text instead (UTF-8,
 * on standard output): for scripts. */
#include "ocr.h"
#include "image.h"
#include "lang.h"
#include "settings.h"
#include "shot.h"
#include "ov_layout.inc"
#include <dwmapi.h>
#include <math.h>

#define PANEL_CLASS L"sniplite.text"
#define NOTICE_CLASS L"sniplite.notice"
#define HUB_CLASS L"sniplite.ocr"
#define HELPER_IDLE_MS 30000 /* how long a helper with nothing to do stays */

enum { OP_PANEL = 1, OP_COPY, OP_HIDE };

typedef struct {
    uint32_t op;
    int32_t w, h;   /* the picture */
    uint64_t map;   /* its file mapping, a handle of the helper's own */
    RECT at;        /* what it shows, on the screen: the panel goes beside it, a notice into its middle */
    uint64_t owner; /* OP_PANEL: the window the panel belongs to */
} Wire;

/* ---- the background program's side ------------------------------------------------- */

extern uint8_t O[]; /* the capture's state, laid out as ov_layout.inc says */
void enter_edit(const RECT *r);
void ov_cancel(void);
#define OV_I32(f) (*(int32_t *)(O + (f)))
#define OV_RC(f) ((RECT *)(O + (f)))

void finish(int act);
void refresh(void);
bool ov_begin(void);
bool ov_active(void);
extern const int32_t kTbOrder[], kTbPlain[]; /* the toolbar with and without the text buttons (ov_base.S) */
extern const int32_t *tb_order;
extern int32_t tb_count;
extern uint32_t ovp_scale_key; /* what the overlay's table of sizes was made for (ov_paint.S) */

uint8_t ocr_panel_on; /* read by refresh(): call ocr_follow() */
uint8_t ocr_there;

static struct {
    SRWLOCK lock;
    HANDLE proc, pipe; /* the helper and the pipe to its standard input */
    RECT sent;         /* the selection the panel was last given */
    bool seen;         /* the panel has been on the screen */
    int then;          /* a text capture waits for its selection: 1 show the text, 2 copy it */
} H;

/* Whether the module is installed decides what the toolbar offers. Looked up when it
 * can have changed, never on the way to a capture. */
void ocr_refresh(void)
{
    ocr_there = ocr_installed() || ocr_packed();
    tb_order = ocr_there ? kTbOrder : kTbPlain;
    tb_count = TB_ITEMS - (ocr_there ? 0 : 2);
    ovp_scale_key = ~0u; /* that table has the bar's width, counted from the old order */
}

static void helper_drop(void)
{
    if (H.proc) {
        CloseHandle(H.proc);
        CloseHandle(H.pipe);
        H.proc = H.pipe = NULL;
    }
}

static bool helper_start(void)
{
    if (H.proc && WaitForSingleObject(H.proc, 0) == WAIT_TIMEOUT)
        return true;
    helper_drop();
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd = NULL, wr = NULL;
    WCHAR exe[MAX_PATH * 2];
    if (!GetModuleFileNameW(NULL, exe, COUNTOF(exe)) || !CreatePipe(&rd, &wr, &sa, 0))
        return false;
    Str cmd = {0};
    str_add_quoted(&cmd, exe);
    str_add(&cmd, L" --ocr");
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_FORCEOFFFEEDBACK;
    si.hStdInput = rd;
    PROCESS_INFORMATION pi;
    AllowSetForegroundWindow(ASFW_ANY);
    bool ok = cmd.p && spawn_inheriting(exe, cmd.p, &si, &rd, 1, &pi);
    str_free(&cmd);
    CloseHandle(rd);
    if (!ok) {
        CloseHandle(wr);
        return false;
    }
    CloseHandle(pi.hThread);
    H.proc = pi.hProcess;
    H.pipe = wr;
    return true;
}

static DWORD WINAPI warm_thread(void *arg)
{
    (void)arg;
    AcquireSRWLockExclusive(&H.lock);
    if (ocr_there)
        helper_start();
    ReleaseSRWLockExclusive(&H.lock);
    return 0;
}

/* On a thread of its own: starting a process takes a few milliseconds, and the
 * caller is drawing a screenshot. */
void ocr_warm(void)
{
    HANDLE th = CreateThread(NULL, 0, warm_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
}

/* A picture for the helper: part of a screenshot, or pixels. */
typedef struct {
    const Shot *sh;
    RECT r; /* in the screenshot */
    const uint32_t *px;
    int w, h;
} Pic;

static bool job_send(uint32_t op, const Pic *pic, const RECT *at, HWND owner)
{
    Wire wire = {op, 0, 0, 0, {0, 0, 0, 0}, (uint64_t)(UINT_PTR)owner};
    HANDLE map = NULL;
    if (at)
        wire.at = *at;
    if (pic) {
        wire.w = pic->px ? pic->w : pic->r.right - pic->r.left;
        wire.h = pic->px ? pic->h : pic->r.bottom - pic->r.top;
        if (wire.w <= 0 || wire.h <= 0)
            return false;
        size_t bytes = (size_t)wire.w * wire.h * 4;
        map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)((uint64_t)bytes >> 32), (DWORD)bytes, NULL);
        void *view = map ? MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, bytes) : NULL;
        if (!view) {
            if (map)
                CloseHandle(map);
            return false;
        }
        if (pic->px)
            memcpy(view, pic->px, bytes);
        else
            shot_copy(pic->sh, pic->r, view, wire.w);
        UnmapViewOfFile(view);
    }
    bool ok = false;
    AcquireSRWLockExclusive(&H.lock);
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        if (op == OP_HIDE ? !H.proc : !helper_start())
            break; /* nobody to hide anything, or no helper to be had */
        HANDLE theirs = NULL;
        DWORD wrote;
        if (map && !DuplicateHandle(GetCurrentProcess(), map, H.proc, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            helper_drop();
            continue;
        }
        wire.map = (uint64_t)(UINT_PTR)theirs;
        ok = WriteFile(H.pipe, &wire, sizeof wire, &wrote, NULL) && wrote == sizeof wire;
        if (!ok)
            helper_drop(); /* it had left: once more, with a new one */
    }
    ReleaseSRWLockExclusive(&H.lock);
    if (map)
        CloseHandle(map);
    return ok;
}

/* false once the panel was closed from its own side (Esc, its copy button) */
static bool panel_alive(void)
{
    HWND p = FindWindowW(PANEL_CLASS, NULL);
    bool shown = p && IsWindowVisible(p);
    if (shown)
        H.seen = true;
    return shown || !H.seen;
}

void ocr_hide(void)
{
    if (!ocr_panel_on)
        return;
    ocr_panel_on = 0;
    H.then = 0;
    job_send(OP_HIDE, NULL, NULL, NULL);
}

/* the selection: its pixels as they were on the screen (no annotations), and where */
static Pic selection(RECT *at)
{
    const Shot *sh = (const Shot *)(O + OV_SHOT);
    Pic pic = {sh, *OV_RC(OV_SEL), NULL, 0, 0};
    at->left = sh->vx + pic.r.left, at->top = sh->vy + pic.r.top;
    at->right = sh->vx + pic.r.right, at->bottom = sh->vy + pic.r.bottom;
    return pic;
}

void ocr_follow(void)
{
    if (H.then) { /* a text capture: once the selection is made, from the message loop (this is inside a repaint) */
        if (OV_I32(OV_PHASE) != PH_EDIT || OV_I32(OV_DRAG) != DR_NONE)
            return;
        PostMessageW(*(HWND *)(O + OV_DAEMON), WM_APP_OCR_THEN, (WPARAM)H.then, 0);
        H.then = 0;
        ocr_panel_on = 0;
        return;
    }
    if (OV_I32(OV_PHASE) != PH_EDIT) {
        ocr_hide(); /* back to choosing: there is no selection to read */
        return;
    }
    RECT sel = *OV_RC(OV_SEL);
    if (OV_I32(OV_DRAG) != DR_NONE || rect_eq(&sel, &H.sent))
        return; /* still being dragged, or read already */
    if (!panel_alive()) {
        ocr_panel_on = 0;
        return;
    }
    RECT at;
    Pic pic = selection(&at);
    if (job_send(OP_PANEL, &pic, &at, *(HWND *)(O + OV_HWND)))
        H.sent = sel;
    else
        ocr_panel_on = 0;
}

static bool module_there(void)
{
    ocr_refresh();
    if (ocr_there)
        return true;
    settings_open(L"ocr"); /* where it can be downloaded */
    return false;
}

void ocr_panel(void)
{
    if (ocr_panel_on && panel_alive()) {
        ocr_hide();
        return;
    }
    if (OV_I32(OV_PHASE) == PH_SELECT) { /* choosing: take the highlighted window first */
        RECT hl = *OV_RC(OV_HL);
        if (O[OV_LDOWN] || rect_empty(&hl))
            return;
        enter_edit(&hl);
    }
    if (OV_I32(OV_PHASE) != PH_EDIT || OV_I32(OV_DRAG) != DR_NONE)
        return;
    if (!module_there()) {
        ov_cancel();
        return;
    }
    memset(&H.sent, 0, sizeof H.sent);
    H.seen = false;
    ocr_panel_on = 1;
    ocr_follow();
}

void ocr_copy(void)
{
    RECT at;
    Pic pic = selection(&at);
    if (module_there())
        job_send(OP_COPY, &pic, &at, NULL);
}

void ocr_then(int mode)
{
    if (!ov_active() || OV_I32(OV_PHASE) != PH_EDIT)
        return;
    if (mode == 2) {
        finish(ACT_OCR);
    } else {
        ocr_panel();
        refresh();
    }
}

/* The text hotkeys and `sniplite ocr`. While capturing they act on the selection
 * there is; otherwise a capture begins that reads its selection as soon as it is made. */
void ocr_capture(bool copy)
{
    if (ov_active()) {
        ocr_then(copy ? 2 : 1);
        return;
    }
    if (!module_there())
        return;
    H.then = copy ? 2 : 1;
    ocr_panel_on = 1;
    ocr_warm(); /* the module loads while the selection is being made */
    if (!ov_begin()) {
        H.then = 0;
        ocr_panel_on = 0;
    }
}

bool ocr_picture(const uint32_t *px, int w, int h, RECT beside, HWND owner)
{
    Pic pic = {NULL, {0, 0, 0, 0}, px, w, h};
    if (!module_there())
        return false;
    H.seen = false;
    return job_send(owner ? OP_PANEL : OP_COPY, &pic, &beside, owner);
}

bool ocr_picture_shown(void)
{
    if (!panel_alive())
        return false;
    if (H.seen)
        return true;
    AcquireSRWLockShared(&H.lock); /* not on the screen yet: only as long as the helper that was asked is there */
    bool lives = H.proc && WaitForSingleObject(H.proc, 0) == WAIT_TIMEOUT;
    ReleaseSRWLockShared(&H.lock);
    return lives;
}

void ocr_picture_hide(void) { job_send(OP_HIDE, NULL, NULL, NULL); }

/* ---- the helper process ------------------------------------------------------------ */

#define WM_APP_JOB (WM_APP + 1)  /* lParam = Wire*, from the pipe */
#define WM_APP_DONE (WM_APP + 2) /* lParam = Result*, from the reading thread */
#define WM_APP_EOF (WM_APP + 3)  /* the background program closed the pipe, or is gone */
#define TIMER_IDLE 1
#define TIMER_SLOW 2 /* reading takes long enough to say so */
#define TIMER_STAY 3
#define TIMER_FADE 4
#define ID_COPY 10
#define ID_TABLE 11
#define ID_MD 13
#define ID_EDIT 12

typedef struct {
    Wire wire;
    bool good, table;
    Str plain, tab; /* the text line by line, and as a table when it is one */
    WCHAR why[160];
} Result;

static struct {
    HWND hub;
    HWND panel, edit, copy, table;
    HFONT font;
    HBRUSH white;
    float s;
    HWND owner;
    RECT at;        /* the selection on the screen */
    Str plain, tab; /* what the panel shows, both ways */
    bool as_table;
    bool as_md;   /* the table written as Markdown: chosen by its button, and kept for the next table */
    HWND mark;    /* that button */
    Str md;
    int pw;       /* the width the panel wants: more for a table */
    int table_px; /* how wide the table's columns are together */
    const WCHAR *status; /* a line at the bottom left, or NULL */
    Wire *next;   /* the newest job not started; an older one waiting is dropped */
    bool running; /* the reading thread is at work */
    bool eof;
    DWORD idle; /* tick of the last sign of work */
} U;

static struct {
    HWND hwnd;
    POINT at;       /* its centre */
    WCHAR line[96];  /* what happened */
    WCHAR more[400]; /* the start of the text, why it failed, or a path */
    bool corner;     /* in the lower right corner of its monitor, and more is a path */
    int alpha;
} N;

static int px(float v) { return (int)lroundf(v * U.s); }

/* ---- the notice: one or two lines where the picture was ---- */

static HFONT ui_font(float s, float size)
{
    return CreateFontW(-(int)lroundf(size * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, T(FONT));
}

static void notice_paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC win = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    /* drawn off screen, shown at once: the second line's font can take a moment the first time */
    HDC mem = CreateCompatibleDC(win);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(win, rc.right, rc.bottom) : NULL;
    HGDIOBJ oldbmp = bmp ? SelectObject(mem, bmp) : NULL;
    HDC dc = bmp ? mem : win;
    float s = dpi_scale_at(N.at);
    HBRUSH br = CreateSolidBrush(RGB(32, 32, 32));
    FillRect(dc, &rc, br);
    DeleteObject(br);
    SetBkMode(dc, TRANSPARENT);
    int pad = (int)lroundf(10.f * s);
    HFONT f = ui_font(s, 13.f), f2 = ui_font(s, 11.f);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz;
    GetTextExtentPoint32W(dc, N.line, lstrlenW(N.line), &sz);
    RECT r = {pad, pad - (int)lroundf(2.f * s), rc.right - pad, rc.bottom};
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, N.line, -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (N.more[0]) {
        r.top += sz.cy + (int)lroundf(3.f * s);
        SelectObject(dc, f2);
        SetTextColor(dc, RGB(176, 176, 176));
        DrawTextW(dc, N.more, -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | (N.corner ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS));
    }
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteObject(f2);
    if (bmp) {
        BitBlt(win, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
    }
    if (mem)
        DeleteDC(mem);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK notice_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: notice_paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return HTTRANSPARENT; /* never in the way of a click */
    case WM_DESTROY: N.hwnd = NULL; return 0; /* closed by a newer one in the corner */
    case WM_TIMER:
        if (wp == TIMER_STAY) {
            KillTimer(h, TIMER_STAY);
            SetTimer(h, TIMER_FADE, 16, NULL);
        } else if ((N.alpha -= 20) > 0) {
            SetLayeredWindowAttributes(h, 0, (BYTE)N.alpha, LWA_ALPHA);
        } else {
            KillTimer(h, TIMER_FADE);
            ShowWindow(h, SW_HIDE);
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* Show (or change) the notice, sized for its text and kept on its monitor; it fades
 * away after stay_ms. */
static void notice_show(const WCHAR *line, const WCHAR *more, int stay_ms)
{
    lstrcpynW(N.line, line, COUNTOF(N.line));
    lstrcpynW(N.more, more ? more : L"", COUNTOF(N.more));
    for (WCHAR *c = N.more; *c; c++) /* one line: the text's line breaks become spaces */
        if (*c == L'\r' || *c == L'\n' || *c == L'\t')
            *c = L' ';
    if (!N.hwnd) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = notice_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = NOTICE_CLASS;
        RegisterClassW(&wc);
        N.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                 NOTICE_CLASS, L"sniplite", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
        if (!N.hwnd)
            return;
        int round = 2; /* DWMWCP_ROUND, Windows 11; ignored before */
        DwmSetWindowAttribute(N.hwnd, 33, &round, sizeof round);
        SetWindowDisplayAffinity(N.hwnd, 0x11); /* WDA_EXCLUDEFROMCAPTURE: never in the next screenshot */
    }
    float s = dpi_scale_at(N.at);
    HDC dc = GetDC(N.hwnd);
    HFONT f = ui_font(s, 13.f), f2 = ui_font(s, 11.f);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE a, b = {0, 0};
    GetTextExtentPoint32W(dc, N.line, lstrlenW(N.line), &a);
    if (N.more[0]) {
        SelectObject(dc, f2);
        GetTextExtentPoint32W(dc, N.more, lstrlenW(N.more), &b);
    }
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteObject(f2);
    ReleaseDC(N.hwnd, dc);
    int pad = (int)lroundf(10.f * s), widest = (int)lroundf(360.f * s);
    int w = MINI(MAXI(a.cx, b.cx), widest) + pad * 2 + (int)lroundf(12.f * s);
    int h = a.cy + (N.more[0] ? b.cy + (int)lroundf(3.f * s) : 0) + pad * 2 - (int)lroundf(4.f * s);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(N.at, MONITOR_DEFAULTTONEAREST), &mi);
    int x = CLAMPI(N.at.x - w / 2, mi.rcWork.left, MAXI(mi.rcWork.left, mi.rcWork.right - w));
    int y = CLAMPI(N.at.y - h / 2, mi.rcWork.top, MAXI(mi.rcWork.top, mi.rcWork.bottom - h));
    if (N.corner) {
        x = mi.rcWork.right - w - (int)lroundf(16.f * s);
        y = mi.rcWork.bottom - h - (int)lroundf(16.f * s);
    }
    N.alpha = 235;
    KillTimer(N.hwnd, TIMER_FADE);
    SetLayeredWindowAttributes(N.hwnd, 0, (BYTE)N.alpha, LWA_ALPHA);
    SetWindowPos(N.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(N.hwnd, NULL, TRUE);
    UpdateWindow(N.hwnd);
    SetTimer(N.hwnd, TIMER_STAY, (UINT)stay_ms, NULL);
}

/* For the helper of a quick capture, and for "also save" when none started (main.c): saved or not, and where. */
void notice_corner(const WCHAR *line, const WCHAR *path, POINT on_monitor, int stay_ms)
{
    for (HWND w = NULL; (w = FindWindowExW(NULL, w, NOTICE_CLASS, NULL));)
        if (w != N.hwnd)
            PostMessageW(w, WM_CLOSE, 0, 0); /* one at a time: the one before, of another helper, goes */
    N.at = on_monitor;
    N.corner = true;
    notice_show(line, path, stay_ms);
}

void notice_near(const WCHAR *line, const WCHAR *more, POINT at, int stay_ms)
{
    N.at = at;
    N.corner = false;
    notice_show(line, more, stay_ms);
}

/* until it has faded */
void notice_wait(void)
{
    MSG m;
    while (N.hwnd && IsWindowVisible(N.hwnd) && GetMessageW(&m, NULL, 0, 0) > 0)
        DispatchMessageW(&m);
}

/* ---- the panel: the text beside the selection ---- */

static void panel_status(const WCHAR *t)
{
    if (U.status == t)
        return;
    U.status = t;
    if (U.panel) {
        RECT rc;
        GetClientRect(U.panel, &rc);
        rc.top = rc.bottom - px(44);
        InvalidateRect(U.panel, &rc, FALSE);
    }
}

/* Tab stops that make the columns of a table line up: each column as wide as its
 * widest cell. An edit control counts them in quarters of a character's width. */
static void edit_tabs(void)
{
    enum { COLS = 64 };
    int wide[COLS] = {0}, ncol = 0;
    UINT stop[COLS];
    HDC dc = GetDC(U.edit);
    HGDIOBJ old = SelectObject(dc, U.font);
    TEXTMETRICW tm;
    SIZE letters = {0, 0};
    GetTextMetricsW(dc, &tm);
    GetTextExtentPoint32W(dc, L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52, &letters);
    for (const WCHAR *p = U.tab.p, *end = p + U.tab.n; p < end;) {
        int col = 0;
        for (;; col++) {
            const WCHAR *q = p;
            while (q < end && *q != L'\t' && *q != L'\r')
                q++;
            SIZE sz;
            if (col < COLS && q > p && GetTextExtentPoint32W(dc, p, (int)(q - p), &sz))
                wide[col] = MAXI(wide[col], sz.cx);
            p = q;
            if (p >= end || *p != L'\t')
                break;
            p++;
        }
        ncol = MAXI(ncol, MINI(col + 1, COLS));
        while (p < end && (*p == L'\r' || *p == L'\n'))
            p++;
    }
    SelectObject(dc, old);
    ReleaseDC(U.edit, dc);
    int unit = MAXI(tm.tmAveCharWidth, 1), at = 0;
    if (letters.cx > 0)
        unit = (letters.cx / 26 + 1) / 2; /* a character's width as dialogs count it */
    for (int c = 0; c < ncol; c++) {
        at += wide[c] + px(16);
        stop[c] = (UINT)((at * 4 + unit - 1) / unit);
    }
    U.table_px = at;
    SendMessageW(U.edit, EM_SETTABSTOPS, (WPARAM)ncol, (LPARAM)stop);
}

/* The table as Markdown: | a | b | for every row, the rule Markdown wants under the
 * first. Every row gets as many cells as the widest; a | inside a cell is escaped. */
static void md_build(void)
{
    const WCHAR *p = U.tab.p, *end = p + U.tab.n;
    int ncol = 1, col = 1;
    for (const WCHAR *c = p; c < end; c++)
        if (*c == L'\t') {
            col++;
        } else if (*c == L'\n') {
            ncol = MAXI(ncol, col);
            col = 1;
        }
    ncol = MAXI(ncol, col);
    U.md.n = 0;
    for (int row = 0; p < end; row++) {
        str_add(&U.md, L"|");
        for (int c = 0; c < ncol; c++) {
            str_add(&U.md, L" ");
            for (; p < end && *p != L'\t' && *p != L'\r' && *p != L'\n'; p++) {
                if (*p == L'|')
                    str_add(&U.md, L"\\");
                str_addn(&U.md, p, 1);
            }
            str_add(&U.md, L" |");
            if (p < end && *p == L'\t')
                p++;
        }
        while (p < end && (*p == L'\r' || *p == L'\n'))
            p++;
        if (row == 0) {
            str_add(&U.md, L"\r\n|");
            for (int c = 0; c < ncol; c++)
                str_add(&U.md, L" --- |");
        }
        if (p < end)
            str_add(&U.md, L"\r\n");
    }
}

/* The edit control is made anew for each way of showing the text: lines wrap, a
 * table scrolls sideways, and a control cannot change between the two. */
static void edit_make(void)
{
    bool focus = U.edit && GetFocus() == U.edit;
    if (U.edit)
        DestroyWindow(U.edit);
    RECT rc;
    GetClientRect(U.panel, &rc);
    /* shown only when its text is in it: a control that gets a scroll bar after its first paint leaves lines undrawn */
    DWORD style = WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL;
    if (U.as_table)
        style |= WS_HSCROLL | ES_AUTOHSCROLL;
    U.edit = CreateWindowExW(0, L"EDIT", NULL, style, px(10), px(8), rc.right - px(10) - 1, rc.bottom - px(44) - px(8), U.panel,
                             (HMENU)(INT_PTR)ID_EDIT, GetModuleHandleW(NULL), NULL);
    SendMessageW(U.edit, WM_SETFONT, (WPARAM)U.font, 0);
    SendMessageW(U.edit, EM_SETLIMITTEXT, 0, 0);
    if (U.as_table)
        edit_tabs();
    const Str *t = !U.as_table ? &U.plain : U.as_md ? &U.md : &U.tab;
    SetWindowTextW(U.edit, t->n ? t->p : L"");
    SetWindowPos(U.edit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (focus)
        SetFocus(U.edit);
}

static void panel_layout(void)
{
    RECT rc;
    GetClientRect(U.panel, &rc);
    int bh = px(26), by = rc.bottom - px(44) + (px(44) - bh) / 2, cw = px(84), tw = px(72), mw = px(104), gap = px(8);
    SetWindowPos(U.copy, NULL, rc.right - gap - cw, by, cw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(U.table, NULL, rc.right - gap - cw - gap - tw, by, tw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(U.mark, NULL, rc.right - gap - cw - gap - tw - gap - mw, by, mw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    if (U.edit)
        SetWindowPos(U.edit, NULL, px(10), px(8), rc.right - px(10) - 1, rc.bottom - px(44) - px(8), SWP_NOZORDER | SWP_NOACTIVATE);
}

static void panel_paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc, bar;
    GetClientRect(h, &rc);
    bar = rc;
    bar.top = rc.bottom - px(44);
    rc.bottom = bar.top;
    FillRect(dc, &rc, U.white);
    HBRUSH grey = CreateSolidBrush(RGB(243, 243, 243)), line = CreateSolidBrush(RGB(222, 222, 222));
    FillRect(dc, &bar, grey);
    RECT top = bar;
    top.bottom = top.top + 1;
    FillRect(dc, &top, line);
    GetClientRect(h, &rc);
    FrameRect(dc, &rc, line);
    DeleteObject(grey);
    DeleteObject(line);
    if (U.status) {
        HGDIOBJ old = SelectObject(dc, U.font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(110, 110, 110));
        RECT r = {px(12), bar.top, rc.right - px(8) - px(84) - (IsWindowVisible(U.table) ? px(8) + px(72) : 0) -
                                       (IsWindowVisible(U.mark) ? px(8) + px(104) : 0) - px(8), bar.bottom};
        DrawTextW(dc, U.status, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
    }
    EndPaint(h, &ps);
}

static void job_drop(Wire *w)
{
    if (w && w->map)
        CloseHandle((HANDLE)(UINT_PTR)w->map);
    mem_free(w);
}

static void panel_hide(bool by_user)
{
    if (U.next && U.next->op == OP_PANEL) {
        job_drop(U.next);
        U.next = NULL;
    }
    KillTimer(U.hub, TIMER_SLOW);
    if (!U.panel || !IsWindowVisible(U.panel))
        return;
    ShowWindow(U.panel, SW_HIDE);
    if (by_user && IsWindow(U.owner))
        SetForegroundWindow(U.owner); /* the keyboard back to the capture */
}

/* the whole text to the clipboard, and the capture is done. When the clipboard cannot be
 * had, the panel stays with its text for another try. */
static void panel_copy(void)
{
    int n = GetWindowTextLengthW(U.edit);
    WCHAR *t = mem_alloc(((size_t)n + 1) * sizeof(WCHAR));
    if (!t)
        return;
    GetWindowTextW(U.edit, t, n + 1);
    bool wanted = n > 0 && IsWindowEnabled(U.copy);
    N.at.x = (U.at.left + U.at.right) / 2, N.at.y = (U.at.top + U.at.bottom) / 2;
    if (wanted && !clip_put_text(U.panel, t)) {
        notice_show(T(COPY_FAILED), NULL, 3000);
        mem_free(t);
        return;
    }
    panel_hide(false);
    HWND d = ipc_find();
    if (d) {
        WCHAR *argv[1] = {L"_cancel"};
        Str reply = {0};
        ipc_call(d, 1, argv, &reply);
        str_free(&reply);
    }
    if (wanted)
        notice_show(T(OCR_COPIED), t, 1300);
    mem_free(t);
}

static LRESULT CALLBACK panel_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: panel_paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, RGB(255, 255, 255));
        return (LRESULT)U.white;
    case WM_NCHITTEST: { /* the bar at the bottom moves the panel */
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        RECT rc;
        ScreenToClient(h, &p);
        GetClientRect(h, &rc);
        return p.y >= rc.bottom - px(44) ? HTCAPTION : HTCLIENT;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_COPY) {
            panel_copy();
        } else if (LOWORD(wp) == ID_TABLE || LOWORD(wp) == ID_MD) {
            U.as_table = SendMessageW(U.table, BM_GETCHECK, 0, 0) == BST_CHECKED;
            U.as_md = SendMessageW(U.mark, BM_GETCHECK, 0, 0) == BST_CHECKED;
            ShowWindow(U.mark, U.as_table ? SW_SHOW : SW_HIDE);
            if (U.as_table && U.as_md)
                md_build();
            edit_make();
        } else if (LOWORD(wp) == IDCANCEL) {
            panel_hide(true);
        }
        return 0;
    case WM_DESTROY: /* the capture window took it along */
        U.panel = U.edit = U.copy = U.table = U.mark = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* Beside the selection: to its right, else to its left, else over its right part. As
 * wide as it wants to be where there is room for that, never in the selection's way. */
static void panel_move(void)
{
    const RECT *sel = &U.at;
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromRect(sel, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT *mon = &mi.rcMonitor;
    int gap = px(8), least = px(340), pw = MAXI(U.pw, least), x;
    int right = mon->right - gap - (sel->right + gap), left = sel->left - gap - (mon->left + gap);
    if (right >= least) {
        pw = MINI(pw, right);
        x = sel->right + gap;
    } else if (left >= least) {
        pw = MINI(pw, left);
        x = sel->left - gap - pw;
    } else {
        pw = MINI(least, mon->right - mon->left - 2 * gap);
        x = mon->right - gap - pw;
    }
    int ph = CLAMPI(sel->bottom - sel->top, px(220), MAXI(px(220), mon->bottom - mon->top - 2 * gap));
    ph = MINI(ph, mon->bottom - mon->top - 2 * gap);
    int y = CLAMPI(sel->top, mon->top + gap, MAXI(mon->top + gap, mon->bottom - gap - ph));
    SetWindowPos(U.panel, HWND_TOP, x, y, pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    panel_layout();
}

static void panel_place(const Wire *w)
{
    RECT sel = w->at;
    POINT mid = {(sel.left + sel.right) / 2, (sel.top + sel.bottom) / 2};
    float s = dpi_scale_at(mid);
    HWND owner = (HWND)(UINT_PTR)w->owner;
    if (U.panel && (U.owner != owner || s != U.s)) {
        DestroyWindow(U.panel); /* another capture window or another scaling: begin again */
        U.panel = U.edit = NULL;
    }
    U.owner = owner, U.at = sel;
    if (!U.panel) {
        WNDCLASSW wc = {0};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = panel_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = PANEL_CLASS;
        RegisterClassW(&wc);
        U.s = s;
        if (U.font)
            DeleteObject(U.font);
        U.font = ui_font(s, 14.f);
        if (!U.white)
            U.white = CreateSolidBrush(RGB(255, 255, 255));
        U.panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT, PANEL_CLASS, L"sniplite",
                                  WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0, 0, owner, NULL, wc.hInstance, NULL);
        if (!U.panel)
            return;
        int round = 2; /* DWMWCP_ROUND, Windows 11; ignored before */
        DwmSetWindowAttribute(U.panel, 33, &round, sizeof round);
        U.copy = CreateWindowExW(0, L"BUTTON", T(OCR_COPY), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, U.panel,
                                 (HMENU)(INT_PTR)ID_COPY, wc.hInstance, NULL);
        U.table = CreateWindowExW(0, L"BUTTON", T(OCR_TABLE), WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, U.panel,
                                  (HMENU)(INT_PTR)ID_TABLE, wc.hInstance, NULL);
        U.mark = CreateWindowExW(0, L"BUTTON", L"Markdown", WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, U.panel,
                                 (HMENU)(INT_PTR)ID_MD, wc.hInstance, NULL);
        SendMessageW(U.copy, WM_SETFONT, (WPARAM)U.font, 0);
        SendMessageW(U.table, WM_SETFONT, (WPARAM)U.font, 0);
        SendMessageW(U.mark, WM_SETFONT, (WPARAM)U.font, 0);
        U.plain.n = U.tab.n = 0;
        U.as_table = false;
        U.status = NULL;
    }
    bool fresh = !IsWindowVisible(U.panel);
    if (fresh)
        U.pw = 0;
    panel_move();
    if (fresh) { /* empty until this selection's text is here */
        U.plain.n = U.tab.n = 0;
        U.as_table = false;
        ShowWindow(U.table, SW_HIDE);
        ShowWindow(U.mark, SW_HIDE);
        edit_make();
        EnableWindow(U.copy, FALSE);
        panel_status(NULL);
    }
    InvalidateRect(U.panel, NULL, FALSE);
    UpdateWindow(U.panel);
}

static void panel_fill(Result *r)
{
    str_free(&U.plain);
    str_free(&U.tab);
    U.plain = r->plain, U.tab = r->tab;
    memset(&r->plain, 0, sizeof r->plain);
    memset(&r->tab, 0, sizeof r->tab);
    U.as_table = r->table;
    ShowWindow(U.table, r->table ? SW_SHOW : SW_HIDE);
    SendMessageW(U.table, BM_SETCHECK, r->table ? BST_CHECKED : BST_UNCHECKED, 0);
    ShowWindow(U.mark, r->table ? SW_SHOW : SW_HIDE);
    SendMessageW(U.mark, BM_SETCHECK, U.as_md ? BST_CHECKED : BST_UNCHECKED, 0);
    if (r->table && U.as_md)
        md_build();
    edit_make();
    if (!r->good)
        SetWindowTextW(U.edit, r->why); /* the reason, whole: the line at the bottom has no room for it */
    int want = r->table ? MINI(U.table_px + px(10) + GetSystemMetrics(SM_CXVSCROLL) + px(12), px(680)) : 0;
    if (want != U.pw) { /* a table: wide enough for its columns, if the screen has the room */
        U.pw = want;
        panel_move();
    }
    EnableWindow(U.copy, U.plain.n > 0);
    panel_status(!r->good ? T(OCR_FAILED) : U.plain.n ? NULL : T(OCR_NONE));
    InvalidateRect(U.panel, NULL, FALSE);
}

/* ---- jobs ---- */

static DWORD WINAPI read_thread(void *arg)
{
    Result *r = arg;
    const Wire *w = &r->wire;
    HANDLE map = (HANDLE)(UINT_PTR)w->map;
    const uint32_t *pic = MapViewOfFile(map, FILE_MAP_READ, 0, 0, (SIZE_T)w->w * w->h * 4);
    OcrOut out;
    POINT mid = {(w->at.left + w->at.right) / 2, (w->at.top + w->at.bottom) / 2};
    r->good = pic && ocr_run(pic, w->w, w->w, w->h, dpi_scale_at(mid), &out);
    if (r->good) {
        ocr_text(&out, L' ', &r->plain);
        r->table = ocr_table(&out, pic, w->w, w->w, w->h, &r->tab);
        ocr_free(&out);
    } else {
        lstrcpynW(r->why, pic ? ocr_error() : L"", COUNTOF(r->why));
    }
    if (pic)
        UnmapViewOfFile(pic);
    CloseHandle(map);
    PostMessageW(U.hub, WM_APP_DONE, 0, (LPARAM)r);
    return 0;
}

static void job_pump(void)
{
    if (U.running || !U.next)
        return;
    Result *r = mem_alloc(sizeof *r);
    HANDLE th = NULL;
    if (r) {
        r->wire = *U.next;
        th = CreateThread(NULL, 0, read_thread, r, 0, NULL);
    }
    if (th) {
        CloseHandle(th);
        mem_free(U.next);
        U.running = true;
        SetTimer(U.hub, TIMER_SLOW, 350, NULL);
    } else {
        mem_free(r);
        job_drop(U.next);
    }
    U.next = NULL;
}

static void job_take(Wire *w)
{
    if (w->op == OP_HIDE) {
        panel_hide(false);
        mem_free(w);
        return;
    }
    if (w->op == OP_PANEL)
        panel_place(w);
    else
        N.at.x = (w->at.left + w->at.right) / 2, N.at.y = (w->at.top + w->at.bottom) / 2;
    job_drop(U.next);
    U.next = w;
    job_pump();
}

static void job_done(Result *r)
{
    U.running = false;
    KillTimer(U.hub, TIMER_SLOW);
    if (r->wire.op == OP_PANEL) {
        bool newer = U.next && U.next->op == OP_PANEL;
        if (!newer && U.panel && IsWindowVisible(U.panel))
            panel_fill(r);
    } else if (!r->good) {
        notice_show(T(OCR_FAILED), r->why, 3000);
    } else if (!r->plain.n) {
        notice_show(T(OCR_NONE), NULL, 1300);
    } else {
        const Str *t = r->table ? &r->tab : &r->plain;
        if (clip_put_text(U.hub, t->p))
            notice_show(T(OCR_COPIED), r->plain.p, 1500);
        else
            notice_show(T(COPY_FAILED), NULL, 3000);
    }
    str_free(&r->plain);
    str_free(&r->tab);
    mem_free(r);
    job_pump();
}

/* Anything on the screen or being read. A job that has just arrived is no concern
 * here: the pipe's thread posts it, and posted messages come before timers. */
static bool at_work(void)
{
    return U.running || U.next || (U.panel && IsWindowVisible(U.panel)) || (N.hwnd && IsWindowVisible(N.hwnd));
}

static LRESULT CALLBACK hub_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_APP_JOB: job_take((Wire *)lp); return 0;
    case WM_APP_DONE: job_done((Result *)lp); return 0;
    case WM_APP_EOF: U.eof = true; return 0;
    case WM_TIMER:
        if (wp == TIMER_SLOW) { /* long enough to wonder: say that it is being read */
            KillTimer(h, TIMER_SLOW);
            if (U.panel && IsWindowVisible(U.panel))
                panel_status(T(OCR_WORKING));
            else if (U.running)
                notice_show(T(OCR_WORKING), NULL, 60000);
        } else if (at_work()) {
            U.idle = GetTickCount();
        } else if (U.eof || GetTickCount() - U.idle > HELPER_IDLE_MS) {
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static DWORD WINAPI pipe_thread(void *arg)
{
    (void)arg;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    for (;;) {
        Wire *w = mem_alloc(sizeof *w);
        DWORD got = 0;
        if (!w || !ReadFile(in, w, sizeof *w, &got, NULL) || got != sizeof *w)
            break;
        PostMessageW(U.hub, WM_APP_JOB, 0, (LPARAM)w);
    }
    PostMessageW(U.hub, WM_APP_EOF, 0, 0);
    return 0;
}

/* sniplite.exe --ocr-file <picture> [--tab]: the text on standard output */
static int ocr_file(int argc, WCHAR **argv)
{
    Img img;
    OcrOut out;
    if (argc < 3 || !image_load(argv[2], &img))
        return EXIT_USAGE;
    Str text = {0};
    bool good = ocr_run(img.px, img.w, img.w, img.h, 1.0f, &out);
    if (good && !(argc > 3 && arg_eq(argv[3], L"--tab") && ocr_table(&out, img.px, img.w, img.w, img.h, &text)))
        ocr_text(&out, L' ', &text);
    img_destroy(&img);
    if (!good)
        return EXIT_FAIL;
    str_add(&text, L"\r\n");
    int n = WideCharToMultiByte(CP_UTF8, 0, text.p, text.n, NULL, 0, NULL, NULL);
    char *u = mem_alloc((size_t)n + 1);
    DWORD wrote;
    if (u) {
        WideCharToMultiByte(CP_UTF8, 0, text.p, text.n, u, n, NULL, NULL);
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), u, (DWORD)n, &wrote, NULL);
    }
    mem_free(u);
    int lines = out.n;
    str_free(&text);
    ocr_free(&out);
    return lines ? EXIT_OK : EXIT_NOTHING;
}

/* sniplite.exe --ocr: load the module, then do what comes down the pipe */
int ocr_helper(int argc, WCHAR **argv)
{
    if (argc > 1 && arg_eq(argv[1], L"--ocr-file"))
        return ocr_file(argc, argv);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = hub_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = HUB_CLASS;
    RegisterClassW(&wc);
    U.hub = CreateWindowExW(0, HUB_CLASS, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!U.hub)
        return EXIT_FAIL;
    U.idle = GetTickCount();
    HANDLE th = CreateThread(NULL, 0, pipe_thread, NULL, 0, NULL);
    if (!th)
        return EXIT_FAIL;
    CloseHandle(th);
    ocr_load(NULL, 0); /* jobs that came meanwhile wait in the queue; a failure is told with the first */
    SetTimer(U.hub, TIMER_IDLE, 2000, NULL);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (U.panel && (m.hwnd == U.panel || IsChild(U.panel, m.hwnd))) {
            if (m.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && m.wParam == VK_RETURN) {
                panel_copy(); /* Ctrl+Enter: like the copy button */
                continue;
            }
            if (m.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && m.wParam == 'A' && m.hwnd == U.edit) {
                SendMessageW(U.edit, EM_SETSEL, 0, -1);
                continue;
            }
            if (IsDialogMessageW(U.panel, &m)) /* Tab between the controls, Esc closes */
                continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return EXIT_OK;
}
