/* wmw2_fmod.c -- interposes on the engine's FMOD::System::setDSPBufferSize()
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Why this exists
 * ---------------
 * libwalaber.so configures FMOD like this, before System::init():
 *
 *     setDSPBufferSize(1024, 4);   // @0x78c03c
 *     setOutput(21);               // FMOD_OUTPUTTYPE_OPENSL
 *     init(64, 0, 0);
 *
 * FMOD's OpenSL output keeps numbuffers blocks of bufferlength frames queued,
 * and the newest mixed block sits (numbuffers - 1) blocks from the play head.
 * FMOD mixes at its default 24000 Hz here (the engine's setSoftwareFormat call
 * comes after init() and fails), so 1024-frame blocks put ~128 ms of FMOD's own
 * buffering between a sound starting and it reaching the speaker -- before the
 * port adds anything. WMW1 runs the identical libfmodex at FMOD's default of
 * 512 x 4, i.e. ~64 ms.
 *
 * Measured with the OpenSL shim fix in place (see opensles.c), tap to audible:
 *     game's own 1024 x 4    ~181 ms
 *     512 x 4 (WMW1's)       ~113 ms
 *
 * So by default the length is overridden to WMW2_FMOD_DSP_BUFFER_LENGTH and the
 * buffer count is left as the engine asked. Set that to 0 in config.h to pass
 * the engine's request through untouched.
 *
 * Nothing else about FMOD is touched: every other FMOD import still binds
 * straight to the real libfmodex. This one binds here only because so_resolve()
 * consults the static import table before sibling modules' exports.
 */

#include <stddef.h>
#include <stdint.h>

#include "wmw2_fmod.h"
#include "config.h"
#include "util.h"

#ifndef WMW2_FMOD_DSP_BUFFER_LENGTH
#define WMW2_FMOD_DSP_BUFFER_LENGTH 512
#endif

typedef int (*fn_setDSPBufferSize)(void *system, unsigned int bufferlength, int numbuffers);
static fn_setDSPBufferSize s_real;

void wmw2_fmod_bind(uintptr_t real_setDSPBufferSize) {
  s_real = (fn_setDSPBufferSize)real_setDSPBufferSize;
  if (!s_real)
    debugPrintf("fmod: setDSPBufferSize not exported by libfmodex -- "
                "engine's call will be ignored, FMOD keeps its default buffer\n");
}

int wmw2_fmod_setDSPBufferSize(void *system, unsigned int bufferlength, int numbuffers) {
  unsigned int len = bufferlength;
  if (WMW2_FMOD_DSP_BUFFER_LENGTH > 0)
    len = (unsigned int)WMW2_FMOD_DSP_BUFFER_LENGTH;

  if (!s_real)
    return 0; /* FMOD_OK: FMOD's own default (512 x 4) stands, which is fine */

  const int r = s_real(system, len, numbuffers);
  debugPrintf("fmod: setDSPBufferSize(%u, %d) -> using %u x %d (%.1f ms/block at 24 kHz) = %d\n",
              bufferlength, numbuffers, len, numbuffers, len * 1000.0 / 24000.0, r);
  return r;
}
