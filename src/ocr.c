/* Text recognition with the PP-OCR models, run by ONNX Runtime.
 *
 * Two networks do the work. The detector takes the whole picture and returns, for
 * every pixel, how likely it is to be text; connected regions of likely pixels are
 * the lines. The recognizer takes one line scaled to 48 pixels high and returns, for
 * every 8 pixels of width, a probability for each character it knows; the best
 * character of each step, with repeats and blanks dropped, is the text (CTC).
 *
 * The steps and their constants follow PaddleOCR's own pipeline. One simplification:
 * a line's box is the upright rectangle around its region, which is exact for the
 * level text of a screen and loose for rotated text.
 *
 * Nearly all the time is spent inside the networks, so what is done here is chosen
 * for them: every line is read by its own call, several calls at once (measured:
 * 1.6 times faster than batches), and a big picture from a scaled-up screen is given
 * to the detector smaller, never smaller than it would be on a 100% screen. The
 * loops of this file that touch every pixel use SSE2, which every x64 processor has. */
#pragma GCC optimize("O2") /* the rest of the program is built for size; this file's loops are worth more */
#include "ocr.h"
#include "ort.h"
#include <emmintrin.h>

#define DET_AREA 600000.0f  /* the detector is given about this many pixels when the screen's scaling allows */
#define DET_CAP 4000000.0f  /* and never more than this many */
#define DET_PAD 16          /* margin around the picture, so text at its edge is still found */
#define DET_THRESH 0.3f     /* a pixel is text above this */
#define BOX_THRESH 0.5f     /* a region is a line when its pixels average above this */
#define BOX_GROW 1.6f       /* the detector marks the core of a line: grow the box back */
#define REC_H 48            /* PP-OCR recognizer: the height a line is scaled to */
#define REC_MIN_W 320       /* the width it was trained at: narrower reads worse (measured) */
#define VIE_H 64            /* VieSVTR: its own height, any width from 32 to 1600 that 4 divides, no padding */
#define VIE_MIN_W 32
#define VIE_MAX_W 1600
#define REC_WORKERS 8       /* at most this many threads read lines at once */
#define TEXT_SCORE 0.5f     /* a line is kept when its characters average above this */
#define MAX_BOXES 4000

typedef struct {
    OrtSession *s;
    char *in, *out; /* names of its input and output */
} Net;

static struct {
    HMODULE dll;
    Ort o;
    OrtEnv *env;
    OrtMemoryInfo *mem;
    OrtAllocator *alloc;
    Net det, rec;
    char *dict;   /* the recognizer's characters, UTF-8, one per line */
    int *dict_at; /* where each starts; dict_at[ndict] is the end */
    int ndict;
    bool vie;     /* the recognizer is VieSVTR (Vietnamese), not a PP-OCR one: no character list in the model, logits out */
    int rec_h;    /* what its input is scaled to */
    int workers;  /* threads reading lines at once */
    WCHAR err[200];
} G;

const WCHAR *ocr_error(void) { return G.err; }

#ifdef OCR_PROFILE /* tests/ocr: where the detector's time goes */
double ocr_prof[6]; /* input, network, boxes */
#define PROF(i, t) (ocr_prof[i] += now_ms() - (t))
#else
#define PROF(i, t) ((void)(t))
#endif

static bool fail(const WCHAR *what)
{
    lstrcpynW(G.err, what, COUNTOF(G.err));
    return false;
}

static bool ok(OrtStatus *st)
{
    if (!st)
        return true;
    const char *m = G.o.GetErrorMessage(st);
    int i = 0;
    for (; m[i] && i < COUNTOF(G.err) - 1; i++)
        G.err[i] = (WCHAR)(unsigned char)m[i];
    G.err[i] = 0;
    G.o.ReleaseStatus(st);
    return false;
}

static void *raw_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); } /* not zeroed */

/* ---- the module's files ---- */

void ocr_dir(WCHAR *dir, int cap)
{
    int n = (int)GetModuleFileNameW(NULL, dir, (DWORD)cap - 8);
    while (n > 0 && dir[n - 1] != L'\\' && dir[n - 1] != L'/')
        n--;
    lstrcpyW(dir + n, L"ocr");
}

/* the first file in dir matching pattern, as a full path */
static bool find_file(const WCHAR *dir, const WCHAR *pattern, WCHAR *path, int cap)
{
    WIN32_FIND_DATAW fd;
    if (lstrlenW(dir) + lstrlenW(pattern) + 2 > cap)
        return false;
    lstrcpyW(path, dir);
    lstrcatW(path, L"\\");
    lstrcatW(path, pattern);
    HANDLE h = FindFirstFileW(path, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    FindClose(h);
    if (lstrlenW(dir) + lstrlenW(fd.cFileName) + 2 > cap)
        return false;
    lstrcpyW(path, dir);
    lstrcatW(path, L"\\");
    lstrcatW(path, fd.cFileName);
    return true;
}

bool ocr_installed(void)
{
    WCHAR dir[MAX_PATH * 2], path[MAX_PATH * 2];
    ocr_dir(dir, COUNTOF(dir));
    return find_file(dir, L"onnxruntime.dll", path, COUNTOF(path)) && find_file(dir, L"*det*.onnx", path, COUNTOF(path)) &&
           find_file(dir, L"*rec*.onnx", path, COUNTOF(path));
}

static bool net_open(Net *n, const WCHAR *dir, const WCHAR *pattern, const OrtSessionOptions *opts)
{
    WCHAR path[MAX_PATH * 2];
    if (!find_file(dir, pattern, path, COUNTOF(path)))
        return fail(L"a model file is missing");
    return ok(G.o.CreateSession(G.env, path, opts, &n->s)) && ok(G.o.SessionGetInputName(n->s, 0, G.alloc, &n->in)) &&
           ok(G.o.SessionGetOutputName(n->s, 0, G.alloc, &n->out));
}

/* The two models are opened side by side: opening one takes tens of milliseconds,
 * most of what loading the module costs. */
typedef struct {
    Net *net;
    const WCHAR *dir, *pattern;
    const OrtSessionOptions *opts;
    bool good;
} Opening;

static DWORD WINAPI open_thread(void *arg)
{
    Opening *o = arg;
    o->good = net_open(o->net, o->dir, o->pattern, o->opts);
    return 0;
}

static bool nets_open(const WCHAR *dir, const OrtSessionOptions *det, const OrtSessionOptions *rec)
{
    Opening r = {&G.rec, dir, L"*rec*.onnx", rec, G.rec.s != NULL};
    HANDLE th = r.good ? NULL : CreateThread(NULL, 0, open_thread, &r, 0, NULL);
    bool d = G.det.s || net_open(&G.det, dir, L"*det*.onnx", det);
    if (th) {
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
    } else if (!r.good)
        open_thread(&r);
    return d && r.good;
}

/* The characters of the Vietnamese recognizer (VieSVTR), whose model carries none: VietOCR's 229, one
 * per line, the space last. Entry i is what class i + 1 means; class 0 is the blank. */
static const char kVocab[] =
    "a\nA\nà\nÀ\nả\nẢ\nã\nÃ\ná\nÁ\nạ\nẠ\nă\nĂ\nằ\nẰ\nẳ\nẲ\nẵ\nẴ\nắ\nẮ\nặ\nẶ\nâ\nÂ\nầ\nẦ\nẩ\nẨ\nẫ\nẪ\n"
    "ấ\nẤ\nậ\nẬ\nb\nB\nc\nC\nd\nD\nđ\nĐ\ne\nE\nè\nÈ\nẻ\nẺ\nẽ\nẼ\né\nÉ\nẹ\nẸ\nê\nÊ\nề\nỀ\nể\nỂ\nễ\nỄ\n"
    "ế\nẾ\nệ\nỆ\nf\nF\ng\nG\nh\nH\ni\nI\nì\nÌ\nỉ\nỈ\nĩ\nĨ\ní\nÍ\nị\nỊ\nj\nJ\nk\nK\nl\nL\nm\nM\nn\nN\n"
    "o\nO\nò\nÒ\nỏ\nỎ\nõ\nÕ\nó\nÓ\nọ\nỌ\nô\nÔ\nồ\nỒ\nổ\nỔ\nỗ\nỖ\nố\nỐ\nộ\nỘ\nơ\nƠ\nờ\nỜ\nở\nỞ\nỡ\nỠ\n"
    "ớ\nỚ\nợ\nỢ\np\nP\nq\nQ\nr\nR\ns\nS\nt\nT\nu\nU\nù\nÙ\nủ\nỦ\nũ\nŨ\nú\nÚ\nụ\nỤ\nư\nƯ\nừ\nỪ\nử\nỬ\n"
    "ữ\nỮ\nứ\nỨ\nự\nỰ\nv\nV\nw\nW\nx\nX\ny\nY\nỳ\nỲ\nỷ\nỶ\nỹ\nỸ\ný\nÝ\nỵ\nỴ\nz\nZ\n0\n1\n2\n3\n4\n5\n"
    "6\n7\n8\n9\n!\n\"\n#\n$\n%\n&\n'\n(\n)\n*\n+\n,\n-\n.\n/\n:\n;\n<\n=\n>\n?\n@\n[\n\\\n]\n^\n_\n`\n"
    "{\n|\n}\n~\n ";

/* The characters a PP-OCR recognizer knows are stored in the model itself. A recognizer that has none is
 * VieSVTR, and has the list above. */
static bool dict_read(void)
{
    OrtModelMetadata *meta = NULL;
    if (!ok(G.o.SessionGetModelMetadata(G.rec.s, &meta)))
        return false;
    if (!G.dict) {
        ok(G.o.ModelMetadataLookupCustomMetadataMap(meta, G.alloc, "character", &G.dict)); /* a model without it leaves NULL */
        G.vie = !G.dict;
        if (G.vie)
            G.dict = (char *)kVocab;
    }
    G.o.ReleaseModelMetadata(meta);
    G.rec_h = G.vie ? VIE_H : REC_H;
    G.err[0] = 0;
    int n = 1, len = 0;
    for (; G.dict[len]; len++)
        n += G.dict[len] == '\n';
    G.dict_at = mem_alloc(((size_t)n + 1) * sizeof(int));
    if (!G.dict_at)
        return fail(L"out of memory");
    n = 0;
    G.dict_at[n++] = 0;
    for (int i = 0; i < len; i++)
        if (G.dict[i] == '\n')
            G.dict_at[n++] = i + 1;
    G.dict_at[n] = len + 1;
    G.ndict = n;
    return true;
}

bool ocr_load(const WCHAR *dir, int threads)
{
    if (G.rec.s && G.dict_at)
        return true;
    WCHAR own[MAX_PATH * 2], path[MAX_PATH * 2];
    if (!dir) {
        if (!ocr_installed() && ocr_packed())
            ocr_unpack(); /* the first time: out of the program's own file, beside it */
        ocr_dir(own, COUNTOF(own));
        dir = own;
    }
    if (!G.dll) {
        if (!find_file(dir, L"onnxruntime.dll", path, COUNTOF(path)))
            return fail(L"the text recognition module is not installed");
        /* By full path: Windows keeps an older onnxruntime.dll of its own in System32. The Visual
         * C++ runtime it needs is looked for beside it, where the module has its own, then in
         * System32, and nowhere else. */
        G.dll = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!G.dll) {
            wsprintfW(G.err, L"onnxruntime.dll could not be loaded (error %lu)", GetLastError());
            return false;
        }
        const OrtApiBase *(*base)(void) = (const OrtApiBase *(*)(void))(void *)GetProcAddress(G.dll, "OrtGetApiBase");
        const void *api = base ? base()->GetApi(ORT_API_WANTED) : NULL;
        if (api)
            ort_bind(&G.o, api);
    }
    if (!G.o.CreateEnv)
        return fail(L"onnxruntime.dll is too old");
    /* Measured on 14 cores, for one line, a table and a full window of text: the
     * detector is fastest with 6 threads of its own; the recognizer with 4 of its own
     * and 6 callers at once. More of either only get in each other's way. */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int cores = threads ? threads : (int)si.dwNumberOfProcessors;
    int det_threads = MINI(cores, 6), rec_threads = MINI(cores, 4);
    G.workers = MINI(MINI(cores, 6), REC_WORKERS);
#ifdef OCR_TUNE /* tests/ocr: OCR_TUNE=<detector threads>,<recognizer threads>,<readers> */
    {
        char v[64];
        int t[3] = {0, 0, 0}, k = 0;
        if (GetEnvironmentVariableA("OCR_TUNE", v, sizeof v)) {
            for (char *c = v; *c && k < 3; c++)
                if (*c == ',')
                    k++;
                else
                    t[k] = t[k] * 10 + (*c - '0');
            det_threads = t[0], rec_threads = t[1], G.workers = MINI(t[2], REC_WORKERS);
        }
    }
#endif
    OrtSessionOptions *det = NULL, *rec = NULL;
    if (!G.env && !ok(G.o.CreateEnv(ORT_LOG_WARNING + 1, "sniplite", &G.env)))
        return false;
    if (!G.alloc && !ok(G.o.GetAllocatorWithDefaultOptions(&G.alloc)))
        return false;
    if (!G.mem && !ok(G.o.CreateCpuMemoryInfo(ORT_ARENA_ALLOCATOR, ORT_MEM_DEFAULT, &G.mem)))
        return false;
    bool good = ok(G.o.CreateSessionOptions(&det)) && ok(G.o.CreateSessionOptions(&rec)) &&
                ok(G.o.SetIntraOpNumThreads(det, det_threads)) && ok(G.o.SetSessionGraphOptimizationLevel(det, ORT_OPTIMIZE_ALL)) &&
                ok(G.o.SetIntraOpNumThreads(rec, rec_threads)) && ok(G.o.SetSessionGraphOptimizationLevel(rec, ORT_OPTIMIZE_ALL)) &&
                nets_open(dir, det, rec) && dict_read();
    if (det)
        G.o.ReleaseSessionOptions(det);
    if (rec)
        G.o.ReleaseSessionOptions(rec);
    return good;
}

/* ---- pixels to the numbers the networks want: a colour byte v becomes v / 127.5 - 1 ---- */

/* The four pixels around a point blended with 8-bit weights (0..256 across, 0..256
 * down), all four channels at once, as four floats: blue, green, red, alpha. */
static inline __m128 sample(uint32_t p00, uint32_t p01, uint32_t p10, uint32_t p11, int wx, int wy)
{
    const __m128i zero = _mm_setzero_si128();
    __m128i wxv = _mm_unpacklo_epi64(_mm_set1_epi16((short)(256 - wx)), _mm_set1_epi16((short)wx));
    __m128i top = _mm_unpacklo_epi8(_mm_unpacklo_epi32(_mm_cvtsi32_si128((int)p00), _mm_cvtsi32_si128((int)p01)), zero);
    __m128i bot = _mm_unpacklo_epi8(_mm_unpacklo_epi32(_mm_cvtsi32_si128((int)p10), _mm_cvtsi32_si128((int)p11)), zero);
    top = _mm_mullo_epi16(top, wxv); /* at most 255 * 256: fits 16 bits */
    bot = _mm_mullo_epi16(bot, wxv);
    top = _mm_srli_epi16(_mm_add_epi16(top, _mm_srli_si128(top, 8)), 8); /* the four channels across the top */
    bot = _mm_srli_epi16(_mm_add_epi16(bot, _mm_srli_si128(bot, 8)), 8);
    __m128i v = _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(top, bot), _mm_set1_epi32((wy << 16) | (256 - wy))),
                              _mm_set1_epi32(128));
    return _mm_sub_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_srli_epi32(v, 8)), _mm_set1_ps(1.0f / 127.5f)), _mm_set1_ps(1.0f));
}

static inline void put3(__m128 v, float *b, float *g, float *r)
{
    _mm_store_ss(b, v);
    _mm_store_ss(g, _mm_shuffle_ps(v, v, 1));
    _mm_store_ss(r, _mm_shuffle_ps(v, v, 2));
}

/* n packed BGRA pixels to three rows of floats */
static void row_planes(const uint32_t *px, int n, float *b, float *g, float *r)
{
    const __m128i low = _mm_set1_epi32(255);
    const __m128 scale = _mm_set1_ps(1.0f / 127.5f), one = _mm_set1_ps(1.0f);
    int x = 0;
    for (; x + 4 <= n; x += 4) {
        __m128i p = _mm_loadu_si128((const __m128i *)(px + x));
        _mm_storeu_ps(b + x, _mm_sub_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(p, low)), scale), one));
        _mm_storeu_ps(g + x, _mm_sub_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(p, 8), low)), scale), one));
        _mm_storeu_ps(r + x, _mm_sub_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(p, 16), low)), scale), one));
    }
    for (; x < n; x++) {
        uint32_t p = px[x];
        b[x] = (float)(p & 255) * (1.0f / 127.5f) - 1.0f;
        g[x] = (float)((p >> 8) & 255) * (1.0f / 127.5f) - 1.0f;
        r[x] = (float)((p >> 16) & 255) * (1.0f / 127.5f) - 1.0f;
    }
}

/* where source pixels fall for each of n output pixels of a resize from sn: the left
 * neighbour and the weight (0..256) of the right one, as OpenCV's linear resize */
static void resize_map(int n, int sn, int *at, int *weight)
{
    float step = (float)sn / (float)n;
    for (int i = 0; i < n; i++) {
        float s = ((float)i + 0.5f) * step - 0.5f;
        if (s < 0)
            s = 0;
        at[i] = MINI((int)s, sn - 1);
        weight[i] = at[i] + 1 < sn ? (int)((s - (float)(int)s) * 256.0f) : 0;
    }
}

/* ---- finding the lines ---- */

typedef struct {
    int l, t, r, b; /* in the picture */
    float score;
} Box;

typedef struct {
    float *t;    /* 3 planes (blue, green, red) of W x H */
    int W, H;    /* multiples of 32 */
    int sw, sh;  /* the picture inside, after scaling, at (DET_PAD, DET_PAD) */
    float scale; /* <= 1 */
} DetIn;

/* How much smaller the detector may see the picture. It finds text down to about 9
 * pixels high (measured), and what is 12 pixels on a 100% screen is 24 on a 200% one:
 * so a big picture is shrunk towards DET_AREA, but never below its 100% size. */
static float det_scale(int w, int h, float screen)
{
    float area = (float)w * (float)h, s = 1, least = screen > 1 ? 1 / screen : 1;
    if (area > DET_AREA)
        s = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(DET_AREA / area)));
    if (s < least)
        s = least;
    if (area * s * s > DET_CAP)
        s = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(DET_CAP / area)));
    return s;
}

static bool det_input(const uint32_t *px, int stride, int w, int h, float scale, DetIn *d)
{
    d->scale = scale;
    d->sw = scale < 1 ? MAXI(1, (int)((float)w * scale + 0.5f)) : w;
    d->sh = scale < 1 ? MAXI(1, (int)((float)h * scale + 0.5f)) : h;
    d->W = (d->sw + 2 * DET_PAD + 31) & ~31;
    d->H = (d->sh + 2 * DET_PAD + 31) & ~31;
    size_t plane = (size_t)d->W * d->H;
    d->t = raw_alloc(plane * 3 * sizeof(float));
    int *xi = scale < 1 ? raw_alloc((size_t)d->sw * sizeof(int) * 2) : NULL;
    if (!d->t || (scale < 1 && !xi)) {
        mem_free(d->t);
        mem_free(xi);
        return fail(L"out of memory");
    }
    int *xw = xi ? xi + d->sw : NULL;
    if (scale < 1)
        resize_map(d->sw, w, xi, xw);
    float step = (float)h / (float)d->sh;
    for (int y = 0; y < d->sh; y++) {
        float *b = d->t + (size_t)(y + DET_PAD) * d->W + DET_PAD, *g = b + plane, *r = g + plane;
        if (scale < 1) {
            float s = ((float)y + 0.5f) * step - 0.5f;
            if (s < 0)
                s = 0;
            int y0 = MINI((int)s, h - 1), wy = y0 + 1 < h ? (int)((s - (float)(int)s) * 256.0f) : 0;
            const uint32_t *a = px + (size_t)y0 * stride, *c = px + (size_t)MINI(y0 + 1, h - 1) * stride;
            for (int x = 0; x < d->sw; x++) {
                int x0 = xi[x], x1 = MINI(x0 + 1, w - 1);
                put3(sample(a[x0], a[x1], c[x0], c[x1], xw[x], wy), b + x, g + x, r + x);
            }
        } else {
            row_planes(px + (size_t)y * stride, w, b, g, r);
        }
        /* the margin left and right repeats the first and the last pixel */
        for (float *p = b; p <= r; p += r - g) {
            for (int x = -DET_PAD; x < 0; x++)
                p[x] = p[0];
            for (int x = d->sw; x < d->W - DET_PAD; x++)
                p[x] = p[d->sw - 1];
        }
    }
    /* and above and below, the first and the last row */
    for (int c = 0; c < 3; c++) {
        float *p = d->t + plane * c;
        for (int y = 0; y < DET_PAD; y++)
            memcpy(p + (size_t)y * d->W, p + (size_t)DET_PAD * d->W, (size_t)d->W * sizeof(float));
        for (int y = DET_PAD + d->sh; y < d->H; y++)
            memcpy(p + (size_t)y * d->W, p + (size_t)(DET_PAD + d->sh - 1) * d->W, (size_t)d->W * sizeof(float));
    }
    mem_free(xi);
    return true;
}

/* A run of text pixels in one row. Runs that touch, across rows too, are one region:
 * each run points towards its region's first run, which carries the region's box. */
typedef struct {
    int x0, x1, y; /* x1 inclusive */
    int up;        /* a run of the same region nearer its first; itself for the first */
    int l, r, b;   /* the region's box, kept in its first run (its top is that run's y) */
} Run;

static int run_first(Run *run, int i)
{
    while (run[i].up != i) {
        run[i].up = run[run[i].up].up; /* halve the path on the way */
        i = run[i].up;
    }
    return i;
}

/* The detector's map to boxes in the picture: the connected regions of text pixels,
 * each kept when it is big and sure enough, and grown by BOX_GROW.
 *
 * The map becomes one bit per pixel (4 pixels a comparison), each text pixel is
 * widened by one to the right and down as PaddleOCR does (two shifts and an OR), and
 * the rows are walked as runs of set bits found by bit scans: the cost is that of
 * reading the map once, plus a little per run. */
static int det_boxes(const float *p, const DetIn *d, int w, int h, Box *out)
{
    int W = d->W, H = d->H, nw = W / 64 + 1, n = 0;
    uint64_t *bits = mem_alloc((size_t)nw * 3 * sizeof(uint64_t)); /* this row, the one above, both widened */
    int cap = 4096, nrun = 0, above = 0, above_end = 0;
    Run *run = raw_alloc((size_t)cap * sizeof(Run));
    if (!bits || !run) {
        mem_free(bits);
        mem_free(run);
        return 0;
    }
    uint64_t *cur = bits, *prev = bits + nw, *wide = bits + 2 * nw;
    const __m128 thresh = _mm_set1_ps(DET_THRESH);
    for (int y = 0; y < H; y++) {
        const float *row = p + (size_t)y * W;
        for (int x = 0, k = 0; x < W; x += 64, k++) {
            uint64_t v = 0;
            for (int i = 0; i < 64 && x + i < W; i += 4)
                v |= (uint64_t)_mm_movemask_ps(_mm_cmpgt_ps(_mm_loadu_ps(row + x + i), thresh)) << i;
            cur[k] = v;
        }
        uint64_t carry = 0, any = 0;
        for (int k = 0; k < nw; k++) {
            uint64_t v = cur[k] | prev[k];
            wide[k] = v | v << 1 | carry;
            carry = v >> 63;
            any |= wide[k];
        }
        wide[nw - 1] &= ((uint64_t)1 << (W & 63)) - 1; /* nothing is widened past the right edge */
        int first = nrun;
        if (any) {
            /* a run starts at a set bit whose left neighbour is clear, and ends at one
             * whose right neighbour is clear: as many starts as ends, in the same order */
            uint64_t left = 0;
            for (int k = 0; k < nw; k++) {
                uint64_t v = wide[k], starts = v & ~(v << 1 | left);
                left = v >> 63;
                for (; starts; starts &= starts - 1) {
                    if (nrun == cap) {
                        Run *more = HeapReAlloc(GetProcessHeap(), 0, run, (size_t)(cap *= 2) * sizeof(Run));
                        if (!more) {
                            mem_free(bits);
                            mem_free(run);
                            return 0;
                        }
                        run = more;
                    }
                    Run *q = &run[nrun];
                    q->x0 = k * 64 + __builtin_ctzll(starts);
                    q->y = y;
                    q->up = nrun++;
                }
            }
            int at = first;
            for (int k = 0; k < nw; k++) {
                uint64_t v = wide[k], right = k + 1 < nw ? wide[k + 1] << 63 : 0, ends = v & ~(v >> 1 | right);
                for (; ends; ends &= ends - 1) {
                    Run *q = &run[at++];
                    q->x1 = k * 64 + __builtin_ctzll(ends);
                    q->l = q->x0, q->r = q->x1, q->b = y;
                }
            }
            /* join with the runs of the row above that touch, corners included */
            for (int i = above, j = first; i < above_end && j < nrun;) {
                if (run[j].x0 <= run[i].x1 + 1 && run[i].x0 <= run[j].x1 + 1) {
                    int a = run_first(run, i), b = run_first(run, j);
                    if (a != b) {
                        int keep = MINI(a, b), gone = MAXI(a, b);
                        run[gone].up = keep;
                        run[keep].l = MINI(run[keep].l, run[gone].l);
                        run[keep].r = MAXI(run[keep].r, run[gone].r);
                        run[keep].b = MAXI(run[keep].b, run[gone].b);
                    }
                }
                run[i].x1 < run[j].x1 ? i++ : j++;
            }
        }
        above = first, above_end = nrun;
        uint64_t *swap = cur;
        cur = prev, prev = swap;
    }
    float inv = 1.0f / d->scale;
    for (int i = 0; i < nrun && n < MAX_BOXES; i++) {
        const Run *q = &run[i];
        if (q->up != i)
            continue;
        int x0 = q->l, x1 = q->r, y0 = q->y, y1 = q->b;
        int bw = x1 - x0, bh = y1 - y0; /* as PaddleOCR measures: between the centres of the edge pixels */
        if (MINI(bw, bh) < 3)
            continue;
        __m128 sum4 = _mm_setzero_ps();
        float sum = 0;
        for (int y = y0; y <= y1; y++) {
            const float *row = p + (size_t)y * W;
            int x = x0;
            for (; x + 4 <= x1 + 1; x += 4)
                sum4 = _mm_add_ps(sum4, _mm_loadu_ps(row + x));
            for (; x <= x1; x++)
                sum += row[x];
        }
        float part[4];
        _mm_storeu_ps(part, sum4);
        float score = (sum + part[0] + part[1] + part[2] + part[3]) / (float)((bw + 1) * (bh + 1));
        if (score < BOX_THRESH)
            continue;
        float grow = (float)bw * (float)bh * BOX_GROW / (2.0f * (float)(bw + bh));
        if ((float)MINI(bw, bh) + 2 * grow < 5)
            continue;
        /* back to the picture: off with the margin, undo the scaling */
        Box *b = &out[n];
        b->l = CLAMPI((int)(((float)x0 - grow - DET_PAD) * inv + 0.5f), 0, w - 1);
        b->t = CLAMPI((int)(((float)y0 - grow - DET_PAD) * inv + 0.5f), 0, h - 1);
        b->r = CLAMPI((int)(((float)x1 + grow - DET_PAD) * inv + 0.5f), 0, w - 1);
        b->b = CLAMPI((int)(((float)y1 + grow - DET_PAD) * inv + 0.5f), 0, h - 1);
        b->score = score;
        if (b->r - b->l > 3 && b->b - b->t > 3)
            n++;
    }
    mem_free(bits);
    mem_free(run);
    return n;
}

static bool run(const Net *net, float *data, const int64_t *shape, OrtValue **out, float **values, int64_t *dims, int ndims)
{
    size_t bytes = sizeof(float);
    for (int i = 0; i < 4; i++)
        bytes *= (size_t)shape[i];
    OrtValue *in = NULL;
    OrtShape *sh = NULL;
    *out = NULL;
    const char *in_names[1] = {net->in}, *out_names[1] = {net->out};
    bool good = ok(G.o.CreateTensorWithDataAsOrtValue(G.mem, data, bytes, shape, 4, ORT_FLOAT, &in)) &&
                ok(G.o.Run(net->s, NULL, in_names, (const OrtValue *const *)&in, 1, out_names, 1, out)) &&
                ok(G.o.GetTensorMutableData(*out, (void **)values)) && ok(G.o.GetTensorTypeAndShape(*out, &sh));
    size_t nd = 0;
    if (good)
        good = ok(G.o.GetDimensionsCount(sh, &nd)) && ((int)nd == ndims || fail(L"a model answered in an unexpected shape")) &&
               ok(G.o.GetDimensions(sh, dims, nd));
    if (sh)
        G.o.ReleaseShape(sh);
    if (in)
        G.o.ReleaseValue(in);
    if (!good && *out) {
        G.o.ReleaseValue(*out);
        *out = NULL;
    }
    return good;
}

/* ---- reading the lines ---- */

/* Index and value of the largest of n numbers. When they are probabilities (they add
 * up to 1), nothing can beat one above a half, so the search stops there: for the
 * blank, which is most steps, at the first number. */
static int argmax(const float *v, int n, bool probabilities, float *best)
{
    int at = 0;
    float b = v[0], enough = probabilities ? 0.5f : 3.0e38f;
    for (int i = 1; i < n && b <= enough; i++)
        if (v[i] > b) {
            b = v[i];
            at = i;
        }
    *best = b;
    return at;
}

/* what one reading thread keeps between lines, so nothing is allocated per line */
typedef struct {
    float *t;     /* the recognizer's input */
    size_t tcap;  /* in floats */
    int *map;     /* resize_map's two arrays */
    int mapcap;
    Str chars;    /* the text of the lines this thread read */
} Scratch;

/* One line of the picture as the recognizer's input: its box scaled to REC_H high and
 * rw wide into 3 planes of row length W, the rest of each row zero (the padding the
 * model was trained with). A box much taller than wide is vertical text and is
 * turned on its side first, as PaddleOCR does. */
static void rec_fill(const uint32_t *px, int stride, const Box *bx, bool turn, float *t, int W, int rw, int *xi)
{
    int cw = bx->r - bx->l, ch = bx->b - bx->t;
    int sw = turn ? ch : cw, sh = turn ? cw : ch; /* the source as the recognizer sees it */
    int *xw = xi + rw;
    resize_map(rw, sw, xi, xw);
    int RH = G.rec_h;
    size_t plane = (size_t)W * RH;
    float step = (float)sh / (float)RH;
    const uint32_t *o = px + (size_t)bx->t * stride + bx->l;
    for (int y = 0; y < RH; y++) {
        float s = ((float)y + 0.5f) * step - 0.5f;
        if (s < 0)
            s = 0;
        int y0 = MINI((int)s, sh - 1), y1 = MINI(y0 + 1, sh - 1), wy = y0 + 1 < sh ? (int)((s - (float)(int)s) * 256.0f) : 0;
        float *b = t + (size_t)y * W, *g = b + plane, *r = g + plane;
        if (!turn) {
            const uint32_t *a = o + (size_t)y0 * stride, *c = o + (size_t)y1 * stride;
            for (int x = 0; x < rw; x++) {
                int x0 = xi[x], x1 = MINI(x0 + 1, sw - 1);
                put3(sample(a[x0], a[x1], c[x0], c[x1], xw[x], wy), b + x, g + x, r + x);
            }
        } else { /* turned a quarter left: (x, y) of the turned line is (cw - 1 - y, x) of the box */
            const uint32_t *a = o + (cw - 1 - y0), *c = o + (cw - 1 - y1);
            for (int x = 0; x < rw; x++) {
                size_t r0 = (size_t)xi[x] * stride, r1 = (size_t)MINI(xi[x] + 1, sw - 1) * stride;
                put3(sample(a[r0], a[r1], c[r0], c[r1], xw[x], wy), b + x, g + x, r + x);
            }
        }
        if (rw < W) {
            memset(b + rw, 0, (size_t)(W - rw) * sizeof(float));
            memset(g + rw, 0, (size_t)(W - rw) * sizeof(float));
            memset(r + rw, 0, (size_t)(W - rw) * sizeof(float));
        }
    }
}

/* one UTF-8 character list entry appended as UTF-16 */
static void add_utf8(Str *s, const char *u, int len)
{
    for (int i = 0; i < len;) {
        unsigned c = (unsigned char)u[i++], more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more)
            c &= 0x3Fu >> more;
        for (; more && i < len; more--)
            c = c << 6 | ((unsigned char)u[i++] & 0x3F);
        WCHAR w[2] = {(WCHAR)c, 0};
        if (c >= 0x10000) {
            c -= 0x10000;
            w[0] = (WCHAR)(0xD800 + (c >> 10));
            w[1] = (WCHAR)(0xDC00 + (c & 0x3FF));
        }
        str_addn(s, w, w[1] ? 2 : 1);
    }
}

/* The recognizer's answer for one line (T steps of C probabilities) as text: the best
 * character of every step, without repeats and blanks. Returns the mean probability
 * of the characters kept, 0 when there are none. */
/* e^x for x <= 0 as 2^x, to about a tenth of a percent: there is no C runtime to ask */
static float exp_neg(float x)
{
    if (x < -80.0f)
        return 0;
    x *= 1.4426950f;
    int i = (int)x; /* its floor */
    if ((float)i > x)
        i--;
    float f = x - (float)i; /* 0..1 */
    float m = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
    union {
        float f;
        int32_t i;
    } u = {m};
    if (i < -120)
        return 0;
    u.i += (int32_t)((uint32_t)i << 23); /* the exponent */
    return u.f;
}

/* what the best of C logits would be as a probability: the one a softmax gives it */
static float top_probability(const float *l, int C, float best)
{
    float sum = 0;
    for (int i = 0; i < C; i++)
        sum += exp_neg(l[i] - best);
    return 1.0f / sum;
}

static float decode(const float *p, int T, int C, Str *s)
{
    int last = 0, kept = 0;
    float sum = 0, one = 0;
    for (int i = 0; i < C; i++)
        one += p[i];
    bool probabilities = !G.vie && one > 0.99f && one < 1.01f; /* VieSVTR gives logits */
    for (int t = 0; t < T; t++, p += C) {
        float best;
        int at = argmax(p, C, probabilities, &best);
        if (at && at != last) { /* 0 is the blank */
            if (at <= G.ndict)
                add_utf8(s, G.dict + G.dict_at[at - 1], G.dict_at[at] - G.dict_at[at - 1] - 1);
            else
                str_addn(s, L" ", 1); /* the entry after the list is the space */
            sum += G.vie ? top_probability(p, C, best) : best;
            kept++;
        }
        last = at;
    }
    return kept ? sum / (float)kept : 0;
}

/* The lines are read by several threads at once, each taking the next line off a
 * shared counter and running the recognizer for it itself. */
typedef struct {
    const uint32_t *px;
    int stride, nbox;
    const Box *box;
    OcrLine *slot; /* one per box; .row holds the reading thread until the texts are merged */
    Scratch scratch[REC_WORKERS];
    volatile LONG next, worker;
    bool good;
} Reading;

static bool read_one(Reading *j, int at, int who)
{
    const Box *b = &j->box[at];
    Scratch *sc = &j->scratch[who];
    int cw = b->r - b->l, ch = b->b - b->t;
    bool turn = ch * 2 >= cw * 3;
    int RH = G.rec_h;
    float want = (float)RH * (turn ? (float)ch / (float)cw : (float)cw / (float)ch);
    int rw = MAXI(1, (int)want + ((float)(int)want < want)), W = MAXI(G.vie ? VIE_MIN_W : REC_MIN_W, rw);
    if (G.vie) /* its width is the line's own, up to a multiple of 4, and the line is squeezed when longer than it reads */
        rw = W = CLAMPI((rw + 3) & ~3, VIE_MIN_W, VIE_MAX_W);
    size_t need = (size_t)W * RH * 3;
    if (need > sc->tcap) {
        mem_free(sc->t);
        sc->t = raw_alloc((sc->tcap = need + need / 2) * sizeof(float));
    }
    if (rw * 2 > sc->mapcap) {
        mem_free(sc->map);
        sc->map = raw_alloc((size_t)(sc->mapcap = rw * 3) * sizeof(int));
    }
    if (!sc->t || !sc->map) {
        sc->tcap = 0, sc->mapcap = 0;
        return fail(L"out of memory");
    }
    rec_fill(j->px, j->stride, b, turn, sc->t, W, rw, sc->map);
    OrtValue *res = NULL;
    float *p = NULL;
    int64_t shape[4] = {1, 3, RH, W}, dims[3];
    if (!run(&G.rec, sc->t, shape, &res, &p, dims, 3))
        return false;
    OcrLine *ln = &j->slot[at];
    int steps = MINI((int)dims[1], (int)((int64_t)rw * dims[1] / W) + 2); /* the rest is padding */
    ln->text = sc->chars.n;
    ln->score = decode(p, steps, (int)dims[2], &sc->chars);
    ln->len = sc->chars.n - ln->text;
    ln->row = who;
    ln->rc = (RECT){b->l, b->t, b->r, b->b};
    G.o.ReleaseValue(res);
    return true;
}

static DWORD WINAPI read_thread(void *arg)
{
    Reading *j = arg;
    int who = (int)InterlockedIncrement(&j->worker) - 1;
    for (;;) {
        int at = (int)InterlockedIncrement(&j->next) - 1;
        if (at >= j->nbox || !j->good)
            break;
        if (!read_one(j, at, who))
            j->good = false;
    }
    return 0;
}

static bool read_lines(const uint32_t *px, int stride, const Box *box, int nbox, OcrOut *out)
{
    Reading *j = mem_alloc(sizeof *j);
    out->line = mem_alloc((size_t)nbox * sizeof(OcrLine));
    if (j)
        j->slot = mem_alloc((size_t)nbox * sizeof(OcrLine));
    if (!j || !j->slot || !out->line) {
        if (j)
            mem_free(j->slot);
        mem_free(j);
        return fail(L"out of memory");
    }
    j->px = px, j->stride = stride, j->nbox = nbox, j->box = box, j->good = true;
    HANDLE th[REC_WORKERS];
    int nth = 0, want = MINI(G.workers, nbox) - 1;
    for (; nth < want; nth++)
        if (!(th[nth] = CreateThread(NULL, 0, read_thread, j, 0, NULL)))
            break;
    read_thread(j); /* this thread reads too */
    if (nth) {
        WaitForMultipleObjects((DWORD)nth, th, TRUE, INFINITE);
        for (int i = 0; i < nth; i++)
            CloseHandle(th[i]);
    }
    for (int i = 0; i < nbox && j->good; i++) { /* the lines that are sure enough, into one text */
        const OcrLine *ln = &j->slot[i];
        if (ln->score < TEXT_SCORE || !ln->len)
            continue;
        OcrLine *o = &out->line[out->n++];
        *o = *ln;
        o->text = out->chars.n;
        str_addn(&out->chars, j->scratch[ln->row].chars.p + ln->text, ln->len);
        if (out->chars.n != o->text + o->len)
            j->good = fail(L"out of memory");
    }
    bool good = j->good;
    for (int i = 0; i < REC_WORKERS; i++) {
        mem_free(j->scratch[i].t);
        mem_free(j->scratch[i].map);
        str_free(&j->scratch[i].chars);
    }
    mem_free(j->slot);
    mem_free(j);
    return good;
}

/* insertion sort of indices: stable, and the lists are short or nearly sorted */
static void sort_by(int *idx, int n, const float *key)
{
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i;
        for (; j > 0 && key[idx[j - 1]] > key[v]; j--)
            idx[j] = idx[j - 1];
        idx[j] = v;
    }
}

/* Reading order: top to bottom; lines that overlap by half their height are one row
 * and go left to right. Without the memory for it, false and the lines freed: their
 * rows would still be the reading threads'. */
static bool arrange(OcrOut *o)
{
    int n = o->n;
    if (n < 2)
        return true;
    OcrLine *sorted = mem_alloc((size_t)n * (sizeof(OcrLine) + sizeof(int) + sizeof(float)));
    if (sorted) {
        int *idx = (int *)(sorted + n);
        float *key = (float *)(idx + n);
        for (int i = 0; i < n; i++) {
            idx[i] = i;
            key[i] = (float)(o->line[i].rc.top + o->line[i].rc.bottom);
        }
        sort_by(idx, n, key);
        int row = 0;
        for (int i = 0; i < n;) {
            const RECT *a = &o->line[idx[i]].rc;
            int top = a->top, bottom = a->bottom, j = i + 1;
            for (; j < n; j++) {
                const RECT *b = &o->line[idx[j]].rc;
                int over = MINI(bottom, b->bottom) - MAXI(top, b->top);
                if (over * 2 < MINI(bottom - top, b->bottom - b->top))
                    break;
            }
            for (int k = i; k < j; k++)
                key[idx[k]] = (float)o->line[idx[k]].rc.left;
            sort_by(idx + i, j - i, key);
            for (int k = i; k < j; k++) {
                sorted[k] = o->line[idx[k]];
                sorted[k].row = row;
            }
            row++;
            i = j;
        }
        memcpy(o->line, sorted, (size_t)n * sizeof(OcrLine));
        mem_free(sorted);
        return true;
    }
    ocr_free(o);
    return fail(L"out of memory");
}

/* The module, loaded. One whose DLL does not load for want of its runtime files (error 126:
 * the computer has none either, 1114: an old one) is completed and loaded again: once, and
 * here, not in ocr_load, which the helper also calls before there is anything to read. */
static bool ready(void)
{
    static bool mended;
    if (ocr_load(NULL, 0))
        return true;
    if (G.dll || mended)
        return false;
    mended = true;
    return ocr_mend() && ocr_load(NULL, 0);
}

/* one picture, looked at as a whole; its lines as they were found, not yet in reading order */
static bool run_band(const uint32_t *px, int stride, int w, int h, float screen, OcrOut *out)
{
    memset(out, 0, sizeof *out);
    if (!ready())
        return false;
    if (w < 4 || h < 4)
        return true;
    double t0 = now_ms(), t1 = t0;
    DetIn d;
    if (!det_input(px, stride, w, h, det_scale(w, h, screen), &d))
        return false;
    PROF(0, t1);
    t1 = now_ms();
    OrtValue *res = NULL;
    float *p = NULL;
    int64_t shape[4] = {1, 3, d.H, d.W}, dims[4];
    Box *box = raw_alloc((size_t)MAX_BOXES * sizeof(Box));
    bool good = box && run(&G.det, d.t, shape, &res, &p, dims, 4);
    if (good && (dims[2] != d.H || dims[3] != d.W))
        good = fail(L"the detection model answered in an unexpected size");
    PROF(1, t1);
    t1 = now_ms();
    int nbox = good ? det_boxes(p, &d, w, h, box) : 0;
    PROF(2, t1);
    if (res)
        G.o.ReleaseValue(res);
    mem_free(d.t);
    out->ms_find = now_ms() - t0;
    if (good && nbox)
        good = read_lines(px, stride, box, nbox, out);
    mem_free(box);
    if (!good)
        ocr_free(out);
    out->ms_read = now_ms() - t0 - out->ms_find;
    return good;
}

/* A picture much taller than a screen (a long screenshot) would have to be shrunk until
 * its text is too small to find. It is read in bands instead, each about a screen's
 * worth of pixels and overlapping the next by BAND_LAP rows: a line near a seam is whole
 * in one of the two bands, and is taken from the band whose inner part holds its middle. */
#define BAND_AREA 1500000
#define BAND_LAP 160

bool ocr_run(const uint32_t *px, int stride, int w, int h, float screen, OcrOut *out)
{
    int band = CLAMPI(BAND_AREA / MAXI(w, 1), 512, 2400);
    if (h <= band + BAND_LAP)
        return run_band(px, stride, w, h, screen, out) && arrange(out);
    memset(out, 0, sizeof *out);
    int cap = 0;
    for (int y0 = 0;; y0 += band - BAND_LAP) {
        int y1 = MINI(y0 + band, h);
        bool last = y1 == h;
        OcrOut part;
        if (!run_band(px + (size_t)y0 * stride, stride, w, y1 - y0, screen, &part)) {
            ocr_free(out);
            return false;
        }
        int from = y0 ? y0 + BAND_LAP / 2 : 0, to = last ? h : y1 - BAND_LAP / 2; /* the rows this band answers for */
        for (int i = 0; i < part.n; i++) {
            OcrLine ln = part.line[i];
            int mid = y0 + (ln.rc.top + ln.rc.bottom) / 2;
            if (mid < from || mid >= to)
                continue;
            ln.rc.top += y0, ln.rc.bottom += y0;
            ln.text = out->chars.n;
            str_addn(&out->chars, part.chars.p + part.line[i].text, ln.len);
            OcrLine *more = out->line;
            if (out->chars.n != ln.text + ln.len)
                more = NULL;
            else if (out->n == cap)
                more = mem_grow(out->line, (size_t)(cap = cap ? cap * 2 : 256) * sizeof(OcrLine));
            if (!more) {
                ocr_free(&part);
                ocr_free(out);
                return fail(L"out of memory");
            }
            out->line = more;
            out->line[out->n++] = ln;
        }
        out->ms_find += part.ms_find, out->ms_read += part.ms_read;
        ocr_free(&part);
        if (last)
            break;
    }
    return arrange(out);
}

void ocr_text(const OcrOut *o, WCHAR between, Str *s)
{
    for (int i = 0; i < o->n; i++) {
        const OcrLine *ln = &o->line[i];
        if (i)
            ln->row == o->line[i - 1].row ? str_addn(s, &between, 1) : str_addn(s, L"\r\n", 2);
        str_addn(s, o->chars.p + ln->text, ln->len);
    }
}

void ocr_free(OcrOut *o)
{
    mem_free(o->line);
    str_free(&o->chars);
    o->line = NULL;
    o->n = 0;
}
