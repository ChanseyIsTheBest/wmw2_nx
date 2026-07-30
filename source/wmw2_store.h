/* wmw2_store.h -- restore add-on content bought on Google Play
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __WMW2_STORE_H__
#define __WMW2_STORE_H__

/* Answer a restore request with "nothing to restore".
 *
 * Add-ons are granted by editing migs_profile.json (IAPInfo__<item>.BuyCount),
 * not by a separate file. Still posted rather than called directly --
 * jniNotifyNewMigsInfoStoreProductToRestore, exactly as a Google Play restore
 * would have. If the file is absent or lists nothing, queues
 * jniNotifyNewMigsInfoStoreNothingToRestore instead -- which the engine also
 * needs to hear, or the Restore Purchases button never stops spinning.
 *
 * Safe to call more than once: the in-game button re-reads the file. */
void wmw2_store_restore(void);

#endif
