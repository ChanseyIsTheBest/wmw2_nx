/* wmw_assetindex.c -- in-memory index of the asset tree
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * See wmw_assetindex.h for why this exists.
 *
 * Implementation is deliberately plain: one recursive walk at startup into an
 * open-addressed hash set of path strings. Roughly 4000 entries for this game,
 * a few hundred KB including the strings. No deletion, no resizing after build,
 * no locking needed on lookup because the table is immutable once built.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>

#include "wmw_assetindex.h"
#include "wmw_paths.h"
#include "libc_shim.h"
#include "util.h"

#define IDX_CAP 16384          // power of two, comfortably over ~4000 entries

/* On-disk cache of the walk.
 *
 * The tree is ~5900 files and the walk opens every directory through the
 * file-table lock, which on SD card is the single slowest thing the port does
 * before the engine starts. The result is entirely determined by the contents
 * of assets/, and those only change when the user copies new game data in, so
 * it is worth writing down.
 *
 * Format, all little-endian:
 *     magic   "WMW2AIX1"      8 bytes
 *     stamp   struct below
 *     entries count NUL-terminated paths, RELATIVE to the asset root
 *
 * Paths are stored relative so the cache survives the game folder being moved
 * or renamed, which is otherwise a confusing failure -- a cache full of stale
 * absolute paths would answer "yes, that exists" for every file and the engine
 * would fail much later trying to open them. */
#define CACHE_MAGIC "WMW2AIX1"
#define CACHE_NAME  "assetindex.cache"

typedef struct {
  int64_t  root_mtime;   // mtime of assets/ itself
  uint32_t entries;      // how many paths follow
  uint32_t bytes;        // total size of the path block, for a cheap sanity check
} CacheStamp;

static char *s_slot[IDX_CAP];
static int s_count;
static int s_built;
static char s_root[WMW_PATH_MAX];
static size_t s_root_len;

static uint64_t hash_path(const char *s) {
  uint64_t h = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= *p;
    h *= 1099511628211ULL;
  }
  return h;
}

static void index_insert(const char *path) {
  if (s_count >= IDX_CAP / 2) return;   // keep the table sparse
  uint64_t i = hash_path(path) & (IDX_CAP - 1);
  while (s_slot[i]) {
    if (strcmp(s_slot[i], path) == 0) return;
    i = (i + 1) & (IDX_CAP - 1);
  }
  s_slot[i] = strdup(path);
  if (s_slot[i]) s_count++;
}

static int index_contains(const char *path) {
  uint64_t i = hash_path(path) & (IDX_CAP - 1);
  while (s_slot[i]) {
    if (strcmp(s_slot[i], path) == 0) return 1;
    i = (i + 1) & (IDX_CAP - 1);
  }
  return 0;
}

// Recursive walk. Depth-limited purely as a guard against a symlink loop or a
// malformed tree; the real asset tree is three levels deep.
static void walk(const char *dir, int depth) {
  if (depth > 8) return;

  wmw_file_lock();
  DIR *d = opendir(dir);
  wmw_file_unlock();
  if (!d) return;

  for (;;) {
    wmw_file_lock();
    struct dirent *e = readdir(d);
    wmw_file_unlock();
    if (!e) break;

    if (e->d_name[0] == '.' &&
        (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
      continue;

    char child[WMW_PATH_MAX];
    if ((int)snprintf(child, sizeof(child), "%s/%s", dir, e->d_name) >= (int)sizeof(child))
      continue;

    int is_dir = 0;
#ifdef DT_DIR
    if (e->d_type == DT_DIR)       is_dir = 1;
    else if (e->d_type == DT_UNKNOWN)
#endif
    {
      struct stat st;
      wmw_file_lock();
      const int ok = (stat(child, &st) == 0);
      wmw_file_unlock();
      if (ok && S_ISDIR(st.st_mode)) is_dir = 1;
    }

    if (is_dir) walk(child, depth + 1);
    else        index_insert(child);
  }

  wmw_file_lock();
  closedir(d);
  wmw_file_unlock();
}

static void cache_path(char *out, size_t n) {
  snprintf(out, n, "%s/" CACHE_NAME, wmw_game_dir());
}

/* Returns 1 if the index was populated from the cache. */
static int cache_load(int64_t root_mtime) {
  char path[WMW_PATH_MAX];
  cache_path(path, sizeof(path));

  wmw_file_lock();
  FILE *f = fopen(path, "rb");
  wmw_file_unlock();
  if (!f) return 0;

  char magic[8];
  CacheStamp st;
  int ok = (fread(magic, 1, sizeof(magic), f) == sizeof(magic)) &&
           (memcmp(magic, CACHE_MAGIC, 8) == 0) &&
           (fread(&st, 1, sizeof(st), f) == sizeof(st));

  /* A changed mtime on assets/ means game data was added or removed. Anything
   * finer would need the walk this exists to avoid. */
  if (ok && st.root_mtime != root_mtime) {
    debugPrintf("assetindex: cache is stale (assets/ changed) -- rescanning\n");
    ok = 0;
  }
  if (ok && (st.entries == 0 || st.entries > IDX_CAP / 2 ||
             st.bytes == 0 || st.bytes > 8u * 1024 * 1024))
    ok = 0;

  char *blob = NULL;
  if (ok) {
    blob = malloc(st.bytes + 1);
    ok = blob && (fread(blob, 1, st.bytes, f) == st.bytes);
    if (ok) blob[st.bytes] = '\0';
  }
  fclose(f);

  if (!ok) {
    free(blob);
    return 0;
  }

  /* Re-absolutise as we go: the file holds paths relative to the asset root. */
  char full[WMW_PATH_MAX];
  const char *p = blob;
  const char *end = blob + st.bytes;
  uint32_t n = 0;
  while (p < end && n < st.entries) {
    const size_t len = strlen(p);
    if (len && (int)snprintf(full, sizeof(full), "%s/%s", s_root, p) < (int)sizeof(full))
      index_insert(full);
    p += len + 1;
    n++;
  }
  free(blob);

  if (s_count == 0) return 0;
  debugPrintf("assetindex: %d files from cache (walk skipped)\n", s_count);
  return 1;
}

static void cache_save(int64_t root_mtime) {
  char path[WMW_PATH_MAX];
  cache_path(path, sizeof(path));

  wmw_file_lock();
  FILE *f = fopen(path, "wb");
  wmw_file_unlock();
  if (!f) return;

  /* Two passes: size the path block, then write it. Cheaper than building the
   * whole thing in memory when it is already sitting in the table. */
  uint32_t bytes = 0;
  for (int i = 0; i < IDX_CAP; i++)
    if (s_slot[i] && strlen(s_slot[i]) > s_root_len)
      bytes += (uint32_t)(strlen(s_slot[i]) - s_root_len - 1) + 1;

  CacheStamp st = { root_mtime, (uint32_t)s_count, bytes };
  fwrite(CACHE_MAGIC, 1, 8, f);
  fwrite(&st, 1, sizeof(st), f);
  for (int i = 0; i < IDX_CAP; i++) {
    if (!s_slot[i]) continue;
    const size_t len = strlen(s_slot[i]);
    if (len <= s_root_len + 1) continue;
    const char *rel = s_slot[i] + s_root_len + 1;     /* skip "<root>/" */
    fwrite(rel, 1, strlen(rel) + 1, f);
  }
  fclose(f);
  debugPrintf("assetindex: cache written (%u entries, %u bytes of paths)\n",
              st.entries, bytes);
}

void wmw_assetindex_build(void) {
  if (s_built) return;
  s_built = 1;

  snprintf(s_root, sizeof(s_root), "%s/assets", wmw_game_dir());
  s_root_len = strlen(s_root);

  struct stat st;
  if (stat(s_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
    debugPrintf("assetindex: %s not found -- lookups will hit the filesystem\n", s_root);
    s_root[0] = '\0';
    return;
  }

  const int64_t root_mtime = (int64_t)st.st_mtime;
  if (cache_load(root_mtime))
    return;

  walk(s_root, 0);
  debugPrintf("assetindex: %d files indexed under %s\n", s_count, s_root);
  cache_save(root_mtime);
}

int wmw_assetindex_lookup(const char *path) {
  if (!s_built || !s_root[0] || !path) return -1;

  // Only authoritative inside the tree we walked. Anything else -- save data,
  // the database, the generated bundle, downloaded content -- can change while
  // the game runs, so it must still go to the filesystem.
  if (strncmp(path, s_root, s_root_len) != 0 || path[s_root_len] != '/')
    return -1;

  return index_contains(path) ? 1 : 0;
}
