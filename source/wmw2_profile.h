/* wmw2_profile.h -- supply the user identifier MIGS would have assigned
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __WMW2_PROFILE_H__
#define __WMW2_PROFILE_H__

#include <stddef.h>

/* The player's numeric user id, stable across launches.
 *
 * Read from <gamedir>/userid.txt, generated and written on first run. Never
 * changes afterwards -- a new id every launch would look to the engine like a
 * different player each time. */
const char *wmw2_user_identifier(void);

/* Rewrite a factory_profile.json document in place-ish, returning a NEW buffer
 * with PlayerData__UserIdentifier's EventValue replaced by wmw2_user_identifier().
 *
 * Caller owns the result and must free() it. Returns NULL if `in` does not look
 * like the document we expect, in which case use the original unchanged.
 * `*out_len` receives the new length. */
char *wmw2_profile_fixup(const char *in, size_t in_len, size_t *out_len);

/* True if `name` is a file whose contents should go through wmw2_profile_fixup
 * before being handed to the engine. */
int wmw2_profile_needs_fixup(const char *basename);

/* Merge `delta` (a flat JSON object of objects, as sent by
 * jniMigsRequestProfileModify) into `base`, replacing members that already
 * exist and appending those that do not. Returns a NEW buffer the caller owns,
 * or NULL if either document is not the expected shape.
 *
 * This is what a MIGS server does: you modify, then you re-read and see your
 * modification. Returning an unchanged factory profile forever is why
 * Screen_MainMenu::_reloadProfileToGetLatestFromServer never stopped polling. */
char *wmw2_profile_merge(const char *base, const char *delta, size_t *out_len);

/* Record a modification the engine has just made. Applied lazily. */
void wmw2_profile_note_modify(const char *delta);

/* The profile as the engine should currently see it: the player's saved
 * document if there is one, otherwise the shipped factory profile, plus every
 * modification made since. This is the save file -- see wmw2_profile.c. */
const char *wmw2_profile_current(void);

#endif
