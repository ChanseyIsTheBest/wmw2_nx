/* wmw2_jni.c -- answers for the Java methods libwalaber.so calls back into
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * WHERE THIS LIST COMES FROM
 * --------------------------
 * Every name and signature below was recovered from libwalaber.so by
 * disassembling each GetMethodID call site and reading its two string-literal
 * arguments, then cross-checked against the six classes*.dex files. All sixty
 * matched, including the seven-argument analytics calls -- so this is the
 * engine's real upcall surface, not a guess.
 *
 * Grepping .rodata for adjacent name/signature strings does NOT work here, by
 * the way: the compiler hoists a repeated signature into a callee-saved
 * register and reuses it across a dozen GetMethodID calls, so string adjacency
 * pairs the wrong name with the wrong signature. That approach finds 13 of the
 * 61 and mislabels several of them.
 *
 * FOUR GROUPS
 * -----------
 *   1. Queries the engine BRANCHES on. These must answer, and answering "no"
 *      is what keeps the port offline and self-contained.
 *   2. Synchronous JSON getters (jniMigsImmediate*). These return inline.
 *   3. Request halves of async pairs. Swallowed here, answered next frame
 *      through wmw2_callbacks.c. Getting one of these wrong does not crash --
 *      it hangs a screen silently.
 *   4. Fire-and-forget notifications, correctly no-ops.
 *
 * Anything not listed falls through to jni_fake.c, which logs it as
 * "JNI: <kind> <name><sig> -> default". If the game ever stalls, that log line
 * names the method to add here.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "wmw2_jni.h"
#include "jni_fake.h"
#include "wmw2_callbacks.h"
#include "wmw2_store.h"
#include "wmw2_profile.h"
#include "wmw2_movie.h"
#include "config.h"
#include "util.h"

volatile int wmw2_quit_requested = 0;
volatile int wmw2_text_input_requested = 0;

/* jni_fake hands us the raw argument list. The first argument of an upcall is
 * the first *Java* parameter -- the receiver and the method id were already
 * consumed by the dispatcher -- so a jstring lands as a plain pointer, and
 * jni_peek_string() (jni_fake.h) reads it without taking a reference. */

/* ------------------------------------------------------------------------- */
/* 1. numeric answers  ()I / ()Z                                             */
/* ------------------------------------------------------------------------- */

int wmw2_jni_numeric(const char *name, const char *sig, long *out) {
  (void)sig;

  /* --- reachability -------------------------------------------------------
   * BridgeNetGeneral. Zero, and deliberately: answering "connected" invites
   * the engine to start requests that can never complete. Every network-gated
   * flow in the game -- consent, force-update, compatibility check, the whole
   * MIGS surface -- resolves immediately on a negative answer and hangs on a
   * positive one. Offline is not a limitation here, it is the mechanism. */
  if (!strcmp(name, "jniIsConnectedToNetwork"))               { *out = 0; return 1; }
  if (!strcmp(name, "jniIsConnectedToNetworkWithPermission")) { *out = 0; return 1; }

  /* --- storefront and ad availability ------------------------------------
   * The ad stack is Unity LevelPlay / ironSource, entirely Java-side. False
   * everywhere keeps the engine on the no-ad path instead of waiting for a
   * mediation callback that cannot arrive. */
  static const char *const say_false[] = {
    "jniMigsRequestIsIAPAvailable",      /* ()Z */
    "jniMigsQueryVideoAdAvailability",   /* (String)Z */
    "jniMigsRequestVideoAd",             /* (String,String)Z */
    "jniActiveBannerAd",                 /* ()Z */
    "jniIsIronSourceLoaded",             /* ()Z */
    "jniIsNexusSix",                     /* ()Z -- a device quirk check */
    NULL
  };
  for (int i = 0; say_false[i]; i++)
    if (!strcmp(name, say_false[i])) { *out = 0; return 1; }

  return 0;   /* let jni_fake log it so bring-up can identify it */
}

/* ------------------------------------------------------------------------- */
/* 2. float answers  ()F                                                     */
/* ------------------------------------------------------------------------- */

int wmw2_jni_float(const char *name, const char *sig, float *out) {
  if (strcmp(sig, "()F") != 0)
    return 0;
  (void)name;
  (void)out;
  /* WMW2 asks for its display geometry through jniRenderInit's arguments
   * rather than through float upcalls, so unlike WMW1 there is nothing that
   * must be answered here. Left in place because the dispatcher routes ()F
   * through s0 rather than x0 -- returning a float through the integer path
   * hands the engine garbage that only surfaces much later, during resize, as
   * a bad_alloc. That was a real WMW1 bug and the plumbing is worth keeping. */
  return 0;
}

/* ------------------------------------------------------------------------- */
/* 3. string answers                                                         */
/* ------------------------------------------------------------------------- */

const char *wmw2_jni_string(const char *name, const char *sig) {
  if (!strstr(sig, ")Ljava/lang/String;"))
    return NULL;

  /* --- the synchronous MIGS getters --------------------------------------
   * These are the exception to the request/response rule: BridgeDisMoMigs
   * calls MigsRelay and hands the JSONObject straight back, so the engine
   * consumes the answer inline and no notify follows.
   *
   * These must return a REAL document, not "{}". Parsing is not the problem --
   * jsoncpp accepts an empty object happily -- what follows is. The engine
   * looks up named keys and passes each result to std::stoull(), so an empty
   * document means stoull("") on the first lookup, an uncaught
   * std::invalid_argument, and abort() out of std::terminate. The shipped
   * factory_profile.json and store.json are exactly the documents these are
   * meant to return. */
  /* jniMigsImmediateProfileGetProfile is what actually gates the main menu.
   *
   * Screen_MainMenu::_reloadProfileToGetLatestFromServer runs once a frame and
   * calls MigsMessages::PerformMigsImmediate_Profile_GetProfile() -- the
   * SYNCHRONOUS getter, resolved from the PLT stub in that function. No
   * callback is involved, which is why jniMigsRequestProfileReload never
   * appeared in the upcall trace and why answering the async side changed
   * nothing. Returning "{}" here parses fine and yields no profile, so the menu
   * retried forever at a healthy 59 fps.
   *
   * The honest answer is the MIGS factory profile -- the document the engine
   * itself extracted to migs/factory_profile.json, with the UserIdentifier
   * substitution applied. It is the engine's own data in the engine's own
   * schema.
   *
   * Stated plainly: this is UNTESTED on this path. The same document aborts
   * when fed to jniUpdateGameWithMigsProfile, which wants the far richer server
   * document instead. The two are different parsers and the factory profile is
   * by definition what a first-run MIGS profile looks like, so it should be
   * right here -- but if the next log aborts in stoull immediately after a
   * "jni-> obj jniMigsImmediateProfileGetProfile" line, this is the reason. */
  if (!strcmp(name, "jniMigsImmediateProfileGetProfile") ||
      !strcmp(name, "jniMigsImmediateProfileResetAndRetrieveProfile")) {
    /* The factory profile with every modification the engine has made since
     * launch folded in.
     *
     * NOTE: the per-frame polling this was written to stop turned out to have a
     * different cause entirely -- the engine was sitting in
     * Screen_GraphicsContextRestore because main.c wrongly called
     * jniRenderAreaReload at startup. The merge is kept because it is what a
     * MIGS server actually does, not because it fixed the hang.
     *
     * MIGS semantics are read-your-writes:
     * the engine calls jniMigsRequestProfileModify with a delta and then
     * re-reads, expecting to see its own change. Handing back an unchanged
     * factory profile forever is why
     * Screen_MainMenu::_reloadProfileToGetLatestFromServer polled once a frame
     * and never stopped -- the document parsed cleanly every time and simply
     * never said what the engine was waiting to hear.
     *
     * The merge is cached and only recomputed when a new delta arrives, so the
     * per-frame poll stays cheap. */
      /* The player's profile -- and this is the load half of saving.
     *
     * Returning "{}" here was correct as a description of Android's behaviour
     * when MIGS has nothing (BridgeDisMoMigs.jniMigsImmediateProfileGetProfile
     * returns exactly that on a null profile), and wrong as a thing for this
     * port to do. There is no MIGS to have anything, so "nothing" is permanent:
     * the engine asks for the profile at startup, is told it is empty, and
     * every level completed and every setting changed in the previous session
     * is gone. Progress lives ONLY in this document -- neither database has a
     * column for it.
     *
     * wmw2_profile_current() answers with the saved profile if there is one,
     * the shipped factory profile if this is a first run, plus every delta the
     * engine has sent since. */
    return wmw2_profile_current();
  }

  /* The store catalogue equivalent. store.json is what the engine extracts for
   * its own store, so it is the matching document. */
  if (!strcmp(name, "jniMigsImmediateStoreGetStoreItemsInfo"))
    return wmw2_store_items_json();

  /* --- currency formatting ------------------------------------------------
   * jniFormatCurrencyForLocale(float, String)String. The engine has its own
   * fallback -- it logs "Currency format failed (using fallback)" and formats
   * with "%.2f" -- so returning NULL is survivable. There is no storefront
   * here, so let the fallback run rather than inventing a currency symbol. */

  return NULL;   /* jni_fake substitutes "" and logs it */
}

/* ------------------------------------------------------------------------- */
/* 4. void notifications                                                     */
/* ------------------------------------------------------------------------- */

int wmw2_jni_void(const char *name, const char *sig, va_list va) {

  /* ======================================================================= */
  /* request halves of async pairs -- swallow, then answer on a later frame  */
  /* ======================================================================= */

  /* --- profile -----------------------------------------------------------
   *
   * SWALLOWED, with no reply. This is the opposite of what section 5 of the
   * porting notes originally advised, and the change is the whole reason boot
   * five failed.
   *
   * The reasoning that led there was: these are async request/response pairs,
   * so a request without a response hangs the engine. True in general. What is
   * NOT true is that any document will do for the response. The engine
   * requested a MIGS profile -- a *cloud* profile, whose schema the port does
   * not know -- and the nearest thing on disk, factory_profile.json, is the
   * local database seed. It is a different shape:
   *
   *     35 records of EventName / EventValue / EventStringValue
   *     10 of InternalID / BuyCount
   *     21 of Name / Value
   *      4 of Title / Unlocked / Count / EnableTutorial
   *
   * with EventName, InternalID, Name and Title all genuinely textual and 33
   * EventStringValue fields holding the literal "null". Whichever of those the
   * MIGS-side parser reads as a number, std::stoull throws, nothing catches it,
   * and libc++ aborts. Fixing UserIdentifier removed one landmine out of many.
   *
   * Offline, a cloud profile simply never arrives. That is a state the game
   * ships knowing how to be in -- it already built its local profile from
   * factory_profile.json through its own code path, which knows the schema.
   * Silence is the honest answer, and the log bears it out: the engine made
   * this request during startup and went on to render the loading screen and
   * reach the main loop. It only died once the port answered.
   *
   * If a screen ever does hang waiting on one of these, the fix is to learn the
   * real MIGS schema -- not to feed it another document that happens to be
   * nearby. */
  if (!strcmp(name, "jniMigsRequestProfileModify") ||
      !strcmp(name, "jniMigsRequestProfileSynthID")) {
    /* NO RESPONSE. Checked on the Java side rather than guessed this time:
     * MigsTransProfileModify has no RenderExec_Execute that delivers a profile
     * -- it logs, marks the response received, and finishes. Only
     * MigsTransProfileReload and MigsRelay$SendToWalaber_Profile ever call
     * UpdateGameDatabaseWith_MigsUserProfile.
     *
     * Echoing the modify payload back as a profile was wrong twice over: wrong
     * direction, and wrong schema -- the modify carries a ~155 byte delta while
     * the profile callback wants the full server document. It aborted in
     * stoull, as everything of the wrong schema does.
     *
     * The document is still worth keeping for its own sake. */
    /* These carry a document: the engine's own profile, in the engine's own
     * schema. Keep it and hand it straight back.
     *
     * This is what unblocks the main menu. The engine's MIGS layer serialises
     * its transactions -- it will not begin the profile reload the menu is
     * waiting on until this modify is acknowledged. Swallowing it left
     * Screen_MainMenu::_reloadProfileToGetLatestFromServer retrying once a
     * frame forever, with jniMigsRequestProfileReload never even reaching the
     * port. */
    void *jdoc = va_arg(va, void *);
    const char *doc = jni_peek_string(jdoc);
    wmw2_cb_set_profile_doc(doc);      /* logs it */
    wmw2_profile_note_modify(doc);     /* and folds it into what we hand back */
    return 1;
  }
  if (!strcmp(name, "jniMigsRequestProfileReload") ||
      !strcmp(name, "jniMigsRequestProfileReset")) {
    /* These DO expect a profile, but the full server document -- StoreInfo,
     * ABTestCase, MysteryDuckSettings, powerup tuning -- which the port cannot
     * assemble honestly. Neither has been observed being called, because the
     * main menu uses the synchronous getter below instead. Left silent. */
    return 1;
  }
  if (!strcmp(name, "jniMigsRequestProfileFlush")) {
    /* A write with no reply expected: a flush to a service that does not exist
     * is complete the moment it is asked for. */
    return 1;
  }

  /* --- store -------------------------------------------------------------
   * Same reasoning. store.json is the local catalogue the engine already reads
   * itself; the MIGS store payload is a service response with its own schema.
   * Answering with the local file risks the identical abort one subsystem
   * over. */
  if (!strcmp(name, "jniMigsRequestStoreReloadStoreItems") ||
      !strcmp(name, "jniMigsRequestStoreGetSingleStoreItem")) {
    return 1;
  }

  if (!strcmp(name, "jniMigsRequestPurchaseProduct")) {
    /* This one is safe to answer: the payload is (bool, bool, String sku) --
     * typed scalars plus the sku the engine itself just supplied, no schema to
     * get wrong. Reporting failure closes the purchase dialog and leaves the
     * entitlement alone, which is the truth here. */
    void *jsku = va_arg(va, void *);
    wmw2_cb_post(W2CB_STORE_PURCHASED, 0, 0, jni_peek_string(jsku));
    return 1;
  }
  if (!strcmp(name, "jniMigsRequestRestorePurchases")) {
    /* Also safe: a String[] of product ids the port owns end to end. */
    wmw2_store_restore();
    return 1;
  }

  /* --- rewards / gifts / leaderboard / support --------------------------- */
  if (!strcmp(name, "jniMigsRequestRewardsGetBalance")) {
    wmw2_cb_post(W2CB_REWARDS_BALANCE, 0, 0, NULL);
    return 1;
  }
  if (!strcmp(name, "jniMigsRequestRewardsConsume")) {
    const int n = va_arg(va, int);
    wmw2_cb_post(W2CB_REWARDS_CONSUMED, n, 0, NULL);
    return 1;
  }
  if (!strcmp(name, "jniMigsRequestGiftsConsume")) {
    const int n = va_arg(va, int);
    wmw2_cb_post(W2CB_GIFT_CONSUMED, n, 0, NULL);
    return 1;
  }
  if (!strcmp(name, "jniMigsRequestLeaderboardGetInfo")) {
    /* Another unknown-schema document. Swallowed for the same reason as the
     * profile: there is no leaderboard offline, and inventing one risks the
     * same abort. */
    return 1;
  }
  if (!strcmp(name, "jniMigsRequestCustCareSendEmail")) {
    wmw2_cb_post(W2CB_CUSTCARE_EMAIL, 0, 0, NULL);   /* result = false */
    return 1;
  }

  /* --- reward video ------------------------------------------------------ */
  if (!strcmp(name, "jniMigsRequestInitRewardVideo")) {
    wmw2_cb_post(W2CB_REWARD_VIDEO_INIT, 0, 0, NULL);  /* available = false */
    return 1;
  }

  /* --- movies ------------------------------------------------------------
   * jniPlayMovie(String path, String, boolean). assets/Water/Movies/ holds
   * eight .mp4 location intros and outros, so this path is reachable in normal
   * play. There is no decoder here, so the honest answer is "it finished".
   *
   * Deferred by design, and not just for tidiness: answering from inside
   * jniPlayMovie would call jniNotifyMovieFinished while the engine is still
   * inside its own movie-start call, re-entering it on its own stack. One
   * frame's delay costs nothing and matches what the Android VideoView did. */
  if (!strcmp(name, "jniPlayMovie")) {
    /* Blocking, on purpose. This is an upcall from inside the engine's own
     * frame and it expects the clip to happen before being told it is over, so
     * the player drives its own present loop for the duration.
     *
     * The finished notification is still POSTED rather than called here: firing
     * it inline would re-enter the engine on the stack of the call that started
     * the movie. */
    void *jpath = va_arg(va, void *);
    const char *path = jni_peek_string(jpath);
    if (!wmw2_movie_play(path))
      debugPrintf("movie: %s not played -- reporting finished immediately\n", path);
    wmw2_cb_post(W2CB_MOVIE_FINISHED, 0, 0, NULL);
    return 1;
  }

  /* --- ads ---------------------------------------------------------------
   * All swallowed. jniNotifyAllAdsCreated is posted once from main.c during
   * bring-up rather than in response to any single one of these; the engine's
   * ad manager waits for that acknowledgement before it considers the
   * subsystem ready. */
  static const char *const ad_calls[] = {
    "jniShowAd", "jniHideAd",
    "jniShowAdSpecific", "jniHideAdSpecific", "jniUpdateAdSpecific",
    "jniHasAdSpecific", "jniNativeClick",
    "jniShowBannerAd", "jniHideBannerAd",
    "jniMigsBannerBackgroundPosition", "jniMigsShowPostGameInterstitial",
    "jniMigsDidExitMainMenu",
    NULL
  };
  for (int i = 0; ad_calls[i]; i++)
    if (!strcmp(name, ad_calls[i])) return 1;

  if (!strcmp(name, "jniRequestBalanceUpdate")) {
    wmw2_cb_post(W2CB_REWARDS_BALANCE, 0, 0, NULL);
    return 1;
  }
  if (!strcmp(name, "jniSubtractCurrency")) {
    /* (String, I)V -- an ad-currency spend. Nothing to spend from. */
    return 1;
  }

  /* ======================================================================= */
  /* keyboard                                                                */
  /* ======================================================================= */

  if (!strcmp(name, "startTextInput")) {
    wmw2_text_input_requested = 1;
    debugPrintf("keyboard: startTextInput\n");
    return 1;
  }
  if (!strcmp(name, "stopTextInput")) {
    wmw2_text_input_requested = 0;
    debugPrintf("keyboard: stopTextInput\n");
    return 1;
  }

  /* ======================================================================= */
  /* display                                                                 */
  /* ======================================================================= */

  if (!strcmp(name, "jniSetDisplayPercent") && !strcmp(sig, "(F)V")) {
    /* BridgeRendering asked the Java view to shrink the game viewport to a
     * percentage of the surface (WalaberCustomLayout.SetGameViewSizePercentage).
     * The port owns its framebuffer geometry and honouring this would fight
     * wmw_tate.c's rotation, so acknowledge and ignore. */
    return 1;
  }

  /* ======================================================================= */
  /* fire-and-forget                                                         */
  /* ======================================================================= */

  static const char *const noops[] = {
    /* BridgeDisMoAnalyticals -- six event loggers, all genuinely one-way.
     * Note jniLogPlayEndsEvent takes SEVEN arguments, two of them on the
     * stack; nothing here reads them, but do not "simplify" the dispatcher on
     * the assumption that they fit in registers. */
    "jniLogPlayStartsEvent", "jniLogPlayEndsEvent", "jniLogLoadingEndsEvent",
    "jniLogNavActionsEvent", "jniLogEconomyTransactionsEvent",
    "jniLogIAPTransactionsEvent",
    /* external links */
    "jniOpenURL", "jniOpenURLWithoutDialog",
    /* referral store */
    "jniShowReferralStore", "jniHideReferralStore",
    /* consent */
    "jniRefreshCMP",
    /* game flow -- the engine narrating its own state transitions */
    "jniGameFlowEvent",
    /* A/B testing; BridgeDisMoAbtest has no Java class in this build at all,
     * but the engine still resolves the name if it constructs that bridge. */
    "jniABTInformerPayload",
    /* JunctionTester self-test harness, never called during play */
    "jniConfirmationReplyNonStatic", "jniConfirmationReplyStatic",
    NULL
  };
  for (int i = 0; noops[i]; i++)
    if (!strcmp(name, noops[i])) return 1;

  return 0;
}
