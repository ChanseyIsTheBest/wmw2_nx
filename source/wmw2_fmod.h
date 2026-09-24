/* wmw2_fmod.h -- interposes on the engine's FMOD::System::setDSPBufferSize()
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __WMW2_FMOD_H__
#define __WMW2_FMOD_H__

#include <stdint.h>

/* The import-table entry for _ZN4FMOD6System16setDSPBufferSizeEji. The engine
 * binds to this instead of to libfmodex's export; see wmw2_fmod.c for why. */
int wmw2_fmod_setDSPBufferSize(void *system, unsigned int bufferlength, int numbuffers);

/* Hand over libfmodex's own setDSPBufferSize. Must be called from
 * resolve_entry_points(), before so_finalize() -- after it the module's symbol
 * table is no longer readable. The engine does not call FMOD until the Chassis
 * starts, long after this. */
void wmw2_fmod_bind(uintptr_t real_setDSPBufferSize);

#endif
