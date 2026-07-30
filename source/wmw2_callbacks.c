/* wmw2_callbacks.c -- answers for the engine's asynchronous requests
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * See wmw2_callbacks.h for why these are posted rather than called inline.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <switch.h>

#include "wmw2_callbacks.h"
#include "wmw2_entrypoints.h"
#include "wmw2_bridges.h"
#include "jni_fake.h"
#include "wmw_paths.h"
#include "config.h"
#include "wmw2_profile.h"
#include "util.h"

#define MAX_PENDING  48
#define MAX_RESTORE  24
#define SVAL_MAX     96

typedef struct {
  wmw2_cb_kind kind;
  int a, b;
  char sval[SVAL_MAX];
  uint64_t due_ns;     /* 0 = deliver at the next drain */
} Pending;

static Pending s_queue[MAX_PENDING];
static int     s_count;
static Mutex   s_lock;
static int     s_lock_ready;

/* The last profile document the ENGINE handed us, via
 * jniMigsRequestProfileModify / SynthID.
 *
 * This is the answer to a question that cost several boots: what schema does
 * jniUpdateGameWithMigsProfile expect? Not factory_profile.json -- that is the
 * local database seed and it aborts in std::stoull. The right document is the
 * one the engine already wrote itself when it asked for the modification. Echo
 * it back and the schema is correct by construction, because the engine is both
 * author and reader. */
static char  *s_profile_doc;
static Mutex  s_profile_lock;
static int    s_profile_lock_ready;

void wmw2_cb_set_profile_doc(const char *json) {
  if (!json || !*json) return;
  debugPrintf("cb: engine modification delta = %s\n", json);
  if (!s_profile_lock_ready) { mutexInit(&s_profile_lock); s_profile_lock_ready = 1; }
  char *copy = strdup(json);
  if (!copy) return;
  mutexLock(&s_profile_lock);
  free(s_profile_doc);
  s_profile_doc = copy;
  mutexUnlock(&s_profile_lock);
  debugPrintf("cb: cached engine profile document (%zu bytes)\n", strlen(copy));
}

int wmw2_cb_have_profile_doc(void) { return s_profile_doc != NULL; }

/* The restore list is held separately: it is a String[] rather than a scalar,
 * and only one can be in flight at a time (Android's MigsRelay behaved the
 * same way -- a second restore request supersedes the first). */
static char s_restore[MAX_RESTORE][64];
static int  s_restore_count;

static void post_common(wmw2_cb_kind kind, int a, int b, const char *sval,
                        uint64_t due_ns, int replace) {
  if (!s_lock_ready) { mutexInit(&s_lock); s_lock_ready = 1; }
  mutexLock(&s_lock);

  if (replace) {
    for (int i = 0; i < s_count; ) {
      if (s_queue[i].kind == kind) s_queue[i] = s_queue[--s_count];
      else i++;
    }
  }

  if (s_count < MAX_PENDING) {
    Pending *p = &s_queue[s_count++];
    p->kind   = kind;
    p->a      = a;
    p->b      = b;
    p->due_ns = due_ns;
    if (sval) snprintf(p->sval, sizeof(p->sval), "%s", sval);
    else      p->sval[0] = '\0';
  } else {
    debugPrintf("cb: queue full, dropping kind %d\n", (int)kind);
  }
  mutexUnlock(&s_lock);
}

void wmw2_cb_post(wmw2_cb_kind kind, int a, int b, const char *sval) {
  post_common(kind, a, b, sval, 0, 0);
}

void wmw2_cb_post_delayed(wmw2_cb_kind kind, int a, int b, const char *sval, int delay_ms) {
  const uint64_t now = armTicksToNs(armGetSystemTick());
  post_common(kind, a, b, sval, now + (uint64_t)delay_ms * 1000000ULL, 1);
}

void wmw2_cb_post_restore(const char *const *skus, int count) {
  if (count > MAX_RESTORE) count = MAX_RESTORE;

  if (!s_lock_ready) { mutexInit(&s_lock); s_lock_ready = 1; }
  mutexLock(&s_lock);
  s_restore_count = 0;
  for (int i = 0; i < count; i++)
    snprintf(s_restore[s_restore_count++], sizeof(s_restore[0]), "%s", skus[i]);
  mutexUnlock(&s_lock);

  if (count > 0) post_common(W2CB_STORE_TO_RESTORE, count, 0, NULL, 0, 1);
  else           post_common(W2CB_STORE_NOTHING_TO_RESTORE, 0, 0, NULL, 0, 1);
}

/* ------------------------------------------------------------------------- */


/* Payloads for the MIGS callbacks that carry a JSON document.
 *
 * "{}" is NOT a safe default here, and this cost a boot to learn. The engine
 * does not iterate whatever it is handed -- it looks up specific keys and feeds
 * the result straight to std::stoull():
 *
 *     "PlayerData__HeartCount": { "EventName": "HeartCount",
 *                                 "EventValue": "7", ... }
 *
 * Against an empty document every lookup yields "", stoull throws
 * std::invalid_argument, and since nothing catches it libc++ calls abort() out
 * of std::terminate:
 *
 *     terminating with uncaught exception of type std::invalid_argument:
 *     stoull: no conversion
 *
 * The game ships the correct answer: assets/Water/Data/factory_profile.json is
 * literally the new-player MIGS profile, in exactly the shape this callback is
 * expected to deliver. store.json is the matching store catalogue. Read the
 * asset copies rather than the ones under migs/ -- the engine owns those, and
 * 4.1 (Horizon will not stat a file it has open) applies to anything it is
 * holding. */
static char *load_asset_json(const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/assets/%s/Data/%s",
           wmw_game_dir(), WMW2_ASSET_SUBDIR, name);

  FILE *f = fopen(path, "rb");
  if (!f) { debugPrintf("cb: could not read %s\n", path); return NULL; }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); return NULL; }
  char *buf = malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';

  /* Anything handed back as a MIGS profile needs the same UserIdentifier
   * substitution as the archived copy -- an unpatched one aborts in stoull
   * exactly the same way. */
  if (wmw2_profile_needs_fixup(name)) {
    size_t plen = 0;
    char *patched = wmw2_profile_fixup(buf, got, &plen);
    if (patched) { free(buf); buf = patched; }
  }
  return buf;
}

/* Loaded once and kept: the engine may ask repeatedly, and these are a few KB. */
const char *wmw2_factory_profile_json(void) {
  static char *cached;
  static int tried;
  if (!tried) { tried = 1; cached = load_asset_json("factory_profile.json"); }
  return cached ? cached : "{}";
}

const char *wmw2_store_items_json(void) {
  static char *cached;
  static int tried;
  if (!tried) { tried = 1; cached = load_asset_json("store.json"); }
  return cached ? cached : "{}";
}

static const char *cb_name(wmw2_cb_kind k) {
  switch (k) {
    case W2CB_MIGS_PROFILE:             return "UpdateGameWithMigsProfile";
    case W2CB_MIGS_STORE_ITEMS:         return "UpdateGameWithMigsStoreItems";
    case W2CB_MIGS_NEWS_ITEMS:          return "UpdateGameWithMigsNewsItems";
    case W2CB_MIGS_GIFT_ITEMS:          return "UpdateGameWithMigsGiftItems";
    case W2CB_STORE_SINGLE_ITEM:        return "NotifyStoreSingleItemInfoResult";
    case W2CB_STORE_PURCHASED:          return "NotifyStoreProductPurchased";
    case W2CB_STORE_TO_RESTORE:         return "NotifyStoreProductToRestore";
    case W2CB_STORE_NOTHING_TO_RESTORE: return "NotifyStoreNothingToRestore";
    case W2CB_REWARDS_BALANCE:          return "NotifyRewardsBalance";
    case W2CB_REWARDS_CONSUMED:         return "NotifyRewardsConsumed";
    case W2CB_GIFT_CONSUMED:            return "NotifyGiftConsumed";
    case W2CB_LEADERBOARD:              return "NotifyLeaderboard";
    case W2CB_CUSTCARE_EMAIL:           return "NotifyCustCareEmailResult";
    case W2CB_REWARD_VIDEO_INIT:        return "RewardVideoInit";
    case W2CB_REWARD_VIDEO_FAILED:      return "RewardVideoFailed";
    case W2CB_REWARD_VIDEO_CANCELED:    return "RewardVideoCanceled";
    case W2CB_ADS_ALL_CREATED:          return "NotifyAllAdsCreated";
    case W2CB_AD_EVENT:                 return "NotifyAdEvent";
    case W2CB_MOVIE_FINISHED:           return "NotifyMovieFinished";
  }
  return "?";
}

static void deliver(const Pending *p) {
  /* Logged BEFORE the call, and flushed, so a callback that does not return
   * still names itself in the log. */
  debugPrintf("  cb-> %s\n", cb_name(p->kind));
  debugLogFlush();
  void *env  = fake_env;
  void *migs = wmw2_bridge(BR_MIGS);
  void *ads  = wmw2_bridge(BR_ADVERTS);
  void *lay  = wmw2_bridge(BR_LAYOUT);

  /* Empty JSON rather than NULL: the engine parses these with a statically
   * linked jsoncpp whose reader does not tolerate a null root, and its error
   * path ("A valid JSON document must be either an array or an object value.")
   * is a hard failure rather than a fallback. */
  const char *json = (p->sval[0] ? p->sval : "{}");

  switch (p->kind) {
    case W2CB_MIGS_PROFILE: {
      /* Only ever the engine's own document, never one we invented. If we have
       * not been given one yet there is nothing honest to send, and silence is
       * better than a guess -- see the schema note above. */
      if (!wmw2.migs_UpdateGameWithMigsProfile) break;
      char *doc = NULL;
      if (s_profile_lock_ready) {
        mutexLock(&s_profile_lock);
        if (s_profile_doc) doc = strdup(s_profile_doc);
        mutexUnlock(&s_profile_lock);
      }
      if (!doc) { debugPrintf("cb: no profile document to echo -- skipped\n"); break; }
      debugPrintf("cb: echoing engine profile (%zu bytes)\n", strlen(doc));
      wmw2.migs_UpdateGameWithMigsProfile(env, migs, jni_make_string(doc));
      free(doc);
      break;
    }
    case W2CB_MIGS_STORE_ITEMS:
      if (wmw2.migs_UpdateGameWithMigsStoreItems)
        wmw2.migs_UpdateGameWithMigsStoreItems(env, migs,
            jni_make_string(wmw2_store_items_json()));
      break;
    case W2CB_MIGS_NEWS_ITEMS:
      if (wmw2.migs_UpdateGameWithMigsNewsItems)
        wmw2.migs_UpdateGameWithMigsNewsItems(env, migs, jni_make_string(json));
      break;
    case W2CB_MIGS_GIFT_ITEMS:
      if (wmw2.migs_UpdateGameWithMigsGiftItems)
        wmw2.migs_UpdateGameWithMigsGiftItems(env, migs, jni_make_string(json));
      break;

    case W2CB_STORE_SINGLE_ITEM:
      if (wmw2.migs_NotifyStoreSingleItemInfoResult)
        wmw2.migs_NotifyStoreSingleItemInfoResult(env, migs,
            jni_make_string(p->sval), jni_make_string("{}"));
      break;
    case W2CB_STORE_PURCHASED:
      /* (Z success, Z isRestore, String sku) */
      if (wmw2.migs_NotifyStoreProductPurchased)
        wmw2.migs_NotifyStoreProductPurchased(env, migs, p->a, p->b,
                                              jni_make_string(p->sval));
      break;
    case W2CB_STORE_TO_RESTORE: {
      if (!wmw2.migs_NotifyStoreProductToRestore) break;
      /* Snapshot under the lock. deliver() deliberately runs with the lock
       * released -- the engine may post another callback from inside this very
       * call -- so the restore list has to be copied out rather than read in
       * place. */
      char snap[MAX_RESTORE][64];
      const char *items[MAX_RESTORE];
      int n;
      mutexLock(&s_lock);
      n = s_restore_count;
      for (int i = 0; i < n; i++) memcpy(snap[i], s_restore[i], sizeof(snap[0]));
      mutexUnlock(&s_lock);
      for (int i = 0; i < n; i++) items[i] = snap[i];
      void *arr = jni_make_string_array(items, n);
      debugPrintf("cb: restoring %d entitlement(s)\n", n);
      wmw2.migs_NotifyStoreProductToRestore(env, migs, arr);
      break;
    }
    case W2CB_STORE_NOTHING_TO_RESTORE:
      if (wmw2.migs_NotifyStoreNothingToRestore)
        wmw2.migs_NotifyStoreNothingToRestore(env, migs);
      break;

    case W2CB_REWARDS_BALANCE:
      if (wmw2.migs_NotifyRewardsBalance)
        wmw2.migs_NotifyRewardsBalance(env, migs, p->a);
      break;
    case W2CB_REWARDS_CONSUMED:
      if (wmw2.migs_NotifyRewardsConsumed)
        wmw2.migs_NotifyRewardsConsumed(env, migs, p->a);
      break;
    case W2CB_GIFT_CONSUMED:
      if (wmw2.migs_NotifyGiftConsumed)
        wmw2.migs_NotifyGiftConsumed(env, migs, p->a);
      break;
    case W2CB_LEADERBOARD:
      if (wmw2.migs_NotifyLeaderboard)
        wmw2.migs_NotifyLeaderboard(env, migs, jni_make_string(json));
      break;
    case W2CB_CUSTCARE_EMAIL:
      if (wmw2.migs_NotifyCustCareEmailResult)
        wmw2.migs_NotifyCustCareEmailResult(env, migs, p->a);
      break;

    case W2CB_REWARD_VIDEO_INIT:
      if (wmw2.migs_RewardVideoInit)
        wmw2.migs_RewardVideoInit(env, migs, p->a);
      break;
    case W2CB_REWARD_VIDEO_FAILED:
      if (wmw2.migs_RewardVideoFailed)
        wmw2.migs_RewardVideoFailed(env, migs, jni_make_string(p->sval));
      break;
    case W2CB_REWARD_VIDEO_CANCELED:
      if (wmw2.migs_RewardVideoCanceled)
        wmw2.migs_RewardVideoCanceled(env, migs, jni_make_string(p->sval));
      break;

    case W2CB_ADS_ALL_CREATED:
      if (wmw2.adverts_NotifyAllAdsCreated)
        wmw2.adverts_NotifyAllAdsCreated(env, ads);
      break;
    case W2CB_AD_EVENT:
      if (wmw2.adverts_NotifyAdEvent)
        wmw2.adverts_NotifyAdEvent(env, ads, p->a, p->b, 0);
      break;

    case W2CB_MOVIE_FINISHED:
      /* The engine asked us to play a movie and is waiting to be told it
       * finished. There is no decoder here, so the answer is "finished
       * already" -- but on a LATER frame, never from inside jniPlayMovie, which
       * would re-enter the engine on its own stack. */
      if (wmw2.layout_NotifyMovieFinished)
        wmw2.layout_NotifyMovieFinished(env, lay);
      break;
  }
}

void wmw2_cb_drain(void) {
  if (!s_lock_ready)
    return;

  const uint64_t now = armTicksToNs(armGetSystemTick());

  for (;;) {
    Pending item;
    int found = 0;

    mutexLock(&s_lock);
    for (int i = 0; i < s_count; i++) {
      if (s_queue[i].due_ns == 0 || s_queue[i].due_ns <= now) {
        item = s_queue[i];
        s_queue[i] = s_queue[--s_count];
        found = 1;
        break;
      }
    }
    mutexUnlock(&s_lock);

    if (!found)
      break;

    /* Delivered with the lock released: these calls re-enter the engine, and
     * the engine is entitled to post another callback from inside one. */
    deliver(&item);
  }
}
