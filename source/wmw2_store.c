/* wmw2_store.c -- answer the engine's in-app-purchase requests
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * There is no store on this platform, so every purchase request is answered
 * "no" and every restore is answered "nothing".
 *
 * Add-on content is granted by editing migs_profile.json instead. Each item
 * already has a record there:
 *
 *     "IAPInfo__StarterBundle01": { "InternalID": "StarterBundle01",
 *                                   "BuyCount": "0" }
 *
 * Setting BuyCount grants it. That is the same file the game keeps its level
 * progress and settings in, read through the same path, so there is one source
 * of truth rather than two that can disagree.
 *
 * An earlier version of this file read a purchases.txt next to the .nro and
 * pushed entitlements in through the restore callback. It worked, but it was a
 * second route into the engine for a fact the profile already recorded, and the
 * two could end up saying different things.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "wmw2_store.h"
#include "wmw2_callbacks.h"
#include "wmw_paths.h"
#include "config.h"
#include "util.h"

/* store.json's internalId -> googleStoreId mapping, transcribed at build time.
 *
 * Read from a table rather than parsed out of the asset at runtime, for the
 * same reason WMW1's port did not read water.db: the engine owns that file, and
 * a second reader of something Horizon will not let us stat while it is held
 * open is exactly the trouble documented in 4.1. The engine is told the GOOGLE
 * product id, because that is what Play would have handed it. */


#define NUM_SKUS ((int)(sizeof(g_skus) / sizeof(g_skus[0])))
#define MAX_OWNED 24




/* Create purchases.txt if it is not there. Never overwrites: the file is the
 * player's, and silently replacing an edited one would revoke content they
 * legitimately own. */


void wmw2_store_restore(void) {
  /* Answer "nothing to restore", and nothing else.
   *
   * purchases.txt is gone: the port no longer writes one and no longer reads
   * one. It was a second, parallel way of granting entitlements, and it
   * competed with the profile rather than complementing it.
   *
   * The profile is the better mechanism and it is the one the game actually
   * uses. Every add-on already has a record in migs_profile.json --
   *
   *     "IAPInfo__StarterBundle01": { "InternalID": "StarterBundle01",
   *                                   "BuyCount": "0" }
   *
   * -- so granting something is a matter of setting BuyCount, in the same file
   * that holds level progress and settings, read through the same code path,
   * with the same merge and normalisation applied. A restore posted through
   * this callback went round a completely different route into the engine and
   * could disagree with what the profile said; two sources of truth for the
   * same fact is how state ends up inconsistent.
   *
   * The callback is still answered, because it is one of the request/response
   * pairs in section 5 -- the in-game Restore Purchases button calls
   * jniMigsRequestRestorePurchases and waits for a reply. Reporting nothing is
   * the truthful answer when there is no store to have bought anything from. */
  debugPrintf("store: restore requested -- no store on this platform, "
              "reporting nothing (edit migs_profile.json to grant add-ons)\n");
  wmw2_cb_post_restore(NULL, 0);
}
