/* wmw2_probe.h -- runtime instrumentation for the main-menu loading hang
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __WMW2_PROBE_H__
#define __WMW2_PROBE_H__

#include "so_util.h"

/* Install the Screen_MainMenu lifecycle probes AND the offline replacement for
 * the "reload profile from server" load step -- the step the loading screen
 * otherwise waits on forever. See wmw2_probe.c.
 *
 * Call once, after so_relocate()/so_resolve() have filled in the vtables and
 * BEFORE so_finalize() remaps the module read-execute -- the slots being
 * patched live in .data.rel.ro, which is writable up to that point.
 *
 * Costs nothing when the probe fires rarely and self-limits when it does not,
 * so it is safe to leave installed. */
void wmw2_probe_install(so_module *mod);

#endif
