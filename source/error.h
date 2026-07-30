/* error.h -- error handler
 *
 * Copyright (C) 2021 fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ERROR_H__
#define __ERROR_H__

/* Tell the error handler it may no longer use the console -- call once the
 * display has been handed to EGL. */
void fatal_error_no_console(void);

void fatal_error(const char *fmt, ...) __attribute__((noreturn));

#endif
