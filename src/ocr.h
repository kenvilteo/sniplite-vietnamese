/* Text recognition (OCR). An optional module: it needs the "ocr" folder next to the
 * program, holding ONNX Runtime (onnxruntime.dll) and two PP-OCR models (*det*.onnx
 * finds the lines of text, *rec*.onnx reads them). Nothing of it is loaded until the
 * first ocr_load(). */
#pragma once
#include "common.h"

typedef struct {
    RECT rc;       /* where the line is in the picture */
    float score;   /* how sure the model is, 0..1 */
    int text, len; /* its characters: chars.p[text .. text + len) */
    int row;       /* lines side by side share a row; rows count from the top */
} OcrLine;

typedef struct {
    OcrLine *line; /* in reading order */
    int n;
    Str chars;
    double ms_find, ms_read; /* finding the lines, reading them */
} OcrOut;

bool ocr_installed(void); /* are the module's files there */
void ocr_dir(WCHAR *dir, int cap); /* the module's folder: "ocr" next to the program */

/* ocr_get.c: download the module into that folder, checked file by file. progress
 * gets 0..100; *cancel set from another thread gives up. Blocking. */
enum { OCR_GET_OK, OCR_GET_NET, OCR_GET_CHECK, OCR_GET_WRITE, OCR_GET_CANCEL };
int ocr_get(volatile LONG *cancel, void (*progress)(int percent, void *ctx), void *ctx);
bool ocr_remove(void); /* false when a file is in use */
/* The program may carry the module itself (the portable one with OCR). ocr_unpack writes
 * it into that folder, checked the same way; ocr_load does this when the folder is missing. */
bool ocr_packed(void);
bool ocr_unpack(void);
/* A module without the Visual C++ runtime files of its own gets them: out of the program's
 * own file, or downloaded when this is an installed copy. False when they are there or
 * cannot be had. Blocking. */
bool ocr_mend(void);
int ocr_size_mb(void);
/* dir: the module's folder as a full path, NULL for the "ocr" folder next to the program.
 * threads: 0 lets the module choose. Loading again is free. */
bool ocr_load(const WCHAR *dir, int threads);
/* screen: the scaling of the screen the picture was taken from (1 = 100%, 2 = 200%);
 * the text of a scaled-up screen is big, so the lines may be looked for in a smaller
 * copy, which is faster. They are always read from the picture as given. */
bool ocr_run(const uint32_t *px, int stride, int w, int h, float screen, OcrOut *out);

/* ocr_ui.c, the background program's side: what the user asks for. All of it is done
 * by a helper process (sniplite.exe --ocr) that also owns the windows. */
extern uint8_t ocr_panel_on; /* the text panel shows, or a text capture waits for its selection: refresh calls ocr_follow */
extern uint8_t ocr_there;    /* the module is installed: the text buttons and menu entries show */
void ocr_refresh(void);      /* look again whether it is (at start, after the preferences) */
void ocr_capture(bool copy); /* a capture for its text, from a hotkey or the command line */
void ocr_then(int mode);     /* WM_APP_OCR_THEN: its selection is made */
void ocr_warm(void);   /* about to be asked: have the helper and the module ready */
void ocr_panel(void);  /* the panel with the selection's text, beside it; again hides it */
void ocr_follow(void); /* the selection may have changed: read it again */
void ocr_hide(void);   /* the capture ends or does something else */
void ocr_copy(void);   /* the selection's text to the clipboard */
/* The text of any picture. beside: where on the screen what it shows is. With an owner
 * window the text comes in the panel, beside that place and above the owner; without,
 * it goes to the clipboard. False when the module is missing (the preferences open) or the
 * picture could not be handed over. */
bool ocr_picture(const uint32_t *px, int w, int h, RECT beside, HWND owner);
bool ocr_picture_shown(void); /* false once that panel was closed from its own side, or its helper left before showing it */
void ocr_picture_hide(void);
int ocr_helper(int argc, WCHAR **argv); /* body of `sniplite.exe --ocr` and `--ocr-file` */
/* For the helper of a quick capture, and for "also save" when no helper started: the notice in
 * the lower right corner of the monitor with that point, and the wait until it has faded. */
void notice_corner(const WCHAR *line, const WCHAR *path, POINT on_monitor, int stay_ms);
/* A line (and a second, smaller one, or NULL) under a point, for stay_ms: used by share.c. */
void notice_near(const WCHAR *line, const WCHAR *more, POINT at, int stay_ms);
void notice_wait(void);
/* The text: rows separated by CR LF; lines of one row by `between` (a space, or a
 * tab for pasting into a spreadsheet). */
void ocr_text(const OcrOut *o, WCHAR between, Str *s);
/* ocr_table.c: the lines as a table, when they stand in columns: the cells of a row
 * separated by tabs, rows by CR LF. px is the picture they were read from (its rules
 * tell the rows of a grid apart), or NULL. False, and nothing in s, when it is no table. */
bool ocr_table(const OcrOut *o, const uint32_t *px, int stride, int w, int h, Str *s);
void ocr_free(OcrOut *o);
const WCHAR *ocr_error(void); /* why the last ocr_load or ocr_run failed */
