/* main.c -- Where's My Water? 2 Switch wrapper entry point
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * On Android, Where's My Water? 2 (com.disney.wheresmywater2_goo) is a native
 * C++ game: the Walaber engine drawing through OpenGL ES 1.1 fixed-function,
 * with FMOD Ex 4.44.64 for audio and a statically-linked jsoncpp for its data.
 * Its Java layer is a set of twenty-one subsystem "bridges" under
 * com.disney.GameLib.Bridge.* that forward lifecycle, resize, per-frame draw
 * and touch into the .so.
 *
 * This file recreates that Java layer in C: load and link libfmodex.so +
 * libwalaber.so, stand up a GLES1 context and a fake JNI environment, then call
 * the same entry points in the same order the Android app did:
 *
 *     construct bridges  -> jniBridgeInit x21
 *     Labor_AppStartup   -> jniWalaberChassisStartup(9 args)
 *     surface created    -> jniRenderInit -> jniRenderAreaCreated
 *                                         -> jniRenderAreaReload
 *     every frame        -> jniRenderDrawPreDraw + jniRenderDrawFrame
 *
 * Data files (extracted from a copy of the game you own, never bundled):
 *   libwalaber.so, libfmodex.so and the assets/ tree -- placed next to the .nro.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "libc_shim.h"
#include "nx_pointer.h"
#include "wmw_paths.h"
#include "wmw_bundle.h"
#include "wmw_tate.h"
#include "wmw_assetindex.h"
#include "opensles.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "fmod_audio.h"

#include "wmw2_entrypoints.h"
#include "wmw2_bridges.h"
#include "wmw2_callbacks.h"
#include "wmw2_jni.h"
#include "wmw2_store.h"
#include "wmw2_probe.h"
#include "wmw2_movie.h"

size_t g_heap_total, g_heap_newlib, g_heap_so;  /* reported once at boot */

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module fmod_mod;  /* libfmodex.so  -- loaded first; libwalaber imports 45 symbols from it */
so_module game_mod;  /* libwalaber.so */

wmw2_entry_points wmw2;

/* libfmodex.so's two audio entry points. Resolved in resolve_entry_points(),
 * which runs BEFORE so_finalize() -- see fmod_audio.h for why that matters. */
static uintptr_t fmod_getinfo_addr, fmod_process_addr;

static PadState g_pad;

/* ------------------------------------------------------------------------- */

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  /* Split the process heap between newlib and the .so load zone.
   *
   * The load zone only has to hold the game's native libraries -- libwalaber.so
   * is about 16.8 MB and libfmodex.so about 1.1 MB -- so a fixed reservation is
   * right, and everything else belongs to newlib.
   *
   * That distribution matters more than it looks. Mesa allocates GPU memory
   * from the SAME newlib heap (nouveau_bo_new -> memalign -> malloc), so every
   * texture the engine uploads competes with the engine's own allocations.
   * Capping newlib at a fixed size and giving the remainder to the load zone
   * leaves that memory almost entirely unused while starving the allocator
   * actually under pressure -- on WMW1 that left 2.8 GB idle while texture
   * uploads failed inside malloc(). The failure arrives several frames deep in
   * mesa's texture path:
   *
   *     _malloc_r <- _memalign_r <- nouveau_bo_new <- nvc0_miptree_create
   *               <- st_texture_create <- st_TexImage
   *
   * WMW2 ships 1,573 textures against WMW1's few hundred, so if anything this
   * matters more here.
   */
  size_t so_zone = SO_ZONE_MB * 1024 * 1024;
  if (so_zone > size / 4)          /* pathologically small heap: stay sane */
    so_zone = size / 4;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_size  = size - so_zone;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)((char *)addr + fake_heap_size), 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;

  g_heap_total  = size;
  g_heap_newlib = fake_heap_size;
  g_heap_so     = heap_so_limit;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.\nLaunch through a title override, not the album.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  const char *files[] = { FMOD_SO_NAME, SO_NAME };
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++)
    if (stat(files[i], &st) < 0)
      fatal_error("Could not find\n%s.\nExtract it from your APK's\nlib/arm64-v8a/ next to the .nro.", files[i]);

  /* WMW2's assets sit one level deeper than WMW1's: assets/Water/, not
   * assets/. Check for the real thing so a WMW1-shaped install fails here with
   * a useful message rather than as a thousand missing files later. */
  /* Extracted assets are required, and an .apk is not a substitute: the engine
   * builds <pkg>/assets/Water/Data/... by concatenation and opens the result
   * directly, so the tree has to actually be on disk. */
  if (stat("assets/" WMW2_ASSET_SUBDIR "/Data", &st) < 0)
    fatal_error("No game data found.\nCopy the APK's assets/ folder\n"
                "next to the .nro -- it must contain\nassets/" WMW2_ASSET_SUBDIR "/Data/.");

  /* The five files the engine copies out on first run. If any is missing the
   * copy truncates its destination to zero and the engine aborts several
   * seconds later with nothing useful on screen, so say so now instead. */
  static const char *const required[] = {
    "levelinfo.db", "perry.db", "factory_profile.json", "store.json", "news.json"
  };
  for (unsigned i = 0; i < sizeof(required)/sizeof(*required); i++) {
    char p[WMW_PATH_MAX];
    snprintf(p, sizeof(p), "assets/" WMW2_ASSET_SUBDIR "/Data/%s", required[i]);
    if (stat(p, &st) < 0)
      fatal_error("Game data is incomplete:\nassets/" WMW2_ASSET_SUBDIR "/Data/%s\nis missing.", required[i]);
  }
}

/* WMW1's port looked for an .apk here and synthesised a bundle.zip when there
 * was none, because its engine opened that argument as an archive. WMW2 does
 * not -- see the chassis-startup block below -- so both are gone. wmw_bundle.c
 * is still in the tree for reference but nothing calls it. */

/* jniWalaberChassisStartup's last argument is the GCS working directory, and
 * the engine appends "/GCS.db" to it -- an SQLite database it creates on first
 * run. Make sure the directory exists; SQLite will not create it, and the
 * failure surfaces as the entirely generic "disk I/O error" rather than as
 * anything resembling "that folder is missing". */
static void ensure_work_dirs(char *gcs_out, size_t gcs_len,
                             char *migs_out, size_t migs_len) {
  snprintf(gcs_out,  gcs_len,  "%s/gcs",  wmw_game_dir());
  snprintf(migs_out, migs_len, "%s/migs", wmw_game_dir());
  if (mkdir(gcs_out, 0777) == 0)  debugPrintf("paths: created %s\n", gcs_out);
  if (mkdir(migs_out, 0777) == 0) debugPrintf("paths: created %s\n", migs_out);
}


/* The engine ships two SQLite databases inside its bundle and copies them to a
 * writable location on first run -- Walaber::WalaberGame::copyDatabaseFromBundle(),
 * staging through "/checked_tmp.db". On Android the bundle is the APK and the
 * copy just works.
 *
 * Here it does not. With loose assets there is no real archive to extract from,
 * so the copy fails, and the engine then opens the destination that was never
 * written and freads from the NULL FILE* it got back. That is not a hypothetical:
 * it is exactly how the first WMW2 boot died, faulting in _fread_r with x4 = 0
 * and only "levelinfo.db" left in a register to say why.
 *
 * The shipped copies are right there in assets/Water/Data/, so seed the writable
 * ones ourselves before the engine starts. This is the same fix WMW1's port
 * needed for water.db; only the filenames changed.
 *
 * Note the destination is INFERRED to be the private directory -- the same one
 * handed to jniWalaberChassisStartup. If a future log still shows the engine
 * missing these, that assumption is the first thing to re-check; the fopen line
 * immediately before the failure will name the path it actually wanted. */
static void seed_one_database(const char *name) {
  char dst[WMW_PATH_MAX], src[WMW_PATH_MAX];
  snprintf(dst, sizeof(dst), "%s/%s", wmw_game_dir(), name);
  snprintf(src, sizeof(src), "%s/assets/" WMW2_ASSET_SUBDIR "/Data/%s",
           wmw_game_dir(), name);

  /* Presence is not enough. A half-finished copy leaves a truncated file that
   * the engine then reports as "file is encrypted or is not a database" on
   * every launch from then on, so check the magic and re-seed if it is not a
   * real database. A bad run repairs itself on the next start.
   *
   * What this must NOT do is re-seed a database the player has been writing to.
   * The engine migrates its own databases -- fresh schema in checked_tmp.db,
   * player data copied across, then installed -- so a seed on top of a live
   * file would discard exactly what it is trying to preserve. A valid SQLite
   * header is enough to leave it alone. */
  struct stat st;
  if (stat(dst, &st) == 0 && st.st_size > 0) {
    char magic[16] = {0};
    FILE *chk = fopen(dst, "rb");
    if (chk) {
      const size_t got = fread(magic, 1, sizeof(magic), chk);
      fclose(chk);
      if (got == sizeof(magic) && memcmp(magic, "SQLite format 3", 15) == 0)
        return;                       /* valid -- it holds the player's progress */
    }
    debugPrintf("db: %s is not a valid database -- reseeding\n", dst);
    remove(dst);
  }

  FILE *in = fopen(src, "rb");
  if (!in) {
    debugPrintf("db: no bundled %s at %s\n", name, src);
    return;
  }
  FILE *out = fopen(dst, "wb");
  if (!out) {
    fclose(in);
    debugPrintf("db: could not create %s -- is the SD card writable?\n", dst);
    return;
  }

  char buf[32 * 1024];
  size_t n, total = 0;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) { total = 0; break; }
    total += n;
  }
  fclose(in);
  fclose(out);
  debugPrintf("db: seeded %s (%zu bytes)\n", name, total);
}

static void ensure_databases(void) {
  /* Only these two ship in assets/Water/Data/. progress.db and the perry-Lite /
   * perry-demo variants are named in the binary but are not in the retail
   * asset tree -- the engine creates progress.db itself, and the Lite/demo
   * builds are other SKUs. */
  seed_one_database("levelinfo.db");
  seed_one_database("perry.db");

  /* A leftover staging file from an interrupted copy is a state variable we do
   * not want. */
  char tmp[WMW_PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s/checked_tmp.db", wmw_game_dir());
  if (remove(tmp) == 0)
    debugPrintf("db: cleared stale checked_tmp.db\n");
}

static void set_screen_size(void) {
  /* Locked to the handheld panel in both modes. The portrait target is rotated
   * onto it 1:1, so docking changes nothing: the engine is never told its
   * resolution moved, and there is no re-layout to go wrong halfway through a
   * level. Docked simply shows the same rotated image, upscaled by the console. */
  screen_width  = 1280;
  screen_height = 720;
  render_width  = 720;
  render_height = 1280;
  debugPrintf("screen: window %dx%d, engine renders %dx%d (portrait)\n",
              screen_width, screen_height, render_width, render_height);
}

/* ------------------------------------------------------------------------- */
/* EGL / GLES1 context on the default NWindow                                 */
/* ------------------------------------------------------------------------- */

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

static int egl_init(void) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) { debugPrintf("egl: no display\n"); return 0; }
  eglInitialize(s_display, NULL, NULL);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { debugPrintf("egl: bindAPI failed\n"); return 0; }

  /* GLES1: EGL_OPENGL_ES_BIT, not ES2_BIT. libwalaber.so imports 56 GL entry
   * points, every one fixed-function or GL_OES_framebuffer_object, and zero
   * GLES2 shader entry points. */
  const EGLint cfg_attr[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  EGLConfig config;
  EGLint num = 0;
  if (!eglChooseConfig(s_display, cfg_attr, &config, 1, &num) || num < 1) {
    debugPrintf("egl: no GLES1 config\n");
    return 0;
  }

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  s_surface = eglCreateWindowSurface(s_display, config, win, NULL);
  if (!s_surface) { debugPrintf("egl: no surface\n"); return 0; }

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
  s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctx_attr);
  if (!s_context) { debugPrintf("egl: no context\n"); return 0; }

  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, 1);

  /* From here on the framebuffer belongs to EGL, so fatal_error() must not try
   * to consoleInit() over the top of it -- that is what turned the engine's
   * abort() into a data abort inside ConsoleSwRenderer_drawChar. */
  fatal_error_no_console();

  debugPrintf("gl vendor:   %s\n", (const char *)glGetString(GL_VENDOR));
  debugPrintf("gl renderer: %s\n", (const char *)glGetString(GL_RENDERER));
  debugPrintf("gl version:  %s\n", (const char *)glGetString(GL_VERSION));
  return 1;
}

static void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) eglDestroyContext(s_display, s_context);
  if (s_surface) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
}

/* ------------------------------------------------------------------------- */
/* module loading                                                             */
/* ------------------------------------------------------------------------- */

#define BRIDGE "Java_com_disney_GameLib_Bridge_"

#define RESOLVE(field, sym)                                                    \
  do {                                                                         \
    wmw2.field = (void *)so_try_find_addr_rx(&game_mod, sym);                  \
    if (!wmw2.field) debugPrintf("entry point missing: %s\n", sym);            \
  } while (0)

static void resolve_entry_points(void) {
  wmw2.JNI_OnLoad = (fn_onload)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");

  /* --- WalaberNativeChassis --------------------------------------------- */
  RESOLVE(chassis_BridgeInit,  BRIDGE "WalaberNativeChassis_jniBridgeInit");
  RESOLVE(chassis_BridgeDone,  BRIDGE "WalaberNativeChassis_jniBridgeDone");
  RESOLVE(chassis_Startup,     BRIDGE "WalaberNativeChassis_jniWalaberChassisStartup");
  RESOLVE(chassis_Shutdown,    BRIDGE "WalaberNativeChassis_jniWalaberChassisShutdown");
  RESOLVE(chassis_AppPause,    BRIDGE "WalaberNativeChassis_jniWalaberChassisAppPause");
  RESOLVE(chassis_AppResume,   BRIDGE "WalaberNativeChassis_jniWalaberChassisAppResume");

  /* --- Rendering --------------------------------------------------------- */
  RESOLVE(render_BridgeInit,   BRIDGE "Rendering_BridgeRendering_jniBridgeInit");
  RESOLVE(render_BridgeDone,   BRIDGE "Rendering_BridgeRendering_jniBridgeDone");
  RESOLVE(render_Init,         BRIDGE "Rendering_BridgeRendering_jniRenderInit");
  RESOLVE(render_AreaCreated,  BRIDGE "Rendering_BridgeRendering_jniRenderAreaCreated");
  RESOLVE(render_AreaResized,  BRIDGE "Rendering_BridgeRendering_jniRenderAreaResized");
  RESOLVE(render_AreaReload,   BRIDGE "Rendering_BridgeRendering_jniRenderAreaReload");
  RESOLVE(render_AreaDestroyed,BRIDGE "Rendering_BridgeRendering_jniRenderAreaDestroyed");
  RESOLVE(render_DrawPreDraw,  BRIDGE "Rendering_BridgeRendering_jniRenderDrawPreDraw");
  RESOLVE(render_DrawFrame,    BRIDGE "Rendering_BridgeRendering_jniRenderDrawFrame");

  /* --- DeviceIO ---------------------------------------------------------- */
  RESOLVE(touch_BridgeInit,    BRIDGE "DeviceIO_BridgeTouchHandling_jniBridgeInit");
  RESOLVE(touch_BridgeDone,    BRIDGE "DeviceIO_BridgeTouchHandling_jniBridgeDone");
  RESOLVE(touch_Began,         BRIDGE "DeviceIO_BridgeTouchHandling_jniTouchBegan");
  RESOLVE(touch_Moved,         BRIDGE "DeviceIO_BridgeTouchHandling_jniTouchMoved");
  RESOLVE(touch_Ended,         BRIDGE "DeviceIO_BridgeTouchHandling_jniTouchEnded");

  RESOLVE(sensor_BridgeInit,   BRIDGE "DeviceIO_BridgeSensorHandling_jniBridgeInit");
  RESOLVE(sensor_BridgeDone,   BRIDGE "DeviceIO_BridgeSensorHandling_jniBridgeDone");
  RESOLVE(sensor_AccelerometerChanged,
                               BRIDGE "DeviceIO_BridgeSensorHandling_jniAccelerometerChanged");

  RESOLVE(keyboard_BridgeInit, BRIDGE "DeviceIO_BridgeKeyboardHandling_jniBridgeInit");
  RESOLVE(keyboard_BridgeDone, BRIDGE "DeviceIO_BridgeKeyboardHandling_jniBridgeDone");
  RESOLVE(keyboard_BackKeyPressed,
                               BRIDGE "DeviceIO_BridgeKeyboardHandling_jniBackKeyPressed");

  /* --- AppEvents --------------------------------------------------------- */
  RESOLVE(appfocus_BridgeInit, BRIDGE "AppEvents_BridgeAppFocusEvents_jniBridgeInit");
  RESOLVE(appfocus_BridgeDone, BRIDGE "AppEvents_BridgeAppFocusEvents_jniBridgeDone");
  RESOLVE(appfocus_LostFocusShowPauseMenu,
          BRIDGE "AppEvents_BridgeAppFocusEvents_jniAppLostFocusPleaseShowPauseMenu");

  RESOLVE(audioinfo_BridgeInit, BRIDGE "AppEvents_BridgeAudioAppInfo_jniBridgeInit");
  RESOLVE(audioinfo_BridgeDone, BRIDGE "AppEvents_BridgeAudioAppInfo_jniBridgeDone");
  RESOLVE(audioinfo_IsSafeToPlay,
          BRIDGE "AppEvents_BridgeAudioAppInfo_jniAudioIsSafeToPlay");

  RESOLVE(gameflow_BridgeInit, BRIDGE "AppEvents_BridgeGameFlowEvents_jniBridgeInit");
  RESOLVE(gameflow_BridgeDone, BRIDGE "AppEvents_BridgeGameFlowEvents_jniBridgeDone");

  RESOLVE(compat_BridgeInit,
          BRIDGE "AppEvents_CompatibilityIssue_BridgeCompatibilityIssue_jniBridgeInit");
  RESOLVE(compat_ShowMessage,
          BRIDGE "AppEvents_CompatibilityIssue_BridgeCompatibilityIssue_ShowMessage");
  RESOLVE(forceupdate_BridgeInit,
          BRIDGE "AppEvents_ForceUpdate_BridgeForceUpdate_jniBridgeInit");
  RESOLVE(forceupdate_SendSignalToGameCode,
          BRIDGE "AppEvents_ForceUpdate_BridgeForceUpdate_SendForceUpdateSignalToGameCode");
  RESOLVE(googlecmp_BridgeInit,
          BRIDGE "AppEvents_GoogleConsentsUpdate_BridgeGoogleCMPUpdate_jniBridgeInit");

  /* --- Display ----------------------------------------------------------- */
  RESOLVE(layout_BridgeInit,   BRIDGE "Display_BridgeWalaberCustomLayout_jniBridgeInit");
  RESOLVE(layout_BridgeDone,   BRIDGE "Display_BridgeWalaberCustomLayout_jniBridgeDone");
  RESOLVE(layout_NotifyMovieFinished,
          BRIDGE "Display_BridgeWalaberCustomLayout_jniNotifyMovieFinished");
  RESOLVE(videoview_BridgeInit, BRIDGE "Display_BridgeWalaberVideoView_jniBridgeInit");
  RESOLVE(videoview_BridgeDone, BRIDGE "Display_BridgeWalaberVideoView_jniBridgeDone");
  RESOLVE(videoview_RequestVideoVolume,
          BRIDGE "Display_BridgeWalaberVideoView_jniRequestVideoVolume");

  /* --- Text / Net -------------------------------------------------------- */
  RESOLVE(text_BridgeInit,     BRIDGE "Text_BridgeTextL18Ning_jniBridgeInit");
  RESOLVE(text_BridgeDone,     BRIDGE "Text_BridgeTextL18Ning_jniBridgeDone");
  RESOLVE(text_GetLocalizedText, BRIDGE "Text_BridgeTextL18Ning_jniGetLocalizedText");

  RESOLVE(net_BridgeInit,      BRIDGE "Net_BridgeNetGeneral_jniBridgeInit");
  RESOLVE(net_BridgeDone,      BRIDGE "Net_BridgeNetGeneral_jniBridgeDone");
  RESOLVE(net_GetLocalizedString, BRIDGE "Net_BridgeNetGeneral_jniGetLocalizedString");

  /* --- Adverts ----------------------------------------------------------- */
  RESOLVE(adverts_BridgeInit,  BRIDGE "Net_Adverts_BridgeAdverts_jniBridgeInit");
  RESOLVE(adverts_BridgeDone,  BRIDGE "Net_Adverts_BridgeAdverts_jniBridgeDone");
  RESOLVE(adverts_NotifyAdEvent,       BRIDGE "Net_Adverts_BridgeAdverts_jniNotifyAdEvent");
  RESOLVE(adverts_NotifyAllAdsCreated, BRIDGE "Net_Adverts_BridgeAdverts_jniNotifyAllAdsCreated");
  RESOLVE(adverts_NotifyHandleUrl,     BRIDGE "Net_Adverts_BridgeAdverts_jniNotifyHandleUrl");
  RESOLVE(adverts_NotifyRewardCurrency,BRIDGE "Net_Adverts_BridgeAdverts_jniNotifyRewardCurrency");

  /* --- DisMoLibs: MIGS --------------------------------------------------- */
#define MIGS BRIDGE "DisMoLibs_BridgeDisMoMigs_"
  RESOLVE(migs_BridgeInit,  MIGS "jniBridgeInit");
  RESOLVE(migs_BridgeDone,  MIGS "jniBridgeDone");
  RESOLVE(migs_GetRewardVideoIdsToPreload, MIGS "jniGetRewardVideoIdsToPreload");
  RESOLVE(migs_GetUserAge,                 MIGS "jniGetUserAge");
  RESOLVE(migs_GetUserPreferredLang,       MIGS "jniGetUserPreferredLang");
  RESOLVE(migs_HideDownloadingPopup,       MIGS "jniHideDownloadingPopup");
  RESOLVE(migs_ShowDownloadingPopup,       MIGS "jniShowDownloadingPopup");
  RESOLVE(migs_NotifyCustCareEmailResult,  MIGS "jniNotifyNewMigsInfoCustCareEmailResult");
  RESOLVE(migs_NotifyGiftConsumed,         MIGS "jniNotifyNewMigsInfoGiftConsumed");
  RESOLVE(migs_NotifyLeaderboard,          MIGS "jniNotifyNewMigsInfoLeaderboard");
  RESOLVE(migs_NotifyRewardsBalance,       MIGS "jniNotifyNewMigsInfoRewardsBalance");
  RESOLVE(migs_NotifyRewardsConsumed,      MIGS "jniNotifyNewMigsInfoRewardsConsumed");
  RESOLVE(migs_NotifyStoreNothingToRestore,MIGS "jniNotifyNewMigsInfoStoreNothingToRestore");
  RESOLVE(migs_NotifyStoreProductPurchased,MIGS "jniNotifyNewMigsInfoStoreProductPurchased");
  RESOLVE(migs_NotifyStoreProductToRestore,MIGS "jniNotifyNewMigsInfoStoreProductToRestore");
  RESOLVE(migs_NotifyStoreSingleItemInfoResult,
                                           MIGS "jniNotifyNewMigsInfoStoreSingleItemInfoResult");
  RESOLVE(migs_RewardVideoCanceled,        MIGS "jniRewardVideoCanceled");
  RESOLVE(migs_RewardVideoCompleted,       MIGS "jniRewardVideoCompleted");
  RESOLVE(migs_RewardVideoFailed,          MIGS "jniRewardVideoFailed");
  RESOLVE(migs_RewardVideoInit,            MIGS "jniRewardVideoInit");
  RESOLVE(migs_UpdateGameWithMigsGiftItems, MIGS "jniUpdateGameWithMigsGiftItems");
  RESOLVE(migs_UpdateGameWithMigsNewsItems, MIGS "jniUpdateGameWithMigsNewsItems");
  RESOLVE(migs_UpdateGameWithMigsProfile,   MIGS "jniUpdateGameWithMigsProfile");
  RESOLVE(migs_UpdateGameWithMigsStoreItems,MIGS "jniUpdateGameWithMigsStoreItems");
#undef MIGS

  RESOLVE(analytics_BridgeInit, BRIDGE "DisMoLibs_BridgeDisMoAnalyticals_jniBridgeInit");
  RESOLVE(analytics_BridgeDone, BRIDGE "DisMoLibs_BridgeDisMoAnalyticals_jniBridgeDone");
  RESOLVE(referralstore_BridgeInit, BRIDGE "DisMoLibs_BridgeDisMoReferralStore_jniBridgeInit");
  RESOLVE(referralstore_BridgeDone, BRIDGE "DisMoLibs_BridgeDisMoReferralStore_jniBridgeDone");

  /* --- BridgeDisMoAbtest -------------------------------------------------
   * Exported by libwalaber.so but with no Java class anywhere in the six dex
   * files -- dead in this build. Resolved anyway so a future build that revives
   * it does not silently skip the bridge. */
  RESOLVE(abtest_BridgeInit, BRIDGE "DisMoLibs_BridgeDisMoAbtest_jniBridgeInit");
  RESOLVE(abtest_BridgeDone, BRIDGE "DisMoLibs_BridgeDisMoAbtest_jniBridgeDone");
  RESOLVE(abtest_DeliverAbtConfigData,
          BRIDGE "DisMoLibs_BridgeDisMoAbtest_jniDeliverAbtConfigData");

  /* --- Junction: the JNI self-test harness -------------------------------
   * Never called during play. jniConfirmStaticConnect is the only entry point
   * in the whole build whose upcall is resolved with GetStaticMethodID rather
   * than GetMethodID, which makes it a decent smoke test for the fake JNI. */
  RESOLVE(junction_ConfirmNonStaticConnect,
          "Java_com_disney_GameLib_Junction_JunctionTester_jniConfirmNonStaticConnect");
  RESOLVE(junction_ConfirmStaticConnect,
          "Java_com_disney_GameLib_Junction_JunctionTester_jniConfirmStaticConnect");
  RESOLVE(junction_DoLoggerTests,
          "Java_com_disney_GameLib_Junction_JunctionTester_jniDoLoggerTests");
  RESOLVE(junction_ManipulateManagedField,
          "Java_com_disney_GameLib_Junction_JunctionTester_jniManipulateManagedField");

  fmod_getinfo_addr = so_try_find_addr_rx(&fmod_mod,
      "Java_org_fmod_FMODAudioDevice_fmodGetInfo");
  fmod_process_addr = so_try_find_addr_rx(&fmod_mod,
      "Java_org_fmod_FMODAudioDevice_fmodProcess");

  if (!wmw2.chassis_Startup || !wmw2.render_Init || !wmw2.render_DrawFrame)
    fatal_error("libwalaber.so is missing its Chassis\nor Rendering entry points.\n"
                "Wrong library or unsupported game version?");
}

static void load_two_modules(void) {
  if (so_load(&fmod_mod, FMOD_SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", FMOD_SO_NAME);

  const size_t used = ALIGN_MEM(fmod_mod.load_size, 0x1000);
  void *base2 = (char *)heap_so_base + used;
  size_t avail2 = (heap_so_limit > used) ? heap_so_limit - used : 0;

  if (so_load(&game_mod, SO_NAME, base2, avail2) < 0)
    fatal_error("Could not load\n%s.\n(%zu MB free in the module zone --\n"
                "raise SO_ZONE_MB if this is tight.)", SO_NAME, avail2 >> 20);

  so_relocate(&fmod_mod);
  so_relocate(&game_mod);

  update_imports();

  /* FMOD first: its libc imports come straight from the table. */
  so_resolve(&fmod_mod, dynlib_functions, dynlib_numfunctions, 1);
  /* Then the engine: the table is consulted first, then sibling modules, so the
   * 45 FMOD_* / FMOD::* imports bind to the real libfmodex sitting in so_list. */
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  resolve_entry_points();

  /* Instrumentation for the main-menu loading hang. Must go here: it patches
   * vtable slots in libwalaber's .data.rel.ro, which so_finalize() is about to
   * remap read-execute. See wmw2_probe.c for what it answers. */
  wmw2_probe_install(&game_mod);

  so_finalize(&fmod_mod);
  so_finalize(&game_mod);
  so_flush_caches(&fmod_mod);
  so_flush_caches(&game_mod);

  /* libwalaber was built with -mstack-protector-guard=tls: guarded functions
   * read the canary from tpidr_el0 + 0x28. It is visible in the prologue of
   * jniWalaberChassisStartup and in the FMOD init path. Install it before any
   * engine code runs. */
  tls_setup_guard();

  so_execute_init_array(&fmod_mod);
  so_execute_init_array(&game_mod);   /* C++ static constructors */

  so_free_temp(&fmod_mod);
  so_free_temp(&game_mod);
}

/* ------------------------------------------------------------------------- */
/* touch: batched parallel arrays                                             */
/* ------------------------------------------------------------------------- */

/* The engine's contact pool is bounded. BridgeTouchHandling logs "Per-Frame
 * Touch Down count met or exceeded expectations" once the count reaches 11, so
 * ten is the working ceiling. The Switch can report up to sixteen contacts;
 * the extra ones are dropped rather than risking the engine's pool. */
#define MAX_TOUCH 10

/* classes*.dex declares:
 *   jniTouchBegan(int, float[], float[], int[])
 *   jniTouchEnded(int, float[], float[], int[])
 *   jniTouchMoved(int, float[], float[], float[], float[], int[])
 * -- so "moved" also wants each contact's PREVIOUS position, and the id array
 * is int[], not float[]. Confirmed independently from the disassembly: the
 * coordinate arrays come out through JNIEnv slot 0x5e8/8 = 189
 * (GetFloatArrayElements) and the ids through 0x5d8/8 = 187
 * (GetIntArrayElements). Allocated once and reused, as the Java bridge did. */
static void *a_x, *a_y, *a_px, *a_py, *a_id;
static float *t_x, *t_y, *t_px, *t_py;
static int32_t *t_id;

/* Last reported position per pointer id, so "moved" can supply prevX/prevY. */
static float last_x[MAX_TOUCH], last_y[MAX_TOUCH];
static int   last_valid[MAX_TOUCH];

static void touch_arrays_init(void) {
  a_x  = jni_make_float_array(MAX_TOUCH, &t_x);
  a_y  = jni_make_float_array(MAX_TOUCH, &t_y);
  a_px = jni_make_float_array(MAX_TOUCH, &t_px);
  a_py = jni_make_float_array(MAX_TOUCH, &t_py);
  a_id = jni_make_int_array(MAX_TOUCH, &t_id);

  /* These live for the whole process and we hold raw pointers into their
   * backing stores (t_x, t_y, ...), refilled every frame. As plain local refs
   * the engine could discard them at any PopLocalFrame -- free_ref() releases
   * the backing store too, after which every touch writes into freed memory and
   * the heap fails later inside free(), with a stack that points at the engine
   * rather than at us. Pin them. */
  jni_pin(a_x);
  jni_pin(a_y);
  jni_pin(a_px);
  jni_pin(a_py);
  jni_pin(a_id);

  for (int i = 0; i < MAX_TOUCH; i++)
    t_id[i] = -1;   /* TOUCH_ID_NOFINGER, as the Java side initialised it */
}

/* Present one movie frame: the clip has already been drawn into the portrait
 * target, so this is the same tail as the main loop's frame. */
static void movie_present(void) {
  wmw_tate_present();
  eglSwapBuffers(s_display, s_surface);
  wmw_tate_begin();      /* rebind for the next frame the player draws */
}

static void nxp_log(const char *m) { debugPrintf("%s", (char *)m); }

/* nx_pointer reads cursor.png and persists pointer.cfg. Those opens allocate
 * newlib handles like any other, so they must take the same lock as the rest of
 * the port -- FMOD's threads are live by then. See libc_shim.c. */
static FILE *nxp_fopen(const char *path, const char *mode) {
  wmw_file_lock();
  FILE *f = fopen(path, mode);
  wmw_file_unlock();
  return f;
}
static int nxp_fclose(FILE *f) {
  wmw_file_lock();
  const int r = fclose(f);
  wmw_file_unlock();
  return r;
}

static void input_init(void) {
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);

  NxpConfig pcfg;
  memset(&pcfg, 0, sizeof(pcfg));

  /* The cursor lives in RENDER space -- the portrait framebuffer the engine
   * draws into -- because nxp_draw() runs before wmw_tate_present() and is
   * therefore rotated along with everything else. Physical input is what needs
   * converting, and `rotation` tells the module to do it. */
  pcfg.screen_w = render_width;
  pcfg.screen_h = render_height;

  /* The touch panel is bonded to the physical glass and does not rotate just
   * because the image does, so it stays in landscape panel space. */
  pcfg.panel_w  = screen_width;
  pcfg.panel_h  = screen_height;

  pcfg.rotation     = wmw_rotation_mode();
  pcfg.handle_touch = 1;   /* one owner for the input rotation, not two */
  pcfg.data_dir     = wmw_game_dir();
  pcfg.log          = nxp_log;
  pcfg.fopen_fn     = nxp_fopen;
  pcfg.fclose_fn    = nxp_fclose;
  nxp_init(&pcfg);
}

/* Dispatch this frame's contacts, matching what WalaberViewTouchHandler did.
 *
 * Verified against classes2.dex rather than guessed, because two details are
 * not what a batched API would suggest:
 *
 *  1. Coordinates are NORMALISED. copyTouches() computes 1.0f/width and
 *     1.0f/height and multiplies, so the engine wants 0..1, not pixels.
 *     Passing pixels puts every contact off the bottom-right corner, which
 *     reads as "touch does nothing" rather than as a coordinate bug.
 *
 *  2. DOWN and UP carry exactly ONE contact. Android delivers ACTION_DOWN and
 *     ACTION_POINTER_DOWN as separate MotionEvents, so the Java side always
 *     called TouchBegan(1, ...) / TouchEnded(1, ...) with the contact at index
 *     0. Only TouchMoved ever carries several, as min(pointerCount, 10).
 *     Batching two simultaneous presses into one call would hand the engine a
 *     shape it has never seen on any device.
 *
 * The unused id slots are set to TOUCH_ID_NOFINGER (-1) because the Java side
 * cleared all ten at the top of every onTouch(). Costs nothing and means the
 * engine never reads a stale id if it scans past `count`. */
#define TOUCH_ID_NOFINGER (-1)

static void touch_log_once(int phase, int count, float px, float py) {
  static int logged = 0;
  if (logged >= 12) return;
  logged++;
  debugPrintf("touch: %s n=%d  panel(%.0f,%.0f) -> engine(%.3f,%.3f)\n",
              phase == NXP_DOWN ? "down" : phase == NXP_UP ? "up" : "move",
              count, (double)px, (double)py, (double)t_x[0], (double)t_y[0]);
}

/* Fill slot `dst` from event `e`, converting to the engine's 0..1 space. */
static void fill_slot(int dst, const NxpEvent *e, int phase) {
  const int id = e->id;
  const int slot = (id >= 0 && id < MAX_TOUCH) ? id : 0;

  /* nx_pointer already delivers render-space coordinates -- it applies the
   * inverse of the display rotation itself, for touch and cursor alike.
   * Rotating again here would undo it; one owner for that transform. */
  const float mx = e->x / (float)render_width;
  const float my = e->y / (float)render_height;

  t_x[dst]  = mx;
  t_y[dst]  = my;
  t_id[dst] = id;

  /* prev = where this id was last seen; on a fresh contact, itself -- which is
   * what the Java side did when it created a new TouchEvent. */
  t_px[dst] = last_valid[slot] ? last_x[slot] : mx;
  t_py[dst] = last_valid[slot] ? last_y[slot] : my;

  last_x[slot] = mx;
  last_y[slot] = my;
  last_valid[slot] = (phase != NXP_UP);
}

static void clear_unused_ids(int from) {
  for (int i = from; i < MAX_TOUCH; i++)
    t_id[i] = TOUCH_ID_NOFINGER;
}

static void dispatch_phase(int phase, const NxpEvent *ev, int n) {
  void *touch = wmw2_bridge(BR_TOUCH);

  if (phase == NXP_DOWN || phase == NXP_UP) {
    /* One call per contact, count = 1, contact at index 0. */
    for (int i = 0; i < n; i++) {
      if (ev[i].phase != phase) continue;
      fill_slot(0, &ev[i], phase);
      clear_unused_ids(1);
      touch_log_once(phase, 1, ev[i].x, ev[i].y);
      if (phase == NXP_DOWN) {
        if (wmw2.touch_Began) wmw2.touch_Began(fake_env, touch, 1, a_x, a_y, a_id);
      } else {
        if (wmw2.touch_Ended) wmw2.touch_Ended(fake_env, touch, 1, a_x, a_y, a_id);
      }
    }
    return;
  }

  /* MOVE: every contact that moved, up to the engine's pool limit. */
  int count = 0;
  for (int i = 0; i < n && count < MAX_TOUCH; i++) {
    if (ev[i].phase != phase) continue;
    fill_slot(count, &ev[i], phase);
    count++;
  }
  if (!count) return;
  clear_unused_ids(count);
  touch_log_once(phase, count, ev[0].x, ev[0].y);
  if (wmw2.touch_Moved)
    wmw2.touch_Moved(fake_env, touch, count, a_x, a_y, a_px, a_py, a_id);
}

static void feed_pointer(void) {
  nxp_update();
  NxpEvent ev[16];
  const int n = nxp_poll(ev, 16);
  if (!n) return;
  dispatch_phase(NXP_DOWN, ev, n);
  dispatch_phase(NXP_MOVE, ev, n);
  dispatch_phase(NXP_UP,   ev, n);
}

/* ------------------------------------------------------------------------- */
/* applet lifecycle -> Chassis lifecycle                                      */
/* ------------------------------------------------------------------------- */

static void applet_hook_fn(AppletHookType type, void *param) {
  (void)param;
  switch (type) {
    case AppletHookType_OnExitRequest:
      wmw2_quit_requested = 1;
      break;

    case AppletHookType_OnFocusState: {
      const bool focused = appletGetFocusState() == AppletFocusState_InFocus;
      debugPrintf("applet: focus %s\n", focused ? "gained" : "lost");
      fmod_audio_set_paused(!focused);
      if (focused) {
        if (wmw2.chassis_AppResume)
          wmw2.chassis_AppResume(fake_env, wmw2_bridge(BR_CHASSIS));
        /* Deliberately NOT calling jniRenderAreaReload on focus regain either.
         *
         * "A real Android context loss would need this; harmless here" was
         * wrong on the second half. The EGL surface survives a Switch focus
         * change -- the port never destroys it -- so there is nothing to
         * rebuild, and asking for a rebuild drops the engine into
         * Screen_GraphicsContextRestore, where it waits for RenderFlow events
         * the port cannot send. That would have turned every return from the
         * home menu into a permanent hang. */
      } else {
        if (wmw2.chassis_AppPause)
          wmw2.chassis_AppPause(fake_env, wmw2_bridge(BR_CHASSIS));
        /* BridgeAppFocusEvents exists precisely for this: the game raises its
         * own pause menu rather than being frozen from outside. */
        if (wmw2.appfocus_LostFocusShowPauseMenu)
          wmw2.appfocus_LostFocusShowPauseMenu(fake_env, wmw2_bridge(BR_APPFOCUS));
      }
      break;
    }

    case AppletHookType_OnOperationMode:
      /* The presentation is locked to 1280x720 in both modes, so the engine is
       * deliberately NOT told anything changed -- there is no re-layout to go
       * wrong mid-level. Only the window needs resizing. */
      set_screen_size();
      nwindowSetDimensions(nwindowGetDefault(), screen_width, screen_height);
      break;

    default:
      break;
  }
}

/* ------------------------------------------------------------------------- */

int main(void) {
  AppletHookCookie cookie;

  socketInitializeDefault();
  debugPrintf("=== Where's My Water? 2 NX loader ===\n");
  debugPrintf("heap: %zu MB total -- %zu MB newlib (engine + mesa textures), %zu MB modules\n",
              g_heap_total >> 20, g_heap_newlib >> 20, g_heap_so >> 20);

  /* Discover where we were launched from before anything touches the disk. */
  wmw_paths_init();

  /* SDL2 backs the OpenSL ES shim that FMOD plays through (see opensles.c).
   * SDL_SetMainReady() is required because this port provides its own main()
   * rather than SDL_main; without it SDL_Init refuses to start and the only
   * symptom is silence. */
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s -- there will be no sound\n", SDL_GetError());
  else
    debugPrintf("SDL audio subsystem ready\n");

  check_syscalls();
  check_data();
  wmw_assetindex_build();
  /* Assemble the archive the engine will ask for once it starts. Built before
   * anything else touches those files, so a half-written bundle from a previous
   * crashed run cannot be served. */
  wmw_bundle_path(NULL);
  ensure_databases();
  set_screen_size();

  char gcs_dir[WMW_PATH_MAX], migs_dir[WMW_PATH_MAX];
  ensure_work_dirs(gcs_dir, sizeof(gcs_dir), migs_dir, sizeof(migs_dir));

  if (!egl_init())
    fatal_error("Could not create a GLES 1.1 context.\nInstall switch-mesa and\nswitch-libdrm_nouveau.");

  /* Portrait presentation. Must come after egl_init() (it needs a current
   * context) and before the engine is told its size. */
  if (!wmw_tate_init(render_width, render_height,
                     screen_width, screen_height, wmw_rotation_mode()))
    debugPrintf("tate: portrait unavailable -- rendering landscape as-is\n");

  load_two_modules();

  jni_init();
  wmw2_bridges_create();
  touch_arrays_init();

  /* Dalvik called this the moment System.loadLibrary() returned. */
  if (wmw2.JNI_OnLoad) {
    int32_t v = wmw2.JNI_OnLoad(fake_vm, NULL);
    debugPrintf("JNI_OnLoad -> 0x%x\n", v);
  }

  /* The movie player needs a way to put a frame on screen: rotate through
   * wmw_tate exactly as the frame loop does, then swap. It cannot reach the EGL
   * handles itself, so hand it this. */
  wmw2_movie_init(movie_present, render_width, render_height);

  input_init();
  appletHook(&cookie, applet_hook_fn, NULL);

  /* --- 1. every bridge announces itself ---------------------------------- */
  wmw2_bridges_init();

  /* --- 2. Labor_AppStartup ----------------------------------------------- */
  {
    /* jniWalaberChassisStartup's first argument, and it is NOT opened as an
     * archive the way WMW1's was. WMW2 treats it as a DIRECTORY PREFIX and
     * concatenates onto it -- the first boot log shows the engine building
     *
     *     <pkg>/assets/Water/Data/levelinfo.db
     *
     * and doing an ordinary open() on the result. Handing it bundle.zip
     * produced "sdmc:/switch/wmw2_nx/bundle.zip/assets/Water/Data/levelinfo.db",
     * which of course does not exist.
     *
     * That miss was quiet and then fatal. copyDatabaseFromBundle() opens the
     * DESTINATION with "w" before it reads the source, so a failed copy does
     * not leave the old file alone -- it truncates it to zero. Both databases
     * this port had just seeded were emptied, SQLite then read 0 bytes where it
     * wanted a 100-byte header, and the engine eventually called abort().
     * Same for factory_profile.json, store.json and news.json.
     *
     * So: hand it the game directory. <gamedir>/assets/Water/Data/... is
     * exactly where those files live.
     *
     * And NOT the .apk, even when one is present. Because this is string
     * concatenation rather than an archive open, pointing it at an .apk just
     * produces paths inside a file, which no amount of zip support here would
     * fix without a full VFS. WMW1's port passed an archive because WMW1's
     * engine genuinely opened one; WMW2 does not, and that difference is the
     * whole of this bug. */
    const char *pkg_path = wmw_game_dir();

    debugPrintf("chassis startup:\n");
    debugPrintf("  package    = %s\n", pkg_path);
    debugPrintf("  privateDir = %s\n", wmw_game_dir());
    debugPrintf("  sku        = %d\n", WMW2_SKU_ID);
    debugPrintf("  locale     = %s_%s\n", wmw_language_code(), wmw_country_code());
    debugPrintf("  migsDir    = %s\n", migs_dir);
    debugPrintf("  gcsDir     = %s   (engine appends /GCS.db)\n", gcs_dir);

    void *chassis = wmw2_bridge(BR_CHASSIS);
    wmw2.chassis_Startup(fake_env, chassis,
                         jni_make_string(pkg_path),
                         jni_make_string(wmw_game_dir()),
                         jni_make_string(wmw_game_dir()),
                         WMW2_SKU_ID,
                         jni_make_string(wmw_language_code()),
                         jni_make_string(wmw_country_code()),
                         jni_make_string(WMW2_APP_VERSION),
                         jni_make_string(migs_dir),
                         jni_make_string(gcs_dir));
  }

  /* --- 3. DoDeferredSurfaceCreate ---------------------------------------- */
  {
    void *r = wmw2_bridge(BR_RENDERING);
    debugPrintf("calling jniRenderInit(%d, %d, %.1fmm, %.1fmm, %d dpi)\n",
                render_width, render_height,
                (double)WMW2_PANEL_WIDTH_MM, (double)WMW2_PANEL_HEIGHT_MM,
                WMW2_DENSITY_DPI);
    wmw2.render_Init(fake_env, r, render_width, render_height,
                     WMW2_PANEL_WIDTH_MM, WMW2_PANEL_HEIGHT_MM, WMW2_DENSITY_DPI);

    if (wmw2.render_AreaCreated) wmw2.render_AreaCreated(fake_env, r);
    if (wmw2.render_AreaResized) wmw2.render_AreaResized(fake_env, r, render_width, render_height);

    /* NO jniRenderAreaReload HERE. This one line cost several boots.
     *
     * WalaberRenderer.DoDeferredSurfaceCreate() has two mutually exclusive
     * branches, on flagIsFirstCreation:
     *
     *   first creation:   RenderInit(); RenderAreaCreated();
     *                     RenderFlow.ShoutEvent(1); GameFlow.ShoutEvent(5)
     *   later creations:  RenderReloadContextData();
     *                     RenderFlow.ShoutEvent(2); RenderFlow.ShoutEvent(3)
     *
     * The second branch is the Android context-loss path: the surface was
     * destroyed and rebuilt, so every GL object has to be recreated. Calling it
     * at startup tells the engine its context was lost before it ever had one,
     * and it enters Screen_GraphicsContextRestore -- which draws a progress bar,
     * fills it, and then waits for RenderFlow events 2 and 3.
     *
     * Those events are shouted by Java. They are not JNI entry points, there is
     * no native equivalent, and the port has no way to send them. So the engine
     * waits forever on a screen that looks exactly like the loading screen
     * finishing normally, which is why this read as "loads fine, never
     * progresses to the menu" rather than as a crash or a stall. */
  }

  /* --- 4. audio ----------------------------------------------------------
   * Has to come AFTER the Chassis starts: FMOD::System::init() runs in there,
   * and that is when it dlopen()s libOpenSLES and creates its engine. Deciding
   * earlier always sees "no OpenSL engine" and starts the FMODAudioDevice pump
   * as well, putting two writers on the audio device.
   *
   * The engine hard-selects setOutput(21) = FMOD_OUTPUTTYPE_OPENSL with no
   * device query and no fallback, so this should always take the first branch.
   * If it does not, the log line below is the first thing to look at -- and the
   * engine's own "sampleRate[...] format[...]" line just above it in the log
   * gives you the mix rate for the resampler. */
  if (opensles_in_use()) {
    debugPrintf("audio: FMOD is using the OpenSL ES output\n");
  } else {
    debugPrintf("audio: no OpenSL engine -- falling back to the FMODAudioDevice pump\n");
    fmod_audio_start(fmod_getinfo_addr, fmod_process_addr);
  }

  /* --- 5. tell the engine what it is waiting to hear ----------------------
   * On Android these answers arrived from services that started in parallel
   * with the game. Nothing will ever start here, so say so up front. The ad
   * manager in particular holds its subsystem "not ready" until it is told
   * every ad object was created. */
  /* Delayed rather than delivered on frame 1. On Android these came from
   * services that finished initialising well after the first frame, so landing
   * them before the engine has drawn anything is a timing it never saw. They
   * carry no parsed payload so the risk is small, but the profile callback
   * already demonstrated what an unexpected early event costs. */
  wmw2_cb_post_delayed(W2CB_ADS_ALL_CREATED,  0, 0, NULL, 500);
  wmw2_cb_post_delayed(W2CB_REWARD_VIDEO_INIT, 0, 0, NULL, 600);
  wmw2_store_restore();

  /* Deliberately NOT pushing a MIGS profile or store catalogue here.
   *
   * On Android those arrive from a network service some time after launch, and
   * when the device is offline they simply never arrive -- the engine carries
   * on with the local profile it built from factory_profile.json. Volunteering
   * one unasked is not "more complete", it is an extra event the engine did not
   * expect at that point in startup, and delivering an empty one is worse still
   * (it aborts inside stoull -- see wmw2_callbacks.c).
   *
   * If the engine does ask, wmw2_jni.c queues the answer and it goes out with
   * the real document. */

  if (wmw2.chassis_AppResume)
    wmw2.chassis_AppResume(fake_env, wmw2_bridge(BR_CHASSIS));
  if (wmw2.audioinfo_IsSafeToPlay)
    wmw2.audioinfo_IsSafeToPlay(fake_env, wmw2_bridge(BR_AUDIOINFO), 1);

  debugPrintf("entering main loop\n");
  /* Eager logging stays ON for the first few frames.
   *
   * Switching to 32-line batching here was a mistake: the first abort inside
   * frame 1 lost everything after "entering main loop" except the three lines
   * the abort handler flushed itself, so the log could not distinguish "died in
   * our callback" from "died in the engine's first draw". Batching resumes once
   * the game is clearly running -- see the end of the loop. */

  /* Frame pacer.
   *
   * eglSwapInterval(1) is set, but mesa's present is asynchronous --
   * eglSwapBuffers does not block on the panel. Left alone the loop free-runs
   * slightly fast, drifts a fraction of a millisecond every frame, and laps the
   * display roughly once a second. That reads as a periodic hitch rather than
   * as "too fast", which makes it easy to misdiagnose as a performance problem.
   *
   * Holding an explicit deadline fixes the pacing regardless of what the
   * present does. */
  const u64 frame_ticks = armNsToTicks(1000000000ull / WMW2_TARGET_FPS);
  u64 frame_deadline = armGetSystemTick() + frame_ticks;

  int frame = 0;
  u64 acc_render = 0, acc_swap = 0, max_render = 0, max_swap = 0;
  u64 win_start = armGetSystemTick();

  void *rend = wmw2_bridge(BR_RENDERING);

  while (appletMainLoop() && !wmw2_quit_requested) {
    padUpdate(&g_pad);

    if (frame < 4) { debugPrintf("frame %d: input\n", frame); debugLogFlush(); }
    feed_pointer();
    if (frame < 4) { debugPrintf("frame %d: callbacks\n", frame); debugLogFlush(); }
    wmw2_cb_drain();   /* deliver answers to earlier async requests */

    const u64 t_render0 = armGetSystemTick();
    /* Phase markers for the first frames. These are what tell you whether a
     * fault was in the port's callbacks, the engine's pre-draw or its draw --
     * a distinction the log previously could not make. */
    if (frame < 4) { debugPrintf("frame %d: predraw\n", frame); debugLogFlush(); }
    wmw_tate_begin();                       /* bind the portrait target */
    /* Both halves, in this order. BridgeRendering.RenderDrawFrame() called
     * jniRenderDrawPreDraw() then jniRenderDrawFrame(); PreDraw alone renders
     * nothing and DrawFrame alone runs against stale state. */
    if (wmw2.render_DrawPreDraw) wmw2.render_DrawPreDraw(fake_env, rend);
    if (frame < 4) { debugPrintf("frame %d: drawframe\n", frame); debugLogFlush(); }
    wmw2.render_DrawFrame(fake_env, rend);
    if (frame < 4) { debugPrintf("frame %d: drawn\n", frame); debugLogFlush(); }
    nxp_draw();                             /* cursor INTO the portrait target... */
    wmw_tate_present();                     /* ...so the rotation carries it too */
    const u64 t_render1 = armGetSystemTick();

    eglSwapBuffers(s_display, s_surface);
    const u64 t_swap1 = armGetSystemTick();

    const u64 d_render = t_render1 - t_render0;
    const u64 d_swap   = t_swap1  - t_render1;
    acc_render += d_render; acc_swap += d_swap;
    if (d_render > max_render) max_render = d_render;
    if (d_swap   > max_swap)   max_swap   = d_swap;

    /* --- hold to the target rate ----------------------------------------- */
    {
      const s64 remain = (s64)(frame_deadline - armGetSystemTick());
      if (remain > 0) {
        const u64 remain_ns = armTicksToNs((u64)remain);
        /* Sleep the bulk of it -- cheap, and lets the CPU drop clocks -- but
         * busy-wait the final millisecond, which is finer than the scheduler
         * can reliably deliver. */
        if (remain_ns > 1500000ull)
          svcSleepThread((s64)(remain_ns - 1000000ull));
        while ((s64)(frame_deadline - armGetSystemTick()) > 0)
          ; /* spin the last <=1ms */
        frame_deadline += frame_ticks;
      } else {
        /* A genuine spike put us behind. Resync to now rather than advancing
         * the deadline: trying to "catch up" races several frames back to back,
         * which looks worse than the hitch it is compensating for. */
        frame_deadline = armGetSystemTick() + frame_ticks;
      }
    }

    frame++;

    /* Clearly running: stop paying an SD write per line, and stop tracing every
     * upcall. Both are bring-up tools, not steady-state behaviour. */
    if (frame == 12) {
      /* Per-line flushing is the expensive part and is only needed while the
       * first frames are at risk. The upcall trace is cheap by comparison and
       * is deliberately LEFT RUNNING until its own budget expires -- cutting it
       * here is why the last stall showed only four upcalls and no sign of the
       * MIGS conversation that was actually blocking. */
      debugPrintf("frame 12 reached -- switching to batched logging\n");
      debugLogSetEager(0);
    }

    if (frame < 5 || (frame % 120) == 0) {
      const u64 now = armGetSystemTick();
      const u64 win_ns = armTicksToNs(now - win_start);
      const int n = (frame % 120 == 0 && frame) ? 120 : (frame ? frame : 1);
      debugPrintf("frame %d: fps=%.1f render avg=%.1fms max=%.1fms | swap avg=%.1fms max=%.1fms\n",
                  frame, n * 1e9 / (double)(win_ns ? win_ns : 1),
                  armTicksToNs(acc_render) / 1e6 / n, armTicksToNs(max_render) / 1e6,
                  armTicksToNs(acc_swap)   / 1e6 / n, armTicksToNs(max_swap)   / 1e6);
      acc_render = acc_swap = max_render = max_swap = 0;
      win_start = now;
    }

    debugLogFlush();
  }

  /* --- Labor_AppShutdown -------------------------------------------------
   * Chassis first, then the bridges -- the mirror of startup, and the order
   * WalaberGameEmbodiment.Labor_AppShutdown() used. */
  debugPrintf("shutting down\n");
  if (wmw2.chassis_AppPause)     wmw2.chassis_AppPause(fake_env, wmw2_bridge(BR_CHASSIS));
  if (wmw2.render_AreaDestroyed) wmw2.render_AreaDestroyed(fake_env, rend);
  if (wmw2.chassis_Shutdown)     wmw2.chassis_Shutdown(fake_env, wmw2_bridge(BR_CHASSIS));
  wmw2_bridges_done();

  nxp_save_settings();
  fmod_audio_stop();
  appletUnhook(&cookie);
  egl_deinit();
  socketExit();
  return 0;
}
