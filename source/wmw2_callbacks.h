/* wmw2_callbacks.h -- answers for the engine's asynchronous requests
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Most of WMW2's platform calls are request/response pairs, not
 * fire-and-forget. The engine calls out through BridgeDisMoMigs, Java forwards
 * to MigsRelay and returns immediately, and the ANSWER arrives later through a
 * completely different native entry point. Both halves were read out of
 * classes*.dex:
 *
 *     BridgeDisMoMigs.jniMigsRequestRewardsGetBalance()
 *         -> MigsRelay.MigsRequest_Rewards_GetBalance()      [async]
 *         -> ... later ...
 *         -> jniNotifyNewMigsInfoRewardsBalance(balance)
 *
 * Treating the outbound half as a no-op -- the obvious thing to do for a
 * storefront and an analytics service that cannot exist on Switch -- leaves the
 * engine waiting for a reply that never comes. It does not crash and it does
 * not log: the screen simply never advances, while the button that opened it
 * animates perfectly because the tap was received. That was the single
 * hardest-to-diagnose bug in the WMW1 port, and WMW2 has twelve of these pairs
 * instead of five.
 *
 * The answers are POSTED rather than called inline. The request arrives on the
 * render thread from inside engine code, and calling straight back into the
 * engine from there re-enters it at an arbitrary point in its own call stack.
 * On Android these were genuinely asynchronous -- MigsRelay answered on a later
 * turn of the main looper -- so draining the queue at the top of the next frame
 * is both safer and more faithful.
 */

#ifndef __WMW2_CALLBACKS_H__
#define __WMW2_CALLBACKS_H__

typedef enum {
  /* --- BridgeDisMoMigs: profile ----------------------------------------- */
  W2CB_MIGS_PROFILE,             /* jniUpdateGameWithMigsProfile(String json)  */
  W2CB_MIGS_STORE_ITEMS,         /* jniUpdateGameWithMigsStoreItems(String)    */
  W2CB_MIGS_NEWS_ITEMS,          /* jniUpdateGameWithMigsNewsItems(String)     */
  W2CB_MIGS_GIFT_ITEMS,          /* jniUpdateGameWithMigsGiftItems(String)     */

  /* --- BridgeDisMoMigs: store ------------------------------------------- */
  W2CB_STORE_SINGLE_ITEM,        /* jniNotifyNewMigsInfoStoreSingleItemInfoResult(sku, json) */
  W2CB_STORE_PURCHASED,          /* jniNotifyNewMigsInfoStoreProductPurchased(ok, isRestore, sku) */
  W2CB_STORE_TO_RESTORE,         /* jniNotifyNewMigsInfoStoreProductToRestore(String[]) */
  W2CB_STORE_NOTHING_TO_RESTORE, /* jniNotifyNewMigsInfoStoreNothingToRestore() */

  /* --- BridgeDisMoMigs: rewards / gifts / leaderboard / support ---------- */
  W2CB_REWARDS_BALANCE,          /* jniNotifyNewMigsInfoRewardsBalance(I)      */
  W2CB_REWARDS_CONSUMED,         /* jniNotifyNewMigsInfoRewardsConsumed(I)     */
  W2CB_GIFT_CONSUMED,            /* jniNotifyNewMigsInfoGiftConsumed(I)        */
  W2CB_LEADERBOARD,              /* jniNotifyNewMigsInfoLeaderboard(String)    */
  W2CB_CUSTCARE_EMAIL,           /* jniNotifyNewMigsInfoCustCareEmailResult(Z) */

  /* --- BridgeDisMoMigs: reward video ------------------------------------ */
  W2CB_REWARD_VIDEO_INIT,        /* jniRewardVideoInit(Z)                      */
  W2CB_REWARD_VIDEO_FAILED,      /* jniRewardVideoFailed(String id)            */
  W2CB_REWARD_VIDEO_CANCELED,    /* jniRewardVideoCanceled(String id)          */

  /* --- BridgeAdverts ----------------------------------------------------- */
  W2CB_ADS_ALL_CREATED,          /* jniNotifyAllAdsCreated()                   */
  W2CB_AD_EVENT,                 /* jniNotifyAdEvent(I,I,I)                    */

  /* --- Display ----------------------------------------------------------- */
  W2CB_MOVIE_FINISHED            /* jniNotifyMovieFinished()                   */
} wmw2_cb_kind;

/* Queue an answer. Safe to call from inside the JNI dispatch.
 * `sval` may be NULL; `a`/`b` carry the integer/boolean arguments. */
void wmw2_cb_post(wmw2_cb_kind kind, int a, int b, const char *sval);

/* As above, but delivered no sooner than `delay_ms` from now, REPLACING any
 * pending callback of the same kind. Mirrors Android's
 * Handler.removeCallbacks() + postDelayed(). */
void wmw2_cb_post_delayed(wmw2_cb_kind kind, int a, int b, const char *sval, int delay_ms);

/* Queue the restore-purchases reply: one W2CB_STORE_TO_RESTORE carrying the
 * whole owned list, or W2CB_STORE_NOTHING_TO_RESTORE if the list is empty.
 * Separated out because it is the only answer that needs a String[]. */
void wmw2_cb_post_restore(const char *const *skus, int count);

/* Remember the profile document the engine handed us with a modify/synth-ID
 * request, so it can be echoed back as the reload response. The engine is both
 * author and reader, so the schema is right by construction -- which is more
 * than can be said for any document the port could assemble itself. */
void wmw2_cb_set_profile_doc(const char *json);
int  wmw2_cb_have_profile_doc(void);

/* The shipped JSON documents, loaded once from assets/Water/Data/.
 *
 * These are the correct payloads for anything that hands the engine a MIGS
 * profile or store catalogue -- deferred or synchronous. Never substitute "{}":
 * the engine looks up specific keys and passes the result to std::stoull(),
 * which throws on an empty document and takes the process down through
 * std::terminate. Both fall back to "{}" only if the asset is unreadable, which
 * check_data() in main.c has already ruled out by this point. */
const char *wmw2_factory_profile_json(void);
const char *wmw2_store_items_json(void);

/* Deliver everything due. Call once per frame, before jniRenderDrawPreDraw. */
void wmw2_cb_drain(void);

#endif
