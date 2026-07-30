/* wmw2_entrypoints.h -- the exported JNI entry points of libwalaber.so
 *                       (Where's My Water? 2, com.disney.wheresmywater2_goo)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * libwalaber.so exports 92 Java_* symbols plus JNI_OnLoad, all *statically*
 * named, so every one resolves by name straight out of the dynamic symbol
 * table -- nothing goes through RegisterNatives, exactly as in WMW1.
 *
 * Unlike WMW1's two flat classes, WMW2 splits the bridge into 21 subsystem
 * classes under com.disney.GameLib.Bridge.*, each with its own
 * jniBridgeInit / jniBridgeDone pair. Every one of those pairs is a no-arg
 * ()V; the init caches the calling object's class and looks its Java-side
 * callbacks up with GetMethodID, which is why the fake JNI must return a
 * stable, distinct object per bridge.
 *
 * Signatures below come from classes*.dex (six multidex files), which declares
 * every native method explicitly. They were cross-checked against the
 * disassembly of each entry point. Three do not match what the disassembly
 * alone suggests, so the dex mattered again:
 *
 *   - jniRenderInit takes FIVE arguments (I I F F I), not three. The two
 *     floats pass in s0/s1 and are forwarded untouched, so a register-use
 *     scan of the prologue never sees them.
 *   - jniWalaberChassisStartup takes NINE arguments, not six. AArch64 puts
 *     the first six in x2..x7 and the remaining three on the stack.
 *   - jniAccelerometerChanged takes THREE floats, not two; s2 is unused in
 *     the prologue.
 *
 * Slot numbering note (same as WMW1): the engine reads the touch arrays with
 * JNIEnv slot 0x5e8/8 = 189 = GetFloatArrayElements for the coordinates and
 * 0x5d8/8 = 187 = GetIntArrayElements for the ids, matching (I[F[F[I)V.
 */

#ifndef __WMW2_ENTRYPOINTS_H__
#define __WMW2_ENTRYPOINTS_H__

#include <stdint.h>

/* Every entry point takes the fake JNIEnv* and the fake bridge object as its
 * first two arguments, exactly as the Dalvik ABI would. jboolean widens to
 * int32_t in the AArch64 calling convention. */
typedef void    (*fn_v)      (void *env, void *thiz);
typedef void    (*fn_i)      (void *env, void *thiz, int32_t a);
typedef void    (*fn_z)      (void *env, void *thiz, int32_t a);
typedef void    (*fn_ii)     (void *env, void *thiz, int32_t a, int32_t b);
typedef void    (*fn_iii)    (void *env, void *thiz, int32_t a, int32_t b, int32_t c);
typedef void    (*fn_fff)    (void *env, void *thiz, float x, float y, float z);
typedef void    (*fn_str)    (void *env, void *thiz, void *jstr);
typedef void    (*fn_zzstr)  (void *env, void *thiz, int32_t a, int32_t b, void *jstr);
typedef void    (*fn_strstr) (void *env, void *thiz, void *a, void *b);
typedef void    (*fn_strarr) (void *env, void *thiz, void *jstrArray);
typedef void   *(*fn_r_str)  (void *env, void *thiz);              /* -> jstring       */
typedef void   *(*fn_str_str)(void *env, void *thiz, void *jstr);  /* -> jstring       */
typedef void   *(*fn_r_strarr)(void *env, void *thiz);             /* -> jobjectArray  */
typedef int32_t (*fn_r_i)    (void *env, void *thiz);
typedef int32_t (*fn_r_z)    (void *env, void *thiz);
typedef float   (*fn_r_f)    (void *env, void *thiz);
typedef int32_t (*fn_onload) (void *vm, void *reserved);

/* jniWalaberChassisStartup -- the one call that hands the engine its world.
 *
 * Recovered from WalaberGameEmbodiment.Labor_AppStartup(), which sources every
 * argument before invoking WalaberChassisStartup():
 *
 *   apkPath      StorageLocations.AppDataPath_PackageFilename()
 *                  the APK file itself; the engine opens it as an archive
 *   privateDir   StorageLocations.AppDataPath_PrivateDir()
 *                  writable app-private directory (saves live here)
 *   cacheDir     StorageLocations.AppDataPath_PrivateCacheDir().getPath()
 *   skuId        ConvertSkuToJniSkuID(SkuMetaConfig.GetSkuMarketTypeId())
 *                  storefront id; the Google build is the only SKU shipped
 *   language     Locale.getDefault().getLanguage()   e.g. "en"
 *   country      Locale.getDefault().getCountry()    e.g. "US"
 *   versionLabel AppPackageInfo.GetAppExtendedVersionLabel()
 *   migsWorkDir  StorageLocations.GetMigsWorkDirPath()
 *   gcsWorkDir   StorageLocations.GetGcsWorkDirPath()
 *                  the engine appends "/GCS.db" -- this is the SQLite file,
 *                  so 4.1/4.2 (stat-while-open, read-past-EOF) apply here.
 *
 * Note the last three arguments arrive on the stack, not in registers. */
typedef void (*fn_chassis_startup)(void *env, void *thiz,
                                   void *apkPath, void *privateDir, void *cacheDir,
                                   int32_t skuId,
                                   void *language, void *country, void *versionLabel,
                                   void *migsWorkDir, void *gcsWorkDir);

/* jniRenderInit(int widthPx, int heightPx, float widthMM, float heightMM, int dpi)
 *
 * From BridgeRendering.RenderInit(), which computes the millimetre figures as
 *     widthMM  = (widthPixels  / DisplayMetrics.xdpi) * 25.4f
 *     heightMM = (heightPixels / DisplayMetrics.ydpi) * 25.4f
 * and passes DisplayMetrics.densityDpi as the final int. The engine logs them
 * back as "Render area dimensions: %.2fx%.2fpx ... %0.2fx%0.2fmm dpi:%d".
 *
 * The prologue also sets an internal "extreme aspect" flag when
 * 2*width <= height or 2*height <= width. A rotated 720x1280 Switch panel
 * trips neither test, so the flag stays clear -- which is what you want. */
typedef void (*fn_render_init)(void *env, void *thiz,
                               int32_t w_px, int32_t h_px,
                               float w_mm, float h_mm, int32_t dpi);

/* jniTouchBegan / jniTouchEnded (int count, float[] x, float[] y, int[] ids)
 * jniTouchMoved (int count, float[] x, float[] y, float[] prevX, float[] prevY, int[] ids)
 *
 * WMW2 batches contacts: one call carries every contact that changed this
 * frame, rather than WMW1's one call per finger. The engine pulls each array
 * with Get*ArrayElements and drops it again with Release*ArrayElements, but
 * the pool it copies into is bounded -- it logs "Per-Frame Touch Down count
 * met or exceeded expectations" once count reaches 11, so keep batches <= 10.
 *
 * These arrays are created once and refilled each frame, so they MUST be
 * jni_pin()'d (see 4.5): the engine is entitled to drop the local refs, and
 * free_ref() would take the backing store with them. */
typedef void (*fn_touch)      (void *env, void *thiz, int32_t count,
                               void *xs, void *ys, void *ids);
typedef void (*fn_touch_moved)(void *env, void *thiz, int32_t count,
                               void *xs, void *ys,
                               void *prev_xs, void *prev_ys, void *ids);

typedef struct {
  fn_onload JNI_OnLoad;

  /* --- WalaberNativeChassis: process lifecycle ---------------------------
   * Order observed on Android: the bridge objects are constructed (each ctor
   * calls its own jniBridgeInit) and only then does Labor_AppStartup() call
   * WalaberChassisStartup. Shutdown is Startup's mirror: ChassisShutdown
   * first, then Bridge_Dispose -> jniBridgeDone. */
  fn_v               chassis_BridgeInit;
  fn_v               chassis_BridgeDone;
  fn_chassis_startup chassis_Startup;
  fn_v               chassis_Shutdown;
  fn_v               chassis_AppPause;
  fn_v               chassis_AppResume;

  /* --- Rendering.BridgeRendering: the frame loop -------------------------
   * WalaberRenderer.DoDeferredSurfaceCreate() runs, in order:
   *     RenderInit() -> jniRenderInit
   *     RenderAreaCreated() -> jniRenderAreaCreated
   *     ... RenderFlow/GameFlow events ...
   *     RenderReloadContextData() -> jniRenderAreaReload
   * and every frame RenderDrawFrame() calls jniRenderDrawPreDraw() then
   * jniRenderDrawFrame(). Both halves are required; PreDraw alone renders
   * nothing and DrawFrame alone runs against stale state. */
  fn_render_init render_Init;
  fn_v           render_AreaCreated;
  fn_ii          render_AreaResized;      /* (widthPx, heightPx) */
  fn_v           render_AreaReload;       /* re-upload GL objects after context loss */
  fn_v           render_AreaDestroyed;
  fn_v           render_DrawPreDraw;
  fn_v           render_DrawFrame;
  fn_v           render_BridgeInit;
  fn_v           render_BridgeDone;

  /* --- DeviceIO ---------------------------------------------------------- */
  fn_v           touch_BridgeInit;
  fn_v           touch_BridgeDone;
  fn_touch       touch_Began;
  fn_touch_moved touch_Moved;
  fn_touch       touch_Ended;

  fn_v   sensor_BridgeInit;
  fn_v   sensor_BridgeDone;
  fn_fff sensor_AccelerometerChanged;     /* (x, y, z) -- optional; gyro can drive it */

  fn_v keyboard_BridgeInit;
  fn_v keyboard_BridgeDone;
  fn_v keyboard_BackKeyPressed;

  /* --- AppEvents --------------------------------------------------------- */
  fn_v appfocus_BridgeInit;
  fn_v appfocus_BridgeDone;
  fn_v appfocus_LostFocusShowPauseMenu;

  fn_v audioinfo_BridgeInit;
  fn_v audioinfo_BridgeDone;
  fn_z audioinfo_IsSafeToPlay;            /* (Z)V -- pass 1; there is no phone call to duck for */

  fn_v gameflow_BridgeInit;
  fn_v gameflow_BridgeDone;

  /* These three are network-gated flows. Each needs a positive "nothing to do"
   * answer rather than silence -- see 6. Note the dex declares a jniBridgeDone
   * for all three that libwalaber.so does not export; only Init exists. */
  fn_v compat_BridgeInit;
  fn_v compat_ShowMessage;
  fn_v forceupdate_BridgeInit;
  fn_v forceupdate_SendSignalToGameCode;
  fn_v googlecmp_BridgeInit;

  /* --- Display ----------------------------------------------------------- */
  fn_v layout_BridgeInit;
  fn_v layout_BridgeDone;
  fn_v layout_NotifyMovieFinished;        /* answer immediately; see below */

  fn_v    videoview_BridgeInit;
  fn_v    videoview_BridgeDone;
  fn_r_f  videoview_RequestVideoVolume;   /* ()F -- engine asks; return 0.0f..1.0f */

  /* --- Text / Net -------------------------------------------------------- */
  fn_v        text_BridgeInit;
  fn_v        text_BridgeDone;
  fn_str_str  text_GetLocalizedText;      /* (String)String */

  fn_v        net_BridgeInit;
  fn_v        net_BridgeDone;
  fn_str_str  net_GetLocalizedString;     /* (String)String */

  /* --- Net.Adverts: stub, but answer ------------------------------------- */
  fn_v   adverts_BridgeInit;
  fn_v   adverts_BridgeDone;
  fn_iii adverts_NotifyAdEvent;           /* (III)V */
  fn_v   adverts_NotifyAllAdsCreated;     /* ()V  -- the engine waits for this */
  fn_str adverts_NotifyHandleUrl;         /* (String)V */
  fn_ii  adverts_NotifyRewardCurrency;    /* (II)V */

  /* --- DisMoLibs.BridgeDisMoMigs: the request/response half ---------------
   * Every jniMigsRequest* upcall the engine makes is asynchronous on Android:
   * BridgeDisMoMigs forwards it to MigsRelay and returns immediately, and the
   * answer arrives later through one of the entry points below. If the port
   * makes the request-side answer but never calls the matching notify, the
   * engine waits forever on a screen that otherwise animates correctly --
   * WMW1's hardest-to-spot bug, in a new dress.
   *
   *   engine upcall (see wmw2_upcalls)      ->  answer with
   *   -----------------------------------------------------------------------
   *   jniMigsRequestProfileReload/Reset/... ->  UpdateGameWithMigsProfile
   *   jniMigsRequestStoreReloadStoreItems   ->  UpdateGameWithMigsStoreItems
   *   jniMigsRequestStoreGetSingleStoreItem ->  NotifyStoreSingleItemInfoResult
   *   jniMigsRequestPurchaseProduct         ->  NotifyStoreProductPurchased
   *   jniMigsRequestRestorePurchases        ->  NotifyStoreProductToRestore (n times)
   *                                             or NotifyStoreNothingToRestore
   *   jniMigsRequestRewardsGetBalance       ->  NotifyRewardsBalance
   *   jniMigsRequestRewardsConsume          ->  NotifyRewardsConsumed
   *   jniMigsRequestGiftsConsume            ->  NotifyGiftConsumed
   *   jniMigsRequestLeaderboardGetInfo      ->  NotifyLeaderboard
   *   jniMigsRequestCustCareSendEmail       ->  NotifyCustCareEmailResult
   *   jniMigsRequestInitRewardVideo         ->  RewardVideoInit
   *   jniMigsRequestVideoAd                 ->  RewardVideoCanceled/Completed/Failed
   *
   * The jniMigsImmediate* upcalls are the exception: they are synchronous and
   * return a JSON string inline, so they need no notify at all. */
  fn_v         migs_BridgeInit;
  fn_v         migs_BridgeDone;
  fn_r_strarr  migs_GetRewardVideoIdsToPreload;  /* ()[String -- engine builds the array */
  fn_r_i       migs_GetUserAge;                  /* ()I */
  fn_r_str     migs_GetUserPreferredLang;        /* ()String */
  fn_r_z       migs_HideDownloadingPopup;        /* ()Z */
  fn_v         migs_ShowDownloadingPopup;        /* ()V */
  fn_z         migs_NotifyCustCareEmailResult;   /* (Z)V */
  fn_i         migs_NotifyGiftConsumed;          /* (I)V */
  fn_str       migs_NotifyLeaderboard;           /* (String)V */
  fn_i         migs_NotifyRewardsBalance;        /* (I)V */
  fn_i         migs_NotifyRewardsConsumed;       /* (I)V */
  fn_v         migs_NotifyStoreNothingToRestore; /* ()V */
  fn_zzstr     migs_NotifyStoreProductPurchased; /* (ZZString)V -- ok, isRestore, sku */
  fn_strarr    migs_NotifyStoreProductToRestore; /* ([String)V */
  fn_strstr    migs_NotifyStoreSingleItemInfoResult; /* (String,String)V */
  fn_str       migs_RewardVideoCanceled;         /* (String)V */
  fn_str       migs_RewardVideoCompleted;        /* (String)V */
  fn_str       migs_RewardVideoFailed;           /* (String)V */
  fn_z         migs_RewardVideoInit;             /* (Z)V */
  fn_str       migs_UpdateGameWithMigsGiftItems;  /* (String)V -- JSON */
  fn_str       migs_UpdateGameWithMigsNewsItems;  /* (String)V -- JSON */
  fn_str       migs_UpdateGameWithMigsProfile;    /* (String)V -- JSON */
  fn_str       migs_UpdateGameWithMigsStoreItems; /* (String)V -- JSON */

  fn_v analytics_BridgeInit;
  fn_v analytics_BridgeDone;
  fn_v referralstore_BridgeInit;
  fn_v referralstore_BridgeDone;

  /* --- DisMoLibs.BridgeDisMoAbtest ---------------------------------------
   * Exported by libwalaber.so but with NO corresponding Java class anywhere in
   * the six dex files -- dead in this build, so the signature below is inferred
   * from the disassembly (one reference argument) rather than declared.
   * Leaving these NULL is safe; nothing on the Java side ever called them. */
  fn_v   abtest_BridgeInit;
  fn_v   abtest_BridgeDone;
  fn_str abtest_DeliverAbtConfigData;      /* (String)V, inferred */

  /* --- Junction.JunctionTester: self-test scaffolding --------------------
   * A JNI round-trip test harness the developers left in. Never called during
   * normal play; useful as a bring-up smoke test for the fake JNI, since
   * jniConfirmStaticConnect is the ONLY entry point resolved with
   * GetStaticMethodID rather than GetMethodID. */
  fn_v junction_ConfirmNonStaticConnect;
  fn_v junction_ConfirmStaticConnect;      /* static */
  fn_v junction_DoLoggerTests;
  fn_v junction_ManipulateManagedField;
} wmw2_entry_points;

extern wmw2_entry_points wmw2;

#endif
