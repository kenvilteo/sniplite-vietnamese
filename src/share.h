/* Uploading a capture to the user's own server (share.c, server/worker.js). */
#pragma once
#include "common.h"

#define WM_APP_UPLOAD_DONE (WM_APP + 9) /* to the daemon window: lParam = the outcome, for upload_done */

/* Called by finish (ov_show.S) on the daemon's thread: the w x h pixels become a PNG
 * and go to <upload_url>/upload on a thread of its own. Says so if not set up. */
void upload_start(const uint32_t *px, int w, int h, HWND daemon);
/* WM_APP_UPLOAD_DONE: copy the link, show the outcome under the mouse. */
void upload_done(HWND daemon, LPARAM lp);
/* The server's page of uploaded images, in the browser. */
void upload_open_gallery(void);
