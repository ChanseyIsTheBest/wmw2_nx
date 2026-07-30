/* wmw2_bridges.h -- the 21 subsystem bridge objects WMW2 splits its JNI across
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * WMW1 had ONE Java object on the other side of the JNI wall
 * (com.disney.common.BaseActivity) and one renderer. WMW2 has twenty-one,
 * under com.disney.GameLib.Bridge.*, each constructed independently on the Java
 * side and each announcing itself to the engine through its own
 * jniBridgeInit(). The engine caches the calling object per subsystem and
 * later invokes that subsystem's callbacks on it.
 *
 * So the port has to do three things WMW1 never had to:
 *
 *   1. Hand each bridge a DISTINCT object. Passing the same pointer to all
 *      twenty-one works right up until the engine stores two of them in the
 *      same slot and starts calling BridgeRendering's methods on what it thinks
 *      is BridgeDisMoMigs.
 *
 *   2. Call jniBridgeInit in the right ORDER. On Android the order falls out of
 *      construction order in WalaberGameEmbodiment and BaseActivity; the
 *      Chassis must be first, because jniWalaberChassisStartup builds the
 *      objects the other bridges register against.
 *
 *   3. Tear down symmetrically. Labor_AppShutdown() calls
 *      WalaberChassisShutdown() and only then Bridge_Dispose() -> jniBridgeDone
 *      on each bridge.
 *
 * Every jniBridgeInit / jniBridgeDone in the build is a no-arg ()V, so the only
 * thing that distinguishes them is which object is passed as `thiz`.
 */

#ifndef __WMW2_BRIDGES_H__
#define __WMW2_BRIDGES_H__

/* Stable ids for the bridges the port actually talks to. The order here is the
 * order jniBridgeInit is called in. */
typedef enum {
  BR_CHASSIS = 0,      /* WalaberNativeChassis      -- must be first */
  BR_RENDERING,        /* Rendering.BridgeRendering */
  BR_TOUCH,            /* DeviceIO.BridgeTouchHandling */
  BR_SENSOR,           /* DeviceIO.BridgeSensorHandling */
  BR_KEYBOARD,         /* DeviceIO.BridgeKeyboardHandling */
  BR_APPFOCUS,         /* AppEvents.BridgeAppFocusEvents */
  BR_AUDIOINFO,        /* AppEvents.BridgeAudioAppInfo */
  BR_GAMEFLOW,         /* AppEvents.BridgeGameFlowEvents */
  BR_COMPAT,           /* AppEvents.CompatibilityIssue.BridgeCompatibilityIssue */
  BR_FORCEUPDATE,      /* AppEvents.ForceUpdate.BridgeForceUpdate */
  BR_GOOGLECMP,        /* AppEvents.GoogleConsentsUpdate.BridgeGoogleCMPUpdate */
  BR_LAYOUT,           /* Display.BridgeWalaberCustomLayout */
  BR_VIDEOVIEW,        /* Display.BridgeWalaberVideoView */
  BR_TEXT,             /* Text.BridgeTextL18Ning */
  BR_NET,              /* Net.BridgeNetGeneral */
  BR_ADVERTS,          /* Net.Adverts.BridgeAdverts */
  BR_MIGS,             /* DisMoLibs.BridgeDisMoMigs */
  BR_ANALYTICS,        /* DisMoLibs.BridgeDisMoAnalyticals */
  BR_REFERRALSTORE,    /* DisMoLibs.BridgeDisMoReferralStore */
  BR_COUNT
} wmw2_bridge_id;

/* Create the objects. Call after jni_init(), before any entry point. */
void wmw2_bridges_create(void);

/* The `thiz` for a given bridge. Never NULL after wmw2_bridges_create(). */
void *wmw2_bridge(wmw2_bridge_id id);

/* Run every jniBridgeInit in order. Must precede jniWalaberChassisStartup. */
void wmw2_bridges_init(void);

/* Run every jniBridgeDone in reverse order, after jniWalaberChassisShutdown. */
void wmw2_bridges_done(void);

#endif
