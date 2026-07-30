/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "util.h"
#include "error.h"

/* Cleared once EGL owns the framebuffer.
 *
 * consoleInit() maps the display for its own software renderer. Calling it
 * while an EGL surface is live leaves the console with no framebuffer, and the
 * first character printed faults inside ConsoleSwRenderer_drawChar -- so a
 * clean "here is what went wrong" screen turns into a data abort at address 0,
 * with the actual reason still sitting unflushed in the log buffer. */
static int s_console_usable = 1;

void fatal_error_no_console(void) { s_console_usable = 0; }

void fatal_error(const char *fmt, ...) {
  PadState pad;

  /* Before anything else: get the log onto the card. Whatever explains this
   * failure was almost certainly written in the last few lines, and after
   * startup the log flushes in 32-line batches -- so without this the reason
   * dies with the process. This is exactly how the first abort() was lost. */
  {
    va_list l;
    va_start(l, fmt);
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, l);
    va_end(l);
    debugPrintf("FATAL: %s\n", msg);
    debugLogFlush();
  }

  if (!s_console_usable) {
    /* No console available. The log has the reason; leave quietly rather than
     * faulting inside the error handler. */
    debugPrintf("fatal_error: graphics are in use, exiting without a console\n");
    debugLogFlush();
    exit(1);
  }

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  consoleInit(NULL);

  va_list list;
  va_start(list, fmt);
  vprintf(fmt, list);
  va_end(list);

  printf("\n\nPress A to exit.");

  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 keys = padGetButtonsDown(&pad);
    if (keys & HidNpadButton_A) break;
  }

  consoleExit(NULL);
  exit(1);
}
