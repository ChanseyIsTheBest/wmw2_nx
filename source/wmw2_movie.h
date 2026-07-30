/* wmw2_movie.h -- cutscene playback for assets/Water/Movies/
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __WMW2_MOVIE_H__
#define __WMW2_MOVIE_H__

#include <stdint.h>

/* Hand the player the two things it cannot reach on its own: a function that
 * puts the current frame on screen (rotation + swap), and the render size.
 * Call once, after EGL and wmw_tate are up. */
void wmw2_movie_init(void (*present)(void), int render_w, int render_h);

/* Play one clip, blocking until it ends or the player skips it.
 *
 * `engine_path` is what the engine passed to jniPlayMovie, e.g.
 * "/Water/Movies/location1_swampy.mp4"; it is resolved against the asset root.
 * Returns 1 if something was played, 0 if the file could not be opened -- in
 * which case the caller should carry on exactly as it did before, reporting the
 * movie finished immediately. */
int wmw2_movie_play(const char *engine_path);

/* Mix any pending movie audio into an output block, called from the FMOD pump.
 * Returns the number of frames mixed. Safe to call when nothing is playing. */
int wmw2_movie_mix_s16(int16_t *dst, int frames, int channels);

/* True while a clip is on screen. */
int wmw2_movie_is_playing(void);

#endif
