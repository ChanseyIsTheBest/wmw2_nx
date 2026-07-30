/* wmw2_jni.h -- answers for the Java methods libwalaber.so calls back into
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __WMW2_JNI_H__
#define __WMW2_JNI_H__

#include <stdarg.h>

/* Consulted by jni_fake.c before it falls back to a logged default.
 * Each returns non-zero (or non-NULL) if it handled the call.
 *
 * Note the void hook takes the argument list. WMW1's did not: none of its void
 * upcalls carried a payload the port had to read. WMW2's do -- jniPlayMovie
 * needs the movie path, jniMigsRequestStoreGetSingleStoreItem needs the sku,
 * and in both cases the answer posted back depends on the argument. */
int         wmw2_jni_numeric(const char *name, const char *sig, long *out);
int         wmw2_jni_float  (const char *name, const char *sig, float *out);
const char *wmw2_jni_string (const char *name, const char *sig);
int         wmw2_jni_void   (const char *name, const char *sig, va_list va);

/* Set when the engine asks the activity to finish -- main.c exits the loop. */
extern volatile int wmw2_quit_requested;

/* Raised by startTextInput()/stopTextInput(); main.c services it with the
 * Switch software keyboard. */
extern volatile int wmw2_text_input_requested;

#endif
