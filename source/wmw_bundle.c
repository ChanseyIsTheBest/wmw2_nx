/* wmw_bundle.c -- give the engine an openable archive
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * The engine treats rendererInit's packagePath as an archive and opens it with
 * its own statically linked minizip ("ZipArchiveReader::readCurrentFile",
 * "wUnzOpen*" in .rodata). On Android that was the APK. Point it at a directory
 * and the open fails -- and the failure is not handled gracefully:
 *
 *     fopen(sdmc:/switch/wmw_nx, rb) -> FAIL
 *     [WMW] RendererResize failed with exception: std::bad_alloc
 *
 * The allocation immediately after the failed open is sized from something the
 * engine never managed to read, so it throws. The same failure breaks
 * ApplicationContext::copyDatabaseFromBundle(), which extracts water.db.
 *
 * The fix is simply to hand it something openable:
 *
 *   1. If an .apk is sitting next to the .nro, use that -- it is the real
 *      thing and contains everything.
 *   2. Otherwise synthesise a small but completely valid ZIP. A successful
 *      open with an entry not found is a case the engine already handles
 *      (every loose asset already resolves that way); a failed open is not.
 *
 * WMW2 note: WMW1 shipped a water.db inside assets/Data/ that the engine
 * extracted on first run, so the synthesised archive carried it. WMW2 has no
 * such file -- its database is GCS.db, created at runtime in the work
 * directory passed to jniWalaberChassisStartup -- so on WMW2 there is nothing
 * to package and the archive comes out EMPTY. That is fine and is the point:
 * an empty archive still opens, which is the only thing the engine needs from
 * it. Returning NULL here instead would send main.c down the "hand it a
 * directory" path and straight into the bad_alloc above.
 *
 * The generated archive stores entries uncompressed, which keeps this to a few
 * dozen lines of header writing and costs nothing at 216 KB.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <zlib.h>

#include "wmw_bundle.h"
#include "wmw_paths.h"
#include "wmw2_profile.h"
#include "config.h"
#include "util.h"

#define BUNDLE_NAME "bundle.zip"

// --- little-endian writers -------------------------------------------------

static void w16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void w32(FILE *f, uint32_t v) {
  fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f);
  fputc((v >> 16) & 0xff, f); fputc((v >> 24) & 0xff, f);
}





/* What goes in the archive: everything under assets/Water/Data/.
 *
 * The engine extracts several files from its bundle on first run -- the two
 * databases plus factory_profile.json, store.json and news.json were all
 * observed on a single boot. Shipping exactly those five would be betting that
 * one boot saw the complete set, and the cost of losing that bet is high: a
 * missing entry does not fail loudly, it leaves an EMPTY destination file,
 * which then detonates somewhere else entirely. That is how factory_profile
 * killed a boot inside std::stoull, several seconds and one subsystem away from
 * the actual mistake.
 *
 * The whole directory is 150 files and 0.8 MB uncompressed, so there is no
 * reason to be clever. Walk it and take everything.
 *
 * Entries are written streaming -- read, deflate-free copy, free -- so peak
 * memory is one file rather than the whole tree.
 */

#define MAX_ENTRIES 512

typedef struct {
  char     name[320];
  uint32_t crc;
  uint32_t size;
  uint32_t local_off;
} CdEntry;

/* Copy one file into the archive at the current offset, computing its CRC as it
 * goes, and record what the central directory will need. */
static int add_file(FILE *out, const char *src, const char *entry_name,
                    CdEntry *cd) {
  FILE *in = fopen(src, "rb");
  if (!in) return 0;
  fseek(in, 0, SEEK_END);
  const long n = ftell(in);
  fseek(in, 0, SEEK_SET);
  if (n < 0 || n > 64 * 1024 * 1024) { fclose(in); return 0; }

  void *buf = malloc((size_t)n ? (size_t)n : 1);
  if (!buf) { fclose(in); return 0; }
  size_t got = fread(buf, 1, (size_t)n, in);
  fclose(in);
  if (got != (size_t)n) { free(buf); return 0; }

  /* factory_profile.json ships with UserIdentifier as the literal string
   * "null", which the engine feeds straight to std::stoull() and aborts on.
   * Substitute a real id here so the copy the engine extracts is already
   * valid, whichever path it reads it through. See wmw2_profile.c. */
  {
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    if (wmw2_profile_needs_fixup(base)) {
      size_t patched_len = 0;
      char *patched = wmw2_profile_fixup((const char *)buf, got, &patched_len);
      if (patched) { free(buf); buf = patched; got = patched_len; }
    }
  }

  const uint32_t crc = (uint32_t)crc32(0, (const Bytef *)buf, (uInt)got);
  snprintf(cd->name, sizeof(cd->name), "%s", entry_name);
  cd->crc = crc;
  cd->size = (uint32_t)got;
  cd->local_off = (uint32_t)ftell(out);

  const size_t namelen = strlen(cd->name);
  w32(out, 0x04034b50);
  w16(out, 20); w16(out, 0); w16(out, 0);
  w16(out, 0); w16(out, 0);
  w32(out, crc);
  w32(out, (uint32_t)got); w32(out, (uint32_t)got);
  w16(out, (uint16_t)namelen); w16(out, 0);
  fwrite(cd->name, 1, namelen, out);
  fwrite(buf, 1, got, out);
  free(buf);
  return 1;
}

static int build_bundle(const char *path) {
  static CdEntry cd[MAX_ENTRIES];
  int n = 0;

  char dir[WMW_PATH_MAX];
  snprintf(dir, sizeof(dir), "%s/assets/%s/Data", wmw_game_dir(), WMW2_ASSET_SUBDIR);

  DIR *d = opendir(dir);
  if (!d) {
    debugPrintf("bundle: cannot open %s -- archive will be empty\n", dir);
  }

  FILE *out = fopen(path, "wb");
  if (!out) {
    if (d) closedir(d);
    debugPrintf("bundle: could not create %s\n", path);
    return 0;
  }

  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) && n + 2 <= MAX_ENTRIES) {
      if (e->d_name[0] == '.') continue;

      char src[WMW_PATH_MAX];
      if (snprintf(src, sizeof(src), "%s/%s", dir, e->d_name) >= (int)sizeof(src))
        continue;
      struct stat st;
      if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) continue;

      /* Two entry names for the same file. The engine builds its loose paths
       * as "assets/Water/Data/<name>", so that is the likely key, but the
       * archive-relative form may drop the assets/ prefix. A duplicate costs
       * only its own bytes and removes a guess. */
      char en[320];
      /* A truncated entry name is a WRONG key, not a shorter one, so check
       * rather than trust. Nothing in this tree comes close, but the failure
       * would be silent and would look like a missing file. */
      if (snprintf(en, sizeof(en), "assets/%s/Data/%s",
                   WMW2_ASSET_SUBDIR, e->d_name) < (int)sizeof(en)) {
        if (add_file(out, src, en, &cd[n])) n++;
      }
      if (snprintf(en, sizeof(en), "%s/Data/%s",
                   WMW2_ASSET_SUBDIR, e->d_name) < (int)sizeof(en)) {
        if (add_file(out, src, en, &cd[n])) n++;
      }
    }
    closedir(d);
  }

  const uint32_t cd_off = (uint32_t)ftell(out);
  for (int i = 0; i < n; i++) {
    const size_t namelen = strlen(cd[i].name);
    w32(out, 0x02014b50);
    w16(out, 20); w16(out, 20);
    w16(out, 0); w16(out, 0);
    w16(out, 0); w16(out, 0);
    w32(out, cd[i].crc);
    w32(out, cd[i].size); w32(out, cd[i].size);
    w16(out, (uint16_t)namelen);
    w16(out, 0); w16(out, 0);
    w16(out, 0);
    w16(out, 0); w32(out, 0);
    w32(out, cd[i].local_off);
    fwrite(cd[i].name, 1, namelen, out);
  }
  const uint32_t cd_size = (uint32_t)ftell(out) - cd_off;

  w32(out, 0x06054b50);
  w16(out, 0); w16(out, 0);
  w16(out, (uint16_t)n); w16(out, (uint16_t)n);
  w32(out, cd_size);
  w32(out, cd_off);
  w16(out, 0);

  const long total = ftell(out);
  fclose(out);
  debugPrintf("bundle: built %s -- %d entries, %ld bytes\n", path, n, total);
  return 1;
}

const char *wmw_bundle_path(const char *unused) {
  (void)unused;
  static char path[WMW_PATH_MAX];
  static int built = 0;
  snprintf(path, sizeof(path), "%s/" BUNDLE_NAME, wmw_game_dir());

  if (!built) {
    /* Rebuilt every launch rather than cached. It is a few hundred KB of
     * uncompressed copies of files that are already on the card, and a stale
     * bundle after a game-data update is a genuinely confusing failure. */
    built = build_bundle(path);
    if (!built) return NULL;
  }
  return path;
}
