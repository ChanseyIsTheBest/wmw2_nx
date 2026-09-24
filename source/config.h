/* config.h -- build-time constants for the Where's My Water? 2 Switch port
 *
 * Where's My Water? 2 (com.disney.wheresmywater2_goo).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

/* Memory reserved for the .so load zone -- the game's native libraries and
 * nothing else. libwalaber.so is ~16.8 MB and libfmodex.so ~1.1 MB, so 48 MB
 * is comfortable; everything NOT reserved here goes to newlib.
 *
 * newlib is deliberately not capped. It carries the engine's own allocations
 * AND every GPU buffer mesa allocates, since nouveau_bo_new() ends up in
 * memalign(). Reserving a fixed slab for the engine and handing the rest to the
 * load zone gets this exactly backwards -- on WMW1 it left 2.8 GB idle while
 * texture uploads failed inside malloc(). WMW2's library is more than twice the
 * size, so the zone grew; the principle did not. See __libnx_initheap in main.c.
 */
#define SO_ZONE_MB 48

/* Two native modules, loaded in the order the Android activity loads them.
 * libfmodex MUST be loaded and relocated before libwalaber is resolved:
 * libwalaber imports 45 FMOD entry points and so_resolve binds them by walking
 * the already-loaded modules once the static import table has been consulted.
 *
 * libquack.so (Duktape) is deliberately absent. It is not in libwalaber's
 * DT_NEEDED -- on Android it was pulled in by System.loadLibrary("quack") from
 * the Java ad stack, which does not exist here. */
#define FMOD_SO_NAME "libfmodex.so"
#define SO_NAME      "libwalaber.so"

#define LOG_NAME     "debug.log"

/* Vestigial, and kept only because obb.c still references it.
 *
 * WMW1 could be shipped with a main.obb expansion file; WMW2 is not, and
 * libwalaber.so imports no AAssetManager_* symbols at all, so obb.c and the
 * AAssetManager path in libc_shim.c are both unreachable here. They are left in
 * rather than surgically removed because deleting them means editing
 * libc_shim.c, which is otherwise byte-identical to the WMW1 port and worth
 * keeping that way. obb_find() simply never finds anything. */
#define OBB_NAME     "main.obb"

/* WMW2's assets live one level deeper than WMW1's: assets/Water/... rather
 * than assets/... The engine emits paths as "/Water/Textures/foo.imagelist",
 * so the asset ROOT is still <gamedir>/assets and the engine supplies the
 * "Water/" component itself. Do not fold it into the root or every path gains
 * a second copy. See wmw_paths.c. */
#define WMW2_ASSET_SUBDIR "Water"

/* jniWalaberChassisStartup's 4th argument: the storefront id.
 *
 * Traced through classes2.dex rather than guessed, because the two mappings
 * compose and the intermediate value is not the one the engine wants:
 *
 *   SkuMetaConfig.ConvertMarketAndTypeToEnum("free", "google")  -> 10
 *   WalaberNativeChassis.ConvertSkuToJniSkuID(10)               -> 2
 *
 * (11 maps to 1, and anything else logs "Untranslated SKU ID" and returns 0.)
 * This build is com.disney.wheresmywater2_goo -- free, Google -- so the engine
 * expects 2. I had 1 here first, which is the Amazon storefront. */
#define WMW2_SKU_ID 2

/* jniWalaberChassisStartup's 7th argument, reported back in analytics and the
 * debug menu's version line. */
#define WMW2_APP_VERSION "1.9.6"

/* Cutscene playback for assets/Water/Movies/ (eight H.264 clips).
 *
 * Set to 0 to build without it: wmw2_movie.c compiles to empty stubs needing no
 * ffmpeg header and no ffmpeg library, and the engine is told each movie
 * finished immediately -- exactly the behaviour before the feature existed.
 *
 * Requires switch-ffmpeg:   dkp-pacman -S switch-ffmpeg */
#define WMW2_VIDEO 1

/* Per-line SD-card writes are slow; set to 0 for release builds. */
#define DEBUG_LOG 0

/* SDL audio period, in 48 kHz frames. The Switch SDL backend double-buffers at
 * this size, so output latency is roughly 1-2x this on top of FMOD's own queue
 * (see WMW2_FMOD_DSP_BUFFER_LENGTH). 1024 = 21.3 ms per period. 512 is snappier
 * still; go back up to 2048 only if you hear crackle (with DEBUG_LOG on, the
 * opensles heartbeat line shows a rising underrun count). Named WMW_ rather
 * than WMW2_ because opensles.c is shared verbatim with the WMW1 port. */
#define WMW_AUDIO_SAMPLES 1024

/* FMOD DSP block length, in frames at FMOD's 24 kHz mix rate.
 *
 * The engine asks FMOD for 1024 x 4 before init. FMOD's OpenSL output keeps the
 * newest mixed block (numbuffers - 1) blocks from the play head, so that alone
 * puts ~128 ms between a sound starting and it being heard. 512 is FMOD's own
 * default and what WMW1 runs on the identical libfmodex (~64 ms). Measured tap
 * to audible with the OpenSL shim fix: 1024 -> ~181 ms, 512 -> ~113 ms.
 *
 * 0 passes the engine's request through untouched. See wmw2_fmod.c. */
#define WMW2_FMOD_DSP_BUFFER_LENGTH 512

/* Frame rate the main loop is held to. The panel is 60 Hz and the engine has no
 * internal limiter, so this is what actually paces the game. */
#define WMW2_TARGET_FPS 60

/* jniRenderInit's 3rd/4th arguments: physical panel size in millimetres.
 *
 * BridgeRendering.RenderInit() computed these as
 *     (widthPixels / DisplayMetrics.xdpi) * 25.4f
 * so they must describe the display AS THE ENGINE SEES IT, which is portrait.
 * The Switch panel is 6.2" 16:9, i.e. 137.2 x 77.2 mm held normally, so 77.2
 * wide by 137.2 tall turned on its side -- ~237 dpi at the panel's own 720x1280.
 *
 * The engine renders at 1080x1920 (see render_* below), so both figures are
 * scaled by the same 1.5x. That keeps pixels / millimetres -- and therefore
 * every physical size the engine derives -- exactly where it was at 720p, and
 * consistent with WMW2_DENSITY_DPI, which the engine is also handed. Scaling
 * only the pixel size would have told the engine it had a 356 dpi display.
 * (WMW2 ships a single texture set: none of the 3,509 asset paths in
 * libwalaber.so carry a resolution-tier suffix, so there is no art tier for a
 * DPI change to reach for. Keeping the DPI fixed is about layout, not assets.)
 *
 * Pairing landscape millimetres with a portrait pixel size instead gives 133
 * dpi across and 421 dpi down: not a real display, and the engine derives a
 * nonsense scale from it. This was a real WMW1 bug; the arithmetic moved from
 * getDisplayWidthInMM() into an argument, but the trap is identical. */
#define WMW2_PANEL_WIDTH_MM  115.8f   /*  77.2 * 1.5 */
#define WMW2_PANEL_HEIGHT_MM 205.8f   /* 137.2 * 1.5 */

/* jniRenderInit's 5th argument: DisplayMetrics.densityDpi. Keep it consistent
 * with the millimetre figures above -- the engine cross-checks them. Unchanged
 * by the 1080p render size, because the millimetres scaled with it. */
#define WMW2_DENSITY_DPI 237

/* The touchscreen digitizer's native resolution. A HARDWARE constant: the Switch
 * reports touches in 1280x720 panel space regardless of what the app renders
 * at, so this must never follow screen_width/screen_height. It only happened to
 * equal them while the swapchain was 1280x720; at 1920x1080 using the screen
 * size here would put every touch in the wrong place. */
#define WMW2_TOUCH_PANEL_W 1280
#define WMW2_TOUCH_PANEL_H 720

/* WMW2 is a portrait game, confirmed from the shipped UI layouts:
 * assets/Water/Data/SN_MainMenu.xml declares its screen-base widget as
 * forceAspect="1257:2047" and every background plate is portrait
 * (1536:2230, 1700:2230). The engine renders into a portrait framebuffer which
 * is rotated 90 degrees onto the landscape panel, so you turn the console on
 * its side and play it like a phone. See wmw_tate.c.
 *
 * Three coordinate spaces, and it matters which is which:
 *
 *   screen_* -- the real landscape swapchain. Locked to 1920x1080 in BOTH
 *               handheld and docked, so the presentation is identical either
 *               way and the engine is never asked to re-lay-out on a
 *               dock/undock. Handheld, the OS downscales it to the 720p panel
 *               (a supersampled image); docked, it is presented 1:1.
 *   render_* -- the portrait size the ENGINE believes it has (1080x1920),
 *               passed to jniRenderInit and jniRenderAreaResized. Rotated, it
 *               lands 1:1 on the swapchain: no scaling, no letterboxing.
 *   touch    -- WMW2_TOUCH_PANEL_W/H above. Fixed by the hardware, and not the
 *               same thing as screen_* any more.
 *
 * Cost: the engine's own draws, the fluid simulation included, cover 2.25x the
 * pixels they did at 720p. WMW1 measured 3-6 ms per frame at this size, well
 * inside the 16.7 ms budget; worth watching the frame log if WMW2 differs.
 *
 * Note 1080x1920 trips neither half of the engine's internal "extreme aspect"
 * test in jniRenderInit (2*w <= h or 2*h <= w), so the flag it would otherwise
 * set stays clear. That is what we want: it is a layout fallback for 2:1+
 * phones, not for this. */
extern int screen_width;
extern int screen_height;
extern int render_width;
extern int render_height;

/* Presentation, from <gamedir>/config.txt (see config.c):
 *   pillarbox = 0 | 1   upright and pillarboxed instead of rotated
 *   rotation  = 1 | 2   which way to rotate when not pillarboxed
 * Holding LS for 2 s flips pillarbox live, in memory only. */
int  wmw_rotation_mode(void);      /* WMW_TATE_CW or WMW_TATE_CCW */
int  wmw_pillarbox_enabled(void);
void wmw_set_pillarbox_enabled(int enabled);

/* The single place pillarbox and rotation are combined into the one mode value
 * that both wmw_tate_init() and NxpConfig.rotation take, so the picture and the
 * input mapping can never disagree about which mode is active. */
int  wmw_tate_mode(void);

/* Language / country reported to the engine through jniWalaberChassisStartup,
 * derived from the Switch system language at boot. */
const char *wmw_language_code(void);
const char *wmw_country_code(void);

#endif
