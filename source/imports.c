/* imports.c -- import symbol table for Where's My Water? 2
 *              (libwalaber.so, com.disney.wheresmywater2_goo)
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen   (loader/shims this derives from)
 * WMW port: import table + glue.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Two modules are loaded: libfmodex.so first, then libwalaber.so. The engine's 45
 * FMOD imports (FMOD_System_Create, FMOD_Memory_GetStats and the mangled
 * FMOD::System / FMOD::Sound / FMOD::Channel / FMOD::ChannelGroup / FMOD::DSP
 * methods) are deliberately NOT in this table: so_resolve consults the table
 * first and then walks every already-loaded module's exports, so they bind
 * straight to the real libfmodex sitting in so_list. That is why libfmodex must
 * be loaded and relocated before libwalaber is resolved.
 *
 * Counts for this build:
 *   libwalaber.so          356 undefined
 *   libfmodex.so            82 undefined
 *   needing this table     337  (after sibling-module resolution)
 *   carried over from WMW1 274
 *   new for WMW2            63
 *
 * The 26 entries WMW1 needed and WMW2 does not are left in place: so_resolve
 * consults the table by name and simply never asks for them.
 *
 * Everything else comes from here:
 *   - GLES 1.1 fixed-function, flat pass-through to mesa. The engine logs
 *     "Preparing ES 1.1" at init and uses GL_OES_framebuffer_object for
 *     render-to-texture plus GL_OES_mapbuffer for dynamic vertex buffers.
 *   - zlib (the engine links Android's libz for its archive reader).
 *   - a libc subset, shimmed only where bionic and newlib actually disagree.
 *   - liblog, routed to the port's debug log.
 *
 * The table is generated from the game library's own dynamic symbol table;
 * re-run tools/gen_imports.py after a game update to diff the surface.
 *
 * Everything below the "--- WMW2 additions ---" banner is new relative to the
 * WMW1 port. The single largest block is the locale-aware xxx_l family, which
 * is the signature of a statically-linked libc++ instantiating std::locale.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <wctype.h>
#include <wchar.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <zlib.h>
#include <dirent.h>
#include <wchar.h>
#include <wctype.h>

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <EGL/egl.h>

#include "so_util.h"
#include "libc_shim.h"
#include "wmw_shims.h"
#include "util.h"
#include "config.h"
#include "wmw_tate.h"
#include "wmw_paths.h"
#include "error.h"

extern uintptr_t __cxa_atexit;
extern uintptr_t __cxa_finalize;
extern uintptr_t __stack_chk_fail;
extern int *__errno(void);

/* ------------------------------------------------------------------------ */
/* small local shims                                                         */
/* ------------------------------------------------------------------------ */

static void ret0v(void) { }   /* ret0()/retm1() come from util.h */

/* liblog -> debug.log ----------------------------------------------------- */
static int android_log_vprint_fake(int prio, const char *tag,
                                   const char *fmt, va_list ap) {
  (void)prio;
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  debugPrintf("[%s] %s\n", tag ? tag : "wmw", buf);
  return 0;
}

static int android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = android_log_vprint_fake(prio, tag, fmt, ap);
  va_end(ap);
  return r;
}

static int android_log_write_fake(int prio, const char *tag, const char *text) {
  (void)prio;
  debugPrintf("[%s] %s\n", tag ? tag : "wmw", text ? text : "");
  return 0;
}

/* misc libc gaps ---------------------------------------------------------- */
static void perror_fake(const char *s) {
  debugPrintf("perror: %s: %s\n", s ? s : "", strerror(errno));
}

/* getpwuid / getrusage return-by-pointer types must use BIONIC's layout, not
 * newlib's -- the game was compiled against bionic and will read the fields at
 * bionic's offsets. Declaring them here also avoids depending on <pwd.h> and
 * <sys/resource.h>, which devkitA64's newlib does not reliably provide. */

struct bionic_passwd {
  char *pw_name;
  char *pw_passwd;
  uint32_t pw_uid;
  uint32_t pw_gid;
  char *pw_gecos;
  char *pw_dir;
  char *pw_shell;
};

static void *getpwuid_fake(uint32_t uid) {
  (void)uid;
  static struct bionic_passwd pw;
  static char name[]  = "switch";
  static char empty[] = "";
  static char dir[]   = "/";
  memset(&pw, 0, sizeof(pw));
  pw.pw_name   = name;
  pw.pw_passwd = empty;
  pw.pw_gecos  = empty;
  pw.pw_dir    = dir;
  pw.pw_shell  = empty;
  return &pw;
}

/* bionic's struct rusage on LP64: two struct timevals (2 x 16 bytes) followed
 * by 14 longs = 144 bytes. The caller owns the storage, so zeroing exactly that
 * much is safe and keeps callers that ignore the return value from reading
 * uninitialised memory. */
#define BIONIC_RUSAGE_SIZE 144

static int getrusage_fake(int who, void *ru) {
  (void)who;
  if (ru) memset(ru, 0, BIONIC_RUSAGE_SIZE);
  return 0;
}

/* The engine calls exit()/abort() on a few unrecoverable asset errors. Route
 * them through the port's fatal-error screen instead of killing the process
 * silently, so the user sees which file is missing. */
static void exit_fake(int code) {
  debugPrintf("engine called exit(%d)\n", code);
  debugLogSetEager(1);   /* whatever comes next is the last thing we will see */
  debugLogFlush();
  fatal_error("The game exited (code %d).\nCheck debug.log.", code);
}

static void abort_fake(void) {
  debugPrintf("engine called abort()\n");
  debugLogSetEager(1);
  debugLogFlush();
  fatal_error("The game aborted.\nCheck debug.log.");
}

static void *signal_fake(int sig, void *handler) { (void)sig; (void)handler; return NULL; }

static char *dlerror_fake(void) { return NULL; }


/* ------------------------------------------------------------------------ */
/* GL_OES_framebuffer_object / GL_OES_mapbuffer                              */
/* ------------------------------------------------------------------------ */
/*
 * libwmw.so imports seven *OES entry points: the five GL_OES_framebuffer_object
 * calls it uses for render-to-texture, and glMapBufferOES/glUnmapBufferOES for
 * dynamic vertex data.
 *
 * Two things make these awkward to bind statically:
 *
 *   1. The Khronos GLES1 headers only declare extension prototypes when
 *      GL_GLEXT_PROTOTYPES is defined, so `&glBindFramebufferOES` does not
 *      compile out of the box.
 *   2. Even with the declaration, whether libGLESv1_CM actually *exports* the
 *      suffixed names depends on how mesa was configured.
 *
 * So the table points at these stable wrappers instead, and update_imports()
 * binds them at runtime through eglGetProcAddress -- which is why egl_init()
 * runs before load_two_modules() in main.c: a context must be current.
 *
 * Each name is tried suffixed first, then unsuffixed. Mesa aliases the OES
 * entry points onto the same dispatch slots as the core ones, so the fallback
 * is the same function, not an approximation.
 *
 * glMapBufferOES has a real emulation below for the case where neither name
 * resolves, because the engine genuinely needs it.
 */

/* Enums live outside the GL_GLEXT_PROTOTYPES guard, but define them defensively
 * in case a slimmed-down glext.h omits the extension block entirely. */
#ifndef GL_FRAMEBUFFER_COMPLETE_OES
#define GL_FRAMEBUFFER_COMPLETE_OES 0x8CD5
#endif
#ifndef GL_WRITE_ONLY_OES
#define GL_WRITE_ONLY_OES 0x88B9
#endif

static void      (*p_BindFramebufferOES)(GLenum, GLuint);
static GLenum    (*p_CheckFramebufferStatusOES)(GLenum);
static void      (*p_DeleteFramebuffersOES)(GLsizei, const GLuint *);
static void      (*p_FramebufferTexture2DOES)(GLenum, GLenum, GLenum, GLuint, GLint);
static void      (*p_GenFramebuffersOES)(GLsizei, GLuint *);
static void     *(*p_MapBufferOES)(GLenum, GLenum);
static GLboolean (*p_UnmapBufferOES)(GLenum);

/* --- emulated GL_OES_mapbuffer -------------------------------------------
 * Hand back a shadow allocation sized to the bound buffer, and push it with
 * glBufferSubData on unmap. Not zero-copy, but correct for the WRITE_ONLY
 * usage the engine has (it refills the whole range each frame). Only one
 * mapping per target may be live at a time, which is what GL requires anyway.
 */
typedef struct { GLenum target; void *shadow; GLint size; } MapSlot;
static MapSlot s_map_slots[2] = { { GL_ARRAY_BUFFER, NULL, 0 },
                                  { GL_ELEMENT_ARRAY_BUFFER, NULL, 0 } };

static MapSlot *map_slot_for(GLenum target) {
  for (unsigned i = 0; i < sizeof(s_map_slots) / sizeof(*s_map_slots); i++)
    if (s_map_slots[i].target == target)
      return &s_map_slots[i];
  return NULL;
}

static void *emu_MapBufferOES(GLenum target, GLenum access) {
  (void)access;
  MapSlot *s = map_slot_for(target);
  if (!s) return NULL;
  GLint size = 0;
  glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
  if (size <= 0) return NULL;
  if (s->shadow && s->size < size) { free(s->shadow); s->shadow = NULL; }
  if (!s->shadow) {
    s->shadow = malloc((size_t)size);
    s->size = size;
  }
  return s->shadow;
}

static GLboolean emu_UnmapBufferOES(GLenum target) {
  MapSlot *s = map_slot_for(target);
  if (!s || !s->shadow) return GL_FALSE;
  glBufferSubData(target, 0, s->size, s->shadow);
  return GL_TRUE;
}

/* --- wrappers the import table points at ---------------------------------- */

static void w_glBindFramebufferOES(GLenum target, GLuint fb) {
  // The engine uses FBOs for its own render-to-texture work and binds 0 when
  // it is finished, meaning "back to the screen". While portrait presentation
  // is active the screen is our rotation target, not the window, so substitute
  // it. wmw_tate.c calls the driver directly and never comes through here, so
  // there is no recursion.
  if (fb == 0 && wmw_tate_active())
    fb = wmw_tate_fbo();
  if (p_BindFramebufferOES) p_BindFramebufferOES(target, fb);
}
static GLenum w_glCheckFramebufferStatusOES(GLenum target) {
  return p_CheckFramebufferStatusOES ? p_CheckFramebufferStatusOES(target)
                                     : GL_FRAMEBUFFER_COMPLETE_OES;
}
static void w_glDeleteFramebuffersOES(GLsizei n, const GLuint *fbs) {
  if (!p_DeleteFramebuffersOES || !fbs) return;

  // The engine creates and destroys its own framebuffers as screens come and
  // go. Because w_glBindFramebufferOES substitutes our rotation target for 0,
  // the engine can end up holding our id -- from glGetIntegerv, or simply by
  // having bound 0 earlier -- and delete it on the way out of a screen. That
  // destroys the texture everything is being drawn into, which shows up as the
  // display going wrong *after* returning to a menu rather than while in it.
  const GLuint ours = wmw_tate_active() ? (GLuint)wmw_tate_fbo() : 0;
  if (!ours) { p_DeleteFramebuffersOES(n, fbs); return; }

  GLuint filtered[32];
  GLsizei out = 0;
  for (GLsizei i = 0; i < n; i++) {
    if (fbs[i] == ours) {
      debugPrintf("gl: refused to delete the portrait render target (fbo %u)\n", ours);
      continue;
    }
    if (out < (GLsizei)(sizeof(filtered) / sizeof(*filtered)))
      filtered[out++] = fbs[i];
  }
  if (out > 0) p_DeleteFramebuffersOES(out, filtered);
}
static void w_glFramebufferTexture2DOES(GLenum target, GLenum att, GLenum textarget,
                                        GLuint tex, GLint level) {
  if (p_FramebufferTexture2DOES) p_FramebufferTexture2DOES(target, att, textarget, tex, level);
}
static void w_glGenFramebuffersOES(GLsizei n, GLuint *fbs) {
  if (p_GenFramebuffersOES) p_GenFramebuffersOES(n, fbs);
  else if (fbs) memset(fbs, 0, (size_t)n * sizeof(GLuint));
}
static void *w_glMapBufferOES(GLenum target, GLenum access) {
  return p_MapBufferOES ? p_MapBufferOES(target, access)
                        : emu_MapBufferOES(target, access);
}
static GLboolean w_glUnmapBufferOES(GLenum target) {
  return p_UnmapBufferOES ? p_UnmapBufferOES(target)
                          : emu_UnmapBufferOES(target);
}

#ifndef GL_FRAMEBUFFER_BINDING_OES
#define GL_FRAMEBUFFER_BINDING_OES 0x8CA6
#endif

/* The engine asks what framebuffer is bound, saves it, and restores it later.
 * Handing back our rotation target works for save/restore, but any code that
 * COMPARES the value against 0 to decide "am I drawing to the screen?" gets the
 * wrong answer. Report 0 for our target so that test behaves as it would on a
 * normal window. */
static void w_glGetIntegerv(GLenum pname, GLint *params) {
  glGetIntegerv(pname, params);
  if (pname == GL_FRAMEBUFFER_BINDING_OES && params &&
      wmw_tate_active() && (GLuint)*params == (GLuint)wmw_tate_fbo())
    *params = 0;
}

/* Try the suffixed name, then the core name. eglGetProcAddress already returns
 * a generic function pointer, so nothing here casts through void*. */
typedef void (*gl_fn)(void);

static gl_fn gl_proc(const char *oes, const char *core) {
  gl_fn p = eglGetProcAddress(oes);
  if (!p && core) p = eglGetProcAddress(core);
  debugPrintf("gl: %-30s -> %s\n", oes, p ? "ok" : "MISSING");
  return p;
}

/* ------------------------------------------------------------------------ */
/* the table                                                                 */
/* ------------------------------------------------------------------------ */


/* ======================= WMW2 additions: implementations ================= */

/* Declared elsewhere in the tree; listed here only so the table can name them. */
extern int  mkdir_fake(const char *path, unsigned int mode);
extern int  chdir_fake(const char *path);
extern int  readlink_fake(const char *path, char *buf, size_t bufsiz);
extern int  system_fake(const char *command);
extern int  lstat_fake(const char *path, struct bionic_stat *st);


/* ---------------------------------------------------------------------------
 * 1. The locale-aware (*_l) family
 *
 * libwalaber.so statically links libc++ and instantiates std::locale, which
 * drags in the whole xxx_l surface. WMW1 imported none of it.
 *
 * Careful here: libc_shim.h ALREADY DECLARES twenty of these, but libc_shim.c
 * only DEFINES five (strcoll_l, wcscoll_l, strtold_l, strtoll_l, strtoull_l).
 * The other fifteen were declared and never written, which was harmless on
 * WMW1 because nothing referenced them -- no table entry, no link error. Adding
 * the table entries makes them referenced, so they have to exist now.
 *
 * So this block:
 *   - defines the fifteen declared-but-missing ones, with EXACTLY the
 *     signatures libc_shim.h declares (int, not wint_t -- a mismatch here is a
 *     conflicting-types error, not a warning);
 *   - defines the six narrow-char ones that appear nowhere in the tree;
 *   - defines nothing that libc_shim.c already has.
 *
 * Every one ignores its locale_t. The engine only ever uses the "C" locale --
 * it reaches these through libc++'s num_put/num_get/collate facets, not because
 * it wants a locale -- so delegating to the plain C-locale function is the same
 * answer, not an approximation. It also sidesteps newlib's locale_t being a
 * different shape from bionic's.
 * ------------------------------------------------------------------------- */

/* The wide-character classifiers, towlower_l, towupper_l, strxfrm_l,
 * wcsxfrm_l and strftime_l are deliberately NOT defined here. libc_shim.c
 * already defines every one of them; the twelve classifiers come out of a
 * WRAP_ISW_L() macro, so their names never appear as literal text and a grep
 * for them finds nothing. Only the table entries below are new.
 *
 * (strcoll_l, wcscoll_l, strtold_l, strtoll_l and strtoull_l are likewise
 * already in libc_shim.c, written out longhand.) */

/* --- narrow-char variants: new to the tree, so these can stay local -------- */
static int isdigit_l_fake (int c, void *loc) { (void)loc; return isdigit(c); }
static int islower_l_fake (int c, void *loc) { (void)loc; return islower(c); }
static int isupper_l_fake (int c, void *loc) { (void)loc; return isupper(c); }
static int isxdigit_l_fake(int c, void *loc) { (void)loc; return isxdigit(c); }
static int tolower_l_fake (int c, void *loc) { (void)loc; return tolower(c); }
static int toupper_l_fake (int c, void *loc) { (void)loc; return toupper(c); }

/* strcoll_l, wcscoll_l, strtold_l, strtoll_l and strtoull_l are NOT here:
 * libc_shim.c defines them and the WMW1 half of the table already binds three
 * of them. Redefining any would shadow the real shim with a static. */

/* ---------------------------------------------------------------------------
 * 2. Positional I/O -- and the trap 4.2 warned about, in a new place
 *
 * pread/pwrite are new in WMW2 and they need exactly the same past-EOF
 * treatment as read(): Horizon fails a read that starts at or beyond the end
 * of the file with -1/EIO where POSIX guarantees 0. SQLite's unixRead uses
 * pread on some paths, so this is not hypothetical -- and the failure mode is
 * the same generic "disk I/O error" that cost so much time the first time.
 *
 * ssize_read_fake already encapsulates the size comparison; reuse it rather
 * than re-deriving it here.
 * ------------------------------------------------------------------------- */

/* Same atomicity requirement as pwrite_fake, and the same reason. The read
 * itself still goes through ssize_read_fake so the past-EOF correction in 6.2
 * applies -- SQLite probes past the end of a rollback journal on every commit,
 * and Horizon reports that as an error where POSIX reports zero. */
ssize_t pread_fake(int fd, void *buf, size_t count, off_t offset) {
  fd = wmw_real_fd(fd);
  wmw_file_lock();
  ssize_t r = -1;
  const off_t saved = lseek(fd, 0, SEEK_CUR);
  if (saved >= 0 && lseek(fd, offset, SEEK_SET) >= 0) {
    r = ssize_read_fake(fd, buf, count);
    lseek(fd, saved, SEEK_SET);
  }
  const int err = errno;
  wmw_file_unlock();
  if (r < 0) {
    wmw_io_trace_always("  db pread(fd=%d, %ld @%lld) -> FAILED\n",
                        fd, (long)count, (long long)offset);
    errno = err;
  }
  return r;
}

/* THE database write path, and for a long time an invisible one.
 *
 * libwalaber.so imports pread and pwrite but NOT write, so its bundled SQLite
 * does all of its I/O positionally. Everything the port had instrumented hung
 * off ssize_write_fake, which the engine therefore never calls -- so "no
 * db write() in the log" was structurally guaranteed and said nothing about
 * whether saves were happening. This is where they actually happen.
 *
 * Emulating pwrite with seek/write/seek also has to be ATOMIC with respect to
 * the file offset. Section 6.3: FMOD runs its own file and stream threads from
 * System::init onwards, so nothing is single-threaded once audio starts, and an
 * interleaved seek from another thread would land a database page at the wrong
 * offset -- silent corruption rather than a clean error. Hold the same lock the
 * rest of the port uses around the whole sequence. */
ssize_t pwrite_fake(int fd, const void *buf, size_t count, off_t offset) {
  fd = wmw_real_fd(fd);
  wmw_file_lock();
  ssize_t r = -1;
  const off_t saved = lseek(fd, 0, SEEK_CUR);
  if (saved >= 0 && lseek(fd, offset, SEEK_SET) >= 0) {
    r = write(fd, buf, count);
    lseek(fd, saved, SEEK_SET);
  }
  const int err = errno;
  wmw_file_unlock();

  wmw_io_trace_always("  db pwrite(fd=%d, %ld @%lld) -> %ld%s\n",
                      fd, (long)count, (long long)offset, (long)r,
                      (r < 0) ? "  <-- FAILED" : "");
  if (r < 0) errno = err;
  return r;
}

/* ---------------------------------------------------------------------------
 * 3. Directory enumeration -- new, and it must go through the path mapper
 *
 * WMW1 never scanned a directory; WMW2 does. Every path the engine hands in is
 * an Android path, so opendir has to be routed through the same resolver as
 * fopen or it will look for "/data/data/..." on the SD card and find nothing.
 *
 * Bionic's struct dirent differs from newlib's: d_ino is 8 bytes, then
 * d_off (8), d_reclen (2), d_type (1), d_name[256]. Returning newlib's layout
 * straight through will scramble d_name. wmw_shims.c already has the
 * stat-layout conversion machinery; this is the same problem one struct over.
 * ------------------------------------------------------------------------- */

/* libc_shim.c already provides readdir_fake(), including the bionic struct
 * dirent conversion -- it was written for WMW1 and then never bound to
 * anything, because WMW1's engine never enumerated a directory. WMW2's does, so
 * the three table entries below are new even though the hard part already
 * existed.
 *
 * That existing readdir_fake takes a REAL DIR*, so opendir_fake must return one
 * rather than a wrapper of its own. Getting this wrong links cleanly and then
 * misreads every directory entry, because readdir() would be handed a pointer
 * to the wrong struct. */

void *opendir_fake(const char *path) {
  char buf[WMW_PATH_MAX];
  return opendir(wmw_resolve(path, buf, sizeof(buf)));
}

int closedir_fake(void *dirp) {
  return dirp ? closedir((DIR *)dirp) : -1;
}

/* ---------------------------------------------------------------------------
 * 4. Filesystem metadata -- route through the mapper, then answer honestly
 *
 * lstat has no symlinks to follow here, so it is stat. The ownership and
 * permission calls have no meaning on a FAT SD card; answer success rather
 * than failure, because the engine treats a failed chmod on its own save
 * directory as a fatal storage error.
 * ------------------------------------------------------------------------- */

/* lstat_fake already exists in libc_shim.c -- nothing here is a symlink, so it
 * is stat with the bionic struct layout, and that code is shared. */

int chmod_fake(const char *path, mode_t m)  { (void)path; (void)m; return 0; }
int fchmod_fake(int fd, mode_t m)           { (void)fd; (void)m; return 0; }
int fchown_fake(int fd, uid_t u, gid_t g)   { (void)fd; (void)u; (void)g; return 0; }

/* mkdir_fake / chdir_fake / readlink_fake / symlink live in wmw_shims.c
 * already; only rmdir is new. */
int rmdir_fake(const char *path) {
  char buf[WMW_PATH_MAX];
  return rmdir(wmw_resolve(path, buf, sizeof(buf)));
}

int symlink_fake(const char *t, const char *l) { (void)t; (void)l; errno = EPERM; return -1; }

/* ---------------------------------------------------------------------------
 * 5. Process control -- there are no processes
 *
 * The engine calls system()/popen() from a diagnostics path (it shells out to
 * read device properties). Fail cleanly: a NULL FILE* from popen and a
 * non-zero status from system both put it on the "could not determine" branch,
 * which is the correct answer here.
 * ------------------------------------------------------------------------- */

void *popen_fake(const char *cmd, const char *mode)     { (void)cmd; (void)mode; return NULL; }
int   pclose_fake(void *f)                              { (void)f; return -1; }
int   raise_fake(int sig)                               { (void)sig; return 0; }
/* __register_atfork_fake lives in libc_shim.c already (declared there as taking
 * no arguments -- it never reads them, and on AArch64 an extra argument in a
 * register the callee ignores is harmless). Only the table entry is new. */
void *mremap_fake(void *old, size_t oldsz, size_t newsz, int flags, ...) {
  (void)old; (void)oldsz; (void)newsz; (void)flags;
  errno = ENOMEM; return (void *)-1;              /* force the realloc+copy path */
}
uid_t geteuid_fake(void) { return 0; }

/* ---------------------------------------------------------------------------
 * 6. stdio objects
 *
 * WMW2 imports stdin/stdout/stderr as OBJECTS in addition to bionic's __sF
 * array. They are the same three FILEs; point them at the existing fake_sF
 * slots so is_fake_file() recognises writes to any of them and fwrite_fake
 * absorbs the output into debug.log rather than handing newlib a FILE it has
 * never seen.
 *
 * These are data, not functions -- so_resolve binds them via R_AARCH64_ABS64
 * and the table entry must be the address OF the pointer, not the pointer.
 * ------------------------------------------------------------------------- */

extern uint8_t fake_sF[3][0x100];
void *fake_stdin  = &fake_sF[0];
void *fake_stdout = &fake_sF[1];
void *fake_stderr = &fake_sF[2];


DynLibFunction dynlib_functions[] = {

  // --- OpenGL ES 1.1 + GL_OES_framebuffer_object / GL_OES_mapbuffer (mesa) ---  // 58 entry points
  { "glActiveTexture", (uintptr_t)&glActiveTexture },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebufferOES", (uintptr_t)&w_glBindFramebufferOES },
  { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glBufferSubData", (uintptr_t)&glBufferSubData },
  { "glCheckFramebufferStatusOES", (uintptr_t)&w_glCheckFramebufferStatusOES },
  { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glClientActiveTexture", (uintptr_t)&glClientActiveTexture },
  { "glColor4f", (uintptr_t)&glColor4f },
  { "glColorMask", (uintptr_t)&glColorMask },
  { "glColorPointer", (uintptr_t)&glColorPointer },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffersOES", (uintptr_t)&w_glDeleteFramebuffersOES },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableClientState", (uintptr_t)&glDisableClientState },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableClientState", (uintptr_t)&glEnableClientState },
  { "glFinish", (uintptr_t)&glFinish },
  { "glFramebufferTexture2DOES", (uintptr_t)&w_glFramebufferTexture2DOES },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffersOES", (uintptr_t)&w_glGenFramebuffersOES },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetFloatv", (uintptr_t)&glGetFloatv },
  { "glGetIntegerv", (uintptr_t)&w_glGetIntegerv },
  { "glGetPointerv", (uintptr_t)&glGetPointerv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glGetTexEnvfv", (uintptr_t)&glGetTexEnvfv },
  { "glGetTexEnviv", (uintptr_t)&glGetTexEnviv },
  { "glLineWidth", (uintptr_t)&glLineWidth },
  { "glLoadIdentity", (uintptr_t)&glLoadIdentity },
  { "glLoadMatrixf", (uintptr_t)&glLoadMatrixf },
  { "glLogicOp", (uintptr_t)&glLogicOp },
  { "glMapBufferOES", (uintptr_t)&w_glMapBufferOES },
  { "glMatrixMode", (uintptr_t)&glMatrixMode },
  { "glOrthof", (uintptr_t)&glOrthof },
  { "glPopMatrix", (uintptr_t)&glPopMatrix },
  { "glPushMatrix", (uintptr_t)&glPushMatrix },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRotatef", (uintptr_t)&glRotatef },
  { "glScalef", (uintptr_t)&glScalef },
  { "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
  { "glTexEnvfv", (uintptr_t)&glTexEnvfv },
  { "glTexEnvi", (uintptr_t)&glTexEnvi },
  { "glTexImage2D", (uintptr_t)&glTexImage2D },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTranslatef", (uintptr_t)&glTranslatef },
  { "glUnmapBufferOES", (uintptr_t)&w_glUnmapBufferOES },
  { "glVertexPointer", (uintptr_t)&glVertexPointer },
  { "glViewport", (uintptr_t)&glViewport },
  // --- zlib (devkitPro -lz) ---
  { "crc32", (uintptr_t)&crc32 },
  { "deflate", (uintptr_t)&deflate },
  { "deflateEnd", (uintptr_t)&deflateEnd },
  { "deflateInit2_", (uintptr_t)&deflateInit2_ },
  { "get_crc_table", (uintptr_t)&get_crc_table },
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  // --- bionic-only / compiler runtime ---
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&__cxa_finalize },
  { "__errno", (uintptr_t)&__errno },
  { "__sF", (uintptr_t)&fake_sF },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  // --- android liblog -> debugPrintf ---
  { "__android_log_print", (uintptr_t)&android_log_print_fake },
  { "__android_log_vprint", (uintptr_t)&android_log_vprint_fake },
  { "__android_log_write", (uintptr_t)&android_log_write_fake },
  // --- dynamic linker (so_util + stubs) ---
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake },
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  // --- pthread (libnx pthread compat) ---
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait },
  { "pthread_create", (uintptr_t)&pthread_create },
  { "pthread_detach", (uintptr_t)&pthread_detach },
  { "pthread_equal", (uintptr_t)&pthread_equal },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock },
  { "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype },
  { "pthread_once", (uintptr_t)&pthread_once },
  { "pthread_self", (uintptr_t)&pthread_self },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  // --- libc / libm ---
  { "abort", (uintptr_t)&abort_fake },
  { "access", (uintptr_t)&access_fake },
  { "acosf", (uintptr_t)&acosf },
  { "atan2f", (uintptr_t)&atan2f },
  { "atanf", (uintptr_t)&atanf },
  { "atoi", (uintptr_t)&atoi },
  { "btowc", (uintptr_t)&btowc },
  { "calloc", (uintptr_t)&calloc },
  { "close", (uintptr_t)&close_fake },
  { "closelog", (uintptr_t)&ret0v },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "exit", (uintptr_t)&exit_fake },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fcntl", (uintptr_t)&fcntl_fake },
  { "feof", (uintptr_t)&feof_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fgetc", (uintptr_t)&fgetc_fake },
  { "fgets", (uintptr_t)&fgets_fake },
  { "fmod", (uintptr_t)&fmod },
  { "fopen", (uintptr_t)&fopen_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "free", (uintptr_t)&free },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko },
  { "fstat", (uintptr_t)&fstat_fake },
  { "fsync", (uintptr_t)&fsync_fake },
  { "ftell", (uintptr_t)&ftell_fake },
  { "ftello", (uintptr_t)&ftello },
  { "ftruncate", (uintptr_t)&ftruncate_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "getc", (uintptr_t)&getc_fake },
  { "getcwd", (uintptr_t)&getcwd_fake },
  { "getenv", (uintptr_t)&getenv_fake },
  { "getpid", (uintptr_t)&getpid },
  { "getpwuid", (uintptr_t)&getpwuid_fake },
  { "getrusage", (uintptr_t)&getrusage_fake },
  { "gettimeofday", (uintptr_t)&gettimeofday_fake },
  { "getuid", (uintptr_t)&getuid_fake },
  { "gmtime", (uintptr_t)&gmtime },
  { "isalnum", (uintptr_t)&isalnum },
  { "isalpha", (uintptr_t)&isalpha },
  { "isatty", (uintptr_t)&isatty_fake },
  { "islower", (uintptr_t)&islower },
  { "isprint", (uintptr_t)&isprint },
  { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper },
  { "iswalpha", (uintptr_t)&iswalpha },
  { "iswblank", (uintptr_t)&iswblank },
  { "iswcntrl", (uintptr_t)&iswcntrl },
  { "iswdigit", (uintptr_t)&iswdigit },
  { "iswlower", (uintptr_t)&iswlower },
  { "iswprint", (uintptr_t)&iswprint },
  { "iswpunct", (uintptr_t)&iswpunct },
  { "iswspace", (uintptr_t)&iswspace },
  { "iswupper", (uintptr_t)&iswupper },
  { "iswxdigit", (uintptr_t)&iswxdigit },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "ldexp", (uintptr_t)&ldexp },
  { "localeconv", (uintptr_t)&localeconv },
  { "localtime", (uintptr_t)&localtime },
  { "log", (uintptr_t)&log },
  { "log10", (uintptr_t)&log10 },
  { "lseek", (uintptr_t)&lseek_fake2 },
  { "malloc", (uintptr_t)&malloc },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "mktime", (uintptr_t)&mktime },
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "nan", (uintptr_t)&nan },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "open", (uintptr_t)&open_fake },
  { "openlog", (uintptr_t)&ret0v },
  { "perror", (uintptr_t)&perror_fake },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "pow", (uintptr_t)&pow },
  { "printf", (uintptr_t)&printf },
  { "putc", (uintptr_t)&putc },
  { "putchar", (uintptr_t)&putchar },
  { "puts", (uintptr_t)&puts },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "read", (uintptr_t)&ssize_read_fake },
  { "realloc", (uintptr_t)&realloc },
  { "remove", (uintptr_t)&remove_fake },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "setlocale", (uintptr_t)&setlocale },
  { "signal", (uintptr_t)&signal_fake },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "sleep", (uintptr_t)&sleep },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "sqrt", (uintptr_t)&sqrt },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "srand", (uintptr_t)&srand },
  { "sscanf", (uintptr_t)&sscanf },
  { "stat", (uintptr_t)&stat_fake },
  { "strcat", (uintptr_t)&strcat },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll", (uintptr_t)&strcoll },
  { "strcpy", (uintptr_t)&strcpy },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strftime", (uintptr_t)&strftime },
  { "strlen", (uintptr_t)&strlen },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "swprintf", (uintptr_t)&swprintf },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "syslog", (uintptr_t)&ret0v },
  { "time", (uintptr_t)&time },
  { "tolower", (uintptr_t)&tolower },
  { "toupper", (uintptr_t)&toupper },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "unlink", (uintptr_t)&unlink_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "utimes", (uintptr_t)&ret0 },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wctob", (uintptr_t)&wctob },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
  { "write", (uintptr_t)&ssize_write_fake },

  // --- additional imports pulled by libfmodex.so (not by libwmw.so) ---------
  // so_resolve() runs against this table for both modules, so FMOD's extra
  // surface has to be here too. See wmw_shims.c for why the sockets are stubs.
  { "_ZdlPv", (uintptr_t)&operator_delete_fake },
  { "__cxa_pure_virtual", (uintptr_t)&cxa_pure_virtual_fake },
  { "__FD_SET_chk", (uintptr_t)&fd_set_chk_fake },
  { "cbrtf", (uintptr_t)&cbrtf },
  { "expf", (uintptr_t)&expf },
  { "ldexpf", (uintptr_t)&ldexpf },
  { "log10f", (uintptr_t)&log10f },
  { "logf", (uintptr_t)&logf },
  { "lrintf", (uintptr_t)&lrintf },
  { "powf", (uintptr_t)&powf },
  { "tanf", (uintptr_t)&tanf },
  { "usleep", (uintptr_t)&usleep },
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "socket", (uintptr_t)&socket_fake },
  { "connect", (uintptr_t)&connect_fake },
  { "setsockopt", (uintptr_t)&setsockopt_fake },
  { "accept", (uintptr_t)&accept_fake },
  { "bind", (uintptr_t)&bind_fake },
  { "listen", (uintptr_t)&listen_fake },
  { "recv", (uintptr_t)&recv_fake },
  { "send", (uintptr_t)&send_fake },
  { "select", (uintptr_t)&select_fake },
  { "gethostbyname", (uintptr_t)&gethostbyname_fake },
  { "inet_addr", (uintptr_t)&inet_addr_fake },

  /* ============================ WMW2 additions ============================ */

  /* --- GLES 1.1: the only two additions --- */
  { "glNormalPointer",   (uintptr_t)&glNormalPointer },
  { "glTexSubImage2D",   (uintptr_t)&glTexSubImage2D },

  /* --- locale-aware character and string classification --- */
  { "isdigit_l",   (uintptr_t)&isdigit_l_fake },
  { "islower_l",   (uintptr_t)&islower_l_fake },
  { "isupper_l",   (uintptr_t)&isupper_l_fake },
  { "isxdigit_l",  (uintptr_t)&isxdigit_l_fake },
  { "tolower_l",   (uintptr_t)&tolower_l_fake },
  { "toupper_l",   (uintptr_t)&toupper_l_fake },
  { "iswalpha_l",  (uintptr_t)&iswalpha_l_fake },
  { "iswblank_l",  (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l",  (uintptr_t)&iswcntrl_l_fake },
  { "iswdigit_l",  (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l",  (uintptr_t)&iswlower_l_fake },
  { "iswprint_l",  (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l",  (uintptr_t)&iswpunct_l_fake },
  { "iswspace_l",  (uintptr_t)&iswspace_l_fake },
  { "iswupper_l",  (uintptr_t)&iswupper_l_fake },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "towlower_l",  (uintptr_t)&towlower_l_fake },
  { "towupper_l",  (uintptr_t)&towupper_l_fake },
  { "strcoll_l",   (uintptr_t)&strcoll_l_fake },
  { "strxfrm_l",   (uintptr_t)&strxfrm_l_fake },
  { "wcscoll_l",   (uintptr_t)&wcscoll_l_fake },
  { "wcsxfrm_l",   (uintptr_t)&wcsxfrm_l_fake },
  { "strftime_l",  (uintptr_t)&strftime_l_fake },

  /* --- positional I/O (EOF-corrected) --- */
  { "pread",  (uintptr_t)&pread_fake },
  { "pwrite", (uintptr_t)&pwrite_fake },

  /* --- directory enumeration --- */
  { "opendir",  (uintptr_t)&opendir_fake },
  { "readdir",  (uintptr_t)&readdir_fake },
  { "closedir", (uintptr_t)&closedir_fake },

  /* --- filesystem metadata --- */
  { "lstat",    (uintptr_t)&lstat_fake },
  { "chmod",    (uintptr_t)&chmod_fake },
  { "fchmod",   (uintptr_t)&fchmod_fake },
  { "fchown",   (uintptr_t)&fchown_fake },
  { "mkdir",    (uintptr_t)&mkdir_fake },
  { "rmdir",    (uintptr_t)&rmdir_fake },
  { "chdir",    (uintptr_t)&chdir_fake },
  { "symlink",  (uintptr_t)&symlink_fake },
  { "readlink", (uintptr_t)&readlink_fake },

  /* --- process control --- */
  { "system",            (uintptr_t)&system_fake },
  { "popen",             (uintptr_t)&popen_fake },
  { "pclose",            (uintptr_t)&pclose_fake },
  { "raise",             (uintptr_t)&raise_fake },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "mremap",            (uintptr_t)&mremap_fake },
  { "geteuid",           (uintptr_t)&geteuid_fake },

  /* --- stdio objects (data, not functions) --- */
  { "stdin",  (uintptr_t)&fake_stdin },
  { "stdout", (uintptr_t)&fake_stdout },
  { "stderr", (uintptr_t)&fake_stderr },

  /* --- plain pass-throughs newlib already provides correctly --- */
  { "strchr",  (uintptr_t)&strchr },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr",  (uintptr_t)&strstr },
  { "strspn",  (uintptr_t)&strspn },
  { "strcspn", (uintptr_t)&strcspn },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strdup",  (uintptr_t)&strdup },
  { "rewind",  (uintptr_t)&rewind },
  { "setvbuf", (uintptr_t)&setvbuf },
  { "vprintf", (uintptr_t)&vprintf },
  { "modf",    (uintptr_t)&modf },
  { "fmodf",   (uintptr_t)&fmodf },
  { "difftime",(uintptr_t)&difftime },
  /* MUST be the translating shim, not newlib's clock_gettime.
   *
   * The engine and FMOD were built against bionic and pass bionic clock ids;
   * newlib numbers them differently (bionic CLOCK_MONOTONIC is 1, which is
   * newlib's CLOCK_REALTIME; newlib's CLOCK_MONOTONIC is 4). Binding the raw
   * function here meant bionic's CLOCK_REALTIME -- 0 -- reached newlib as a
   * clock id it does not define at all:
   *
   *     terminating with uncaught exception of type std::system_error:
   *     clock_gettime(CLOCK_REALTIME) failed: Invalid argument
   *
   * libc++'s system_clock::now() throws on failure, so this aborts the process
   * rather than returning a bad time. The shim has existed in libc_shim.c the
   * whole time; the table simply never pointed at it. */
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },

};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void) {
  /* Bind the *OES extension entry points now that a GLES1 context is current.
   * Everything else in the table is a straight pass-through, and the FMOD
   * surface binds to the real libfmodex module rather than to this table. */
  p_BindFramebufferOES        = (void (*)(GLenum, GLuint))gl_proc("glBindFramebufferOES", "glBindFramebuffer");
  p_CheckFramebufferStatusOES = (GLenum (*)(GLenum))gl_proc("glCheckFramebufferStatusOES", "glCheckFramebufferStatus");
  p_DeleteFramebuffersOES     = (void (*)(GLsizei, const GLuint *))gl_proc("glDeleteFramebuffersOES", "glDeleteFramebuffers");
  p_FramebufferTexture2DOES   = (void (*)(GLenum, GLenum, GLenum, GLuint, GLint))gl_proc("glFramebufferTexture2DOES", "glFramebufferTexture2D");
  p_GenFramebuffersOES        = (void (*)(GLsizei, GLuint *))gl_proc("glGenFramebuffersOES", "glGenFramebuffers");
  p_MapBufferOES              = (void *(*)(GLenum, GLenum))gl_proc("glMapBufferOES", "glMapBuffer");
  p_UnmapBufferOES            = (GLboolean (*)(GLenum))gl_proc("glUnmapBufferOES", "glUnmapBuffer");

  if (!p_MapBufferOES || !p_UnmapBufferOES)
    debugPrintf("gl: GL_OES_mapbuffer unavailable -- using shadow-buffer emulation\n");
}
