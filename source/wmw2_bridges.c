/* wmw2_bridges.c -- the 21 subsystem bridge objects
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * See wmw2_bridges.h for why each bridge needs its own object.
 */

#include <stddef.h>

#include "wmw2_bridges.h"
#include "wmw2_entrypoints.h"
#include "jni_fake.h"
#include "util.h"

typedef struct {
  const char *label;   /* what the fake object reports as its class */
  fn_v       *init;    /* &wmw2.<x>_BridgeInit  -- may point at a NULL slot */
  fn_v       *done;    /* &wmw2.<x>_BridgeDone  -- may point at a NULL slot */
} BridgeDesc;

static void *s_obj[BR_COUNT];
static int   s_created;
static int   s_inited;

/* The table is filled in wmw2_bridges_create() rather than statically, because
 * the function-pointer slots live in `wmw2` which is populated at runtime by
 * main.c's resolve_entry_points(). */
static BridgeDesc s_desc[BR_COUNT];

static void fill_desc(void) {
  s_desc[BR_CHASSIS]       = (BridgeDesc){ "com/disney/GameLib/Bridge/WalaberNativeChassis",
                                           &wmw2.chassis_BridgeInit,  &wmw2.chassis_BridgeDone };
  s_desc[BR_RENDERING]     = (BridgeDesc){ "com/disney/GameLib/Bridge/Rendering/BridgeRendering",
                                           &wmw2.render_BridgeInit,   &wmw2.render_BridgeDone };
  s_desc[BR_TOUCH]         = (BridgeDesc){ "com/disney/GameLib/Bridge/DeviceIO/BridgeTouchHandling",
                                           &wmw2.touch_BridgeInit,    &wmw2.touch_BridgeDone };
  s_desc[BR_SENSOR]        = (BridgeDesc){ "com/disney/GameLib/Bridge/DeviceIO/BridgeSensorHandling",
                                           &wmw2.sensor_BridgeInit,   &wmw2.sensor_BridgeDone };
  s_desc[BR_KEYBOARD]      = (BridgeDesc){ "com/disney/GameLib/Bridge/DeviceIO/BridgeKeyboardHandling",
                                           &wmw2.keyboard_BridgeInit, &wmw2.keyboard_BridgeDone };
  s_desc[BR_APPFOCUS]      = (BridgeDesc){ "com/disney/GameLib/Bridge/AppEvents/BridgeAppFocusEvents",
                                           &wmw2.appfocus_BridgeInit, &wmw2.appfocus_BridgeDone };
  s_desc[BR_AUDIOINFO]     = (BridgeDesc){ "com/disney/GameLib/Bridge/AppEvents/BridgeAudioAppInfo",
                                           &wmw2.audioinfo_BridgeInit,&wmw2.audioinfo_BridgeDone };
  s_desc[BR_GAMEFLOW]      = (BridgeDesc){ "com/disney/GameLib/Bridge/AppEvents/BridgeGameFlowEvents",
                                           &wmw2.gameflow_BridgeInit, &wmw2.gameflow_BridgeDone };
  /* The next three declare a jniBridgeDone on the Java side that libwalaber.so
   * does not export. Only Init exists; the done slot stays NULL and is skipped. */
  s_desc[BR_COMPAT]        = (BridgeDesc){ "com/disney/GameLib/Bridge/AppEvents/CompatibilityIssue/BridgeCompatibilityIssue",
                                           &wmw2.compat_BridgeInit,   NULL };
  s_desc[BR_FORCEUPDATE]   = (BridgeDesc){ "com/disney/GameLib/Bridge/AppEvents/ForceUpdate/BridgeForceUpdate",
                                           &wmw2.forceupdate_BridgeInit, NULL };
  s_desc[BR_GOOGLECMP]     = (BridgeDesc){ "com/disney/GameLib/Bridge/AppEvents/GoogleConsentsUpdate/BridgeGoogleCMPUpdate",
                                           &wmw2.googlecmp_BridgeInit, NULL };
  s_desc[BR_LAYOUT]        = (BridgeDesc){ "com/disney/GameLib/Bridge/Display/BridgeWalaberCustomLayout",
                                           &wmw2.layout_BridgeInit,   &wmw2.layout_BridgeDone };
  s_desc[BR_VIDEOVIEW]     = (BridgeDesc){ "com/disney/GameLib/Bridge/Display/BridgeWalaberVideoView",
                                           &wmw2.videoview_BridgeInit,&wmw2.videoview_BridgeDone };
  s_desc[BR_TEXT]          = (BridgeDesc){ "com/disney/GameLib/Bridge/Text/BridgeTextL18Ning",
                                           &wmw2.text_BridgeInit,     &wmw2.text_BridgeDone };
  s_desc[BR_NET]           = (BridgeDesc){ "com/disney/GameLib/Bridge/Net/BridgeNetGeneral",
                                           &wmw2.net_BridgeInit,      &wmw2.net_BridgeDone };
  s_desc[BR_ADVERTS]       = (BridgeDesc){ "com/disney/GameLib/Bridge/Net/Adverts/BridgeAdverts",
                                           &wmw2.adverts_BridgeInit,  &wmw2.adverts_BridgeDone };
  s_desc[BR_MIGS]          = (BridgeDesc){ "com/disney/GameLib/Bridge/DisMoLibs/BridgeDisMoMigs",
                                           &wmw2.migs_BridgeInit,     &wmw2.migs_BridgeDone };
  s_desc[BR_ANALYTICS]     = (BridgeDesc){ "com/disney/GameLib/Bridge/DisMoLibs/BridgeDisMoAnalyticals",
                                           &wmw2.analytics_BridgeInit,&wmw2.analytics_BridgeDone };
  s_desc[BR_REFERRALSTORE] = (BridgeDesc){ "com/disney/GameLib/Bridge/DisMoLibs/BridgeDisMoReferralStore",
                                           &wmw2.referralstore_BridgeInit, &wmw2.referralstore_BridgeDone };
}

void wmw2_bridges_create(void) {
  if (s_created)
    return;
  fill_desc();
  for (int i = 0; i < BR_COUNT; i++) {
    s_obj[i] = jni_make_object(s_desc[i].label);
    /* Every bridge object lives for the whole process and the engine holds a
     * reference to each one from jniBridgeInit until jniBridgeDone. As plain
     * local refs they would be released at the first PopLocalFrame the engine
     * performs, after which every callback dispatches against freed memory --
     * the failure lands inside free() much later with only engine frames on the
     * stack. Pin them. (4.5 in the platform notes; it cost real time on WMW1
     * with the touch arrays, and this is the same mistake one object over.) */
    jni_pin(s_obj[i]);
  }
  s_created = 1;
  debugPrintf("bridges: created %d bridge objects\n", BR_COUNT);
}

void *wmw2_bridge(wmw2_bridge_id id) {
  if (id < 0 || id >= BR_COUNT)
    return NULL;
  if (!s_created)
    wmw2_bridges_create();
  return s_obj[id];
}

void wmw2_bridges_init(void) {
  if (s_inited)
    return;
  if (!s_created)
    wmw2_bridges_create();

  int done = 0, missing = 0;
  for (int i = 0; i < BR_COUNT; i++) {
    fn_v *slot = s_desc[i].init;
    if (!slot || !*slot) {
      missing++;
      debugPrintf("bridges: no jniBridgeInit for %s\n", s_desc[i].label);
      continue;
    }
    (*slot)(fake_env, s_obj[i]);
    done++;
  }
  s_inited = 1;
  debugPrintf("bridges: jniBridgeInit x%d (%d unavailable)\n", done, missing);
}

void wmw2_bridges_done(void) {
  if (!s_inited)
    return;
  /* Reverse order: the Chassis is torn down last, as on Android, because the
   * other bridges deregister from objects it owns. */
  for (int i = BR_COUNT - 1; i >= 0; i--) {
    fn_v *slot = s_desc[i].done;
    if (slot && *slot)
      (*slot)(fake_env, s_obj[i]);
  }
  s_inited = 0;
  debugPrintf("bridges: jniBridgeDone complete\n");
}
