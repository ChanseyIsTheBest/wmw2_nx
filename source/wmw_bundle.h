/* wmw_bundle.h -- give the engine an openable archive
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __WMW_BUNDLE_H__
#define __WMW_BUNDLE_H__

// Build (once per launch) an archive holding the five files the engine
// extracts from its bundle, and return its path. NULL if it could not be
// written.
//
// WMW2 does not receive this path directly -- it is handed the game DIRECTORY,
// because it also builds loose paths by concatenating onto that argument. The
// archive is substituted by fopen_fake() when the engine opens the directory as
// a file. See wmw_bundle.c for what is in it and why.
const char *wmw_bundle_path(const char *unused);

#endif
