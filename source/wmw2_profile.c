/* wmw2_profile.c -- supply the user identifier MIGS would have assigned
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * THE BUG THIS EXISTS TO FIX
 * --------------------------
 * assets/Water/Data/factory_profile.json is the new-player profile, and every
 * value in it is a number held as a string -- "EventValue": "7" and so on. The
 * engine reads them back with std::stoull().
 *
 * Every value except one. Out of seventy entries, exactly this one:
 *
 *     "PlayerData__UserIdentifier": {
 *         "EventName":  "UserIdentifier",
 *         "EventValue": "null",          <-- the literal four characters
 *         "EventStringValue": "null"
 *     }
 *
 * stoull("null") performs no conversion, throws std::invalid_argument, and
 * since nothing catches it libc++ calls abort() out of std::terminate:
 *
 *     terminating with uncaught exception of type std::invalid_argument:
 *     stoull: no conversion
 *
 * On Android this never fires because the id is not "null" by the time anything
 * reads it -- MIGS assigns one. The engine even has a dedicated request for it,
 * jniMigsRequestProfileSynthID, "synthesise ID". There is no MIGS here, so the
 * factory placeholder survives to the first parse and takes the process down.
 *
 * THE FORMAT MATTERS, AND IT IS NOT "A NUMBER"
 * -------------------------------------------
 * A plain decimal id is still wrong, and cost its own boot. Water::_getUserIdentifier()
 * is the only place in the engine that touches this value, and it does:
 *
 *     log("About to call split %s")
 *     Walaber::StringHelper::split(id, '-')        // split on dashes
 *     operator+(pieces[n-2], pieces[n-1])          // concat the LAST TWO
 *     std::stoull(concat, nullptr, 16)             // parse as HEX
 *
 * So it is a UUID. The last two groups of a canonical UUID are 4 and 12 hex
 * characters -- concatenated, exactly 16 hex digits, which is exactly the range
 * of an unsigned long long. That fit is not a coincidence; it is the format.
 *
 * Given a value with no dashes, split() returns a single piece, the "second to
 * last" element reads before the start of the vector, and stoull sees nothing
 * convertible:
 *
 *     terminating with uncaught exception of type std::invalid_argument:
 *     stoull: no conversion
 *
 * which is the same message as an empty document, from a completely different
 * cause. That ambiguity is what made this take so long to pin down.
 *
 * The id is written into the copy of factory_profile.json that goes into
 * bundle.zip, so the engine reads a valid document however it reaches it.
 *
 * Note this is not patching game content. The value is platform-supplied on
 * every platform; the port is the platform here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <switch.h>

#include "wmw2_profile.h"
#include "wmw_paths.h"
#include "wmw2_callbacks.h"   /* wmw2_factory_profile_json() */
#include "util.h"

#define ID_FILE "userid.txt"

static char s_id[48];   /* a canonical UUID is 36 chars + NUL; 32 was not enough */

const char *wmw2_user_identifier(void) {
  if (s_id[0])
    return s_id;

  char path[WMW_PATH_MAX];
  snprintf(path, sizeof(path), "%s/" ID_FILE, wmw_game_dir());

  /* Reuse the stored id if there is one. Stability matters more than
   * uniqueness: the engine keys per-player state off this, so a fresh id each
   * launch would read as a different player every time. */
  FILE *f = fopen(path, "r");
  if (f) {
    /* 36 characters of UUID, plus the newline fgets keeps, plus the NUL. */
    char buf[64] = {0};
    if (fgets(buf, sizeof(buf), f)) {
      size_t n = strspn(buf, "0123456789abcdefABCDEF-");
      /* Must look like a UUID, not merely be non-empty. An earlier build wrote
       * a plain decimal id here; reject it so it is regenerated rather than
       * loaded and used to abort the engine. */
      if (n == 36 && buf[8] == '-' && buf[13] == '-' && buf[18] == '-' && buf[23] == '-') {
        memcpy(s_id, buf, n);
        s_id[n] = '\0';
      } else {
        debugPrintf("profile: %s does not hold a UUID -- regenerating\n", ID_FILE);
      }
    }
    fclose(f);
    if (s_id[0]) {
      debugPrintf("profile: user id %s (from %s)\n", s_id, ID_FILE);
      return s_id;
    }
  }

  /* First run: synthesise a canonical UUID. Mix the wall clock with the system
   * tick so two consoles do not collide.
   *
   * Only the last two groups are ever parsed (as one 16-digit hex number), but
   * the whole thing is written in the usual 8-4-4-4-12 shape because that is
   * what the value is, and because split() needs the dashes to be there. */
  uint64_t a = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ull;
  a ^= armGetSystemTick();
  uint64_t b = a * 0xBF58476D1CE4E5B9ull;
  b ^= (b >> 31);
  /* Force the first character to be a decimal digit.
   *
   * Water::_getUserIdentifier() only ever parses the LAST two groups, as hex,
   * so the leading character is free as far as it is concerned. But the engine
   * has a second std::stoull site -- InterstitialManager's constructor -- which
   * reads player data in BASE 10, and stoull only throws when it can convert
   * nothing at all. A UUID beginning "157f..." yields 157 and is harmless; one
   * beginning "a57f..." converts nothing and aborts the process.
   *
   * Whether that site ever sees this value is unclear, and a one-in-three
   * chance of a save that crashes on some future launch is not worth leaving to
   * find out. Costs four bits of entropy out of 128. */
  unsigned lead = (unsigned)(a >> 32);
  if (((lead >> 28) & 0xf) > 9)
    lead &= 0x9fffffffu;                  /* top nibble into 0-9 */

  snprintf(s_id, sizeof(s_id), "%08x-%04x-%04x-%04x-%012llx",
           lead, (unsigned)((a >> 16) & 0xffff),
           (unsigned)(a & 0xffff), (unsigned)((b >> 48) & 0xffff),
           (unsigned long long)(b & 0xffffffffffffull));

  f = fopen(path, "w");
  if (f) {
    fprintf(f, "%s\n", s_id);
    fclose(f);
    debugPrintf("profile: generated user id %s -> %s\n", s_id, ID_FILE);
  } else {
    debugPrintf("profile: user id %s (could not persist to %s)\n", s_id, path);
  }
  return s_id;
}

int wmw2_profile_needs_fixup(const char *basename) {
  return basename && strcmp(basename, "factory_profile.json") == 0;
}

char *wmw2_profile_fixup(const char *in, size_t in_len, size_t *out_len) {
  if (!in || !out_len)
    return NULL;

  /* Locate the UserIdentifier block, then the "EventValue" inside it. Anchoring
   * on the key rather than searching for the bare string "null" matters --
   * thirty-three other entries have "EventStringValue": "null", and those are
   * string fields the engine does not run through stoull. Rewriting them would
   * change values the game is entitled to see as absent. */
  const char *key = strstr(in, "\"PlayerData__UserIdentifier\"");
  if (!key)
    return NULL;

  const char *ev = strstr(key, "\"EventValue\"");
  if (!ev || (size_t)(ev - in) > in_len)
    return NULL;

  /* The opening quote of the value, just past the colon. */
  const char *p = strchr(ev + strlen("\"EventValue\""), ':');
  if (!p) return NULL;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '"') return NULL;
  const char *val_start = p + 1;
  const char *val_end = strchr(val_start, '"');
  if (!val_end) return NULL;

  /* Already numeric? Then this document has been through here before, or the
   * game supplied a real id -- leave it alone. */
  if (val_end > val_start && strspn(val_start, "0123456789") == (size_t)(val_end - val_start))
    return NULL;

  const char *id = wmw2_user_identifier();
  const size_t id_len = strlen(id);
  const size_t head = (size_t)(val_start - in);
  const size_t tail_off = (size_t)(val_end - in);
  const size_t tail_len = in_len - tail_off;

  char *out = malloc(head + id_len + tail_len + 1);
  if (!out) return NULL;
  memcpy(out, in, head);
  memcpy(out + head, id, id_len);
  memcpy(out + head + id_len, in + tail_off, tail_len);
  out[head + id_len + tail_len] = '\0';
  *out_len = head + id_len + tail_len;

  debugPrintf("profile: UserIdentifier \"null\" -> \"%s\"\n", id);
  return out;
}


/* --------------------------------------------------------------------------
 * Merging a modification delta into the profile
 *
 * Both documents are flat objects whose members are themselves objects:
 *
 *     { "PlayerData__HeartCount": { "EventName": ..., "EventValue": ... }, ... }
 *
 * so a member-level merge is enough -- no general JSON model required, just
 * brace matching and string-aware scanning. Anything unexpected returns NULL
 * and the caller keeps the original, which is the same behaviour as before this
 * existed.
 * ------------------------------------------------------------------------ */

/* Advance past a JSON value starting at *p, honouring strings and nesting.
 * Returns a pointer just past the value, or NULL if the document is malformed. */
static const char *skip_value(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  if (p >= end) return NULL;
  if (*p == '"') {
    p++;
    while (p < end && *p != '"') { if (*p == '\\' && p + 1 < end) p++; p++; }
    return (p < end) ? p + 1 : NULL;
  }
  if (*p == '{' || *p == '[') {
    const char open = *p, close = (open == '{') ? '}' : ']';
    int depth = 0;
    while (p < end) {
      if (*p == '"') {
        p++;
        while (p < end && *p != '"') { if (*p == '\\' && p + 1 < end) p++; p++; }
        if (p >= end) return NULL;
      } else if (*p == open) depth++;
      else if (*p == close) { depth--; if (!depth) return p + 1; }
      p++;
    }
    return NULL;
  }
  while (p < end && *p != ',' && *p != '}' && *p != ']' &&
         *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
  return p;
}

/* Find the member named `key` in `doc`, returning its full "key": value span. */
static int find_member(const char *doc, size_t len, const char *key, size_t klen,
                       size_t *start, size_t *stop) {
  const char *end = doc + len;
  const char *p = doc;
  while (p < end) {
    while (p < end && *p != '"') p++;
    if (p >= end) return 0;
    const char *ks = p + 1;
    const char *ke = ks;
    while (ke < end && *ke != '"') { if (*ke == '\\' && ke + 1 < end) ke++; ke++; }
    if (ke >= end) return 0;
    const char *after = ke + 1;
    while (after < end && (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r')) after++;
    if (after < end && *after == ':') {
      const char *ve = skip_value(after + 1, end);
      if (!ve) return 0;
      if ((size_t)(ke - ks) == klen && memcmp(ks, key, klen) == 0) {
        *start = (size_t)(p - doc);
        *stop  = (size_t)(ve - doc);
        return 1;
      }
      p = ve;
    } else {
      p = ke + 1;
    }
  }
  return 0;
}

char *wmw2_profile_merge(const char *base, const char *delta, size_t *out_len) {
  if (!base || !delta || !out_len) return NULL;
  const size_t blen = strlen(base), dlen = strlen(delta);
  if (blen < 2 || dlen < 2) return NULL;

  /* Work on a growable copy of base. */
  size_t cap = blen + dlen + 64;
  char *out = malloc(cap);
  if (!out) return NULL;
  memcpy(out, base, blen);
  out[blen] = '\0';
  size_t olen = blen;
  int merged = 0;

  /* Walk the delta's top-level members. */
  const char *dend = delta + dlen;
  const char *p = delta;
  while (p < dend) {
    while (p < dend && *p != '"') p++;
    if (p >= dend) break;
    const char *ks = p + 1, *ke = ks;
    while (ke < dend && *ke != '"') { if (*ke == '\\' && ke + 1 < dend) ke++; ke++; }
    if (ke >= dend) break;
    const char *after = ke + 1;
    while (after < dend && (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r')) after++;
    if (after >= dend || *after != ':') { p = ke + 1; continue; }
    const char *ve = skip_value(after + 1, dend);
    if (!ve) break;

    const size_t klen = (size_t)(ke - ks);
    const size_t mlen = (size_t)(ve - p);          /* the whole "key": value */

    size_t ms, me;
    if (find_member(out, olen, ks, klen, &ms, &me)) {
      /* Replace in place. */
      const size_t tail = olen - me;
      if (olen - (me - ms) + mlen + 1 > cap) {
        cap = olen - (me - ms) + mlen + 64;
        char *n = realloc(out, cap);
        if (!n) { free(out); return NULL; }
        out = n;
      }
      memmove(out + ms + mlen, out + me, tail);
      memcpy(out + ms, p, mlen);
      olen = ms + mlen + tail;
      out[olen] = '\0';
    } else {
      /* Append before the closing brace. */
      char *close = strrchr(out, '}');
      if (!close) { free(out); return NULL; }
      const size_t ci = (size_t)(close - out);
      if (olen + mlen + 4 > cap) {
        cap = olen + mlen + 64;
        char *n = realloc(out, cap);
        if (!n) { free(out); return NULL; }
        out = n; close = out + ci;
      }
      memmove(out + ci + mlen + 2, out + ci, olen - ci);
      out[ci] = ',';
      out[ci + 1] = '\n';
      memcpy(out + ci + 2, p, mlen);
      olen += mlen + 2;
      out[olen] = '\0';
    }
    merged++;
    p = ve;
  }

  if (!merged) { free(out); return NULL; }
  *out_len = olen;
  debugPrintf("profile: merged %d member(s) from the engine's modification\n", merged);
  return out;
}


/* --------------------------------------------------------------------------
 * The profile as the engine should currently see it: the shipped factory
 * document plus every modification it has sent us.
 * ------------------------------------------------------------------------ */

static char  *s_current;        /* merged result, owned here */
static char  *s_pending_delta;  /* set by wmw2_profile_note_modify() */
static int    s_dirty = 1;
static int    s_loaded;         /* tried to read the saved profile yet? */

#define SAVE_NAME "migs_profile.json"

/* THE SAVE FILE.
 *
 * This is where the player's game actually lives, which took an embarrassingly
 * long time to establish. Neither database holds progress: perry.db is twenty
 * tables of static configuration, and levelinfo.db's LevelInfo table is the
 * level CATALOGUE -- ID, Location, FileName, MapPosition, DucksPossible -- with
 * no Completed, Unlocked or DucksCollected column anywhere in it.
 *
 * Progress exists only in the MIGS profile, which the engine pushes out one
 * delta at a time:
 *
 *     jniMigsRequestProfileModify({
 *       "LevelInfo__s_new_beginnings": { "Completed": "1", "DucksCollected": "3",
 *                                        "TimesFinished": "1", "Unlocked": "1" } })
 *
 * On Android MigsRelay persists that. Here there is no MIGS, so the port has to
 * be the store: accumulate every delta into one document and write it to disk.
 *
 * It also explains the symptom that made no sense -- age surviving while
 * everything else reset. Age is written to perry.db's Settings table by a
 * different path entirely, so it was never in the profile that kept getting
 * thrown away. Audio settings (Settings__MusicOn, Settings__AudioOn) are in the
 * profile, and reset exactly like level progress did. */

/* Fields whose value is genuinely text. Everything else in the profile is a
 * number held as a string, and the engine converts it with std::stoull. */
static int field_is_textual(const char *name, size_t len) {
  static const char *const kText[] = {
    "ID", "EventName", "EventStringValue", "Name", "Title", "InternalID", NULL
  };
  for (int i = 0; kText[i]; i++)
    if (strlen(kText[i]) == len && memcmp(kText[i], name, len) == 0) return 1;
  return 0;
}

/* Rewrite every empty numeric value as "0", in place (the replacement is the
 * same length plus one, so the caller supplies a buffer with room).
 *
 * The engine emits "" for numeric fields it has not set yet -- its own
 * modification deltas contain, verbatim:
 *
 *     "LevelInfo__s_split_second_decision": {
 *         "Completed": "", "DucksAnimated": "", "DucksCollected": "",
 *         "HasPlayedVideo": "", "ID": "s_split_second_decision",
 *         "Unlocked": "1" }
 *
 * It is happy to WRITE those. It cannot read them: the profile parser feeds
 * every numeric field to std::stoull, "" converts to nothing, and the
 * uncaught std::invalid_argument takes the process down.
 *
 * On Android this never surfaces because MIGS is what stores the profile, and
 * whatever it does with an empty field, the document handed back is one its own
 * parser accepts. Here the port is the store, so normalising is the port's job.
 * An unset counter is zero, which is what the engine would have inferred.
 *
 * Textual fields are left exactly as they are -- an empty ID or EventStringValue
 * means absent, and "0" would be a different and wrong claim. */
static char *normalise_empties(const char *in, size_t *out_len) {
  const size_t n = strlen(in);
  char *out = malloc(n * 2 + 2);       /* worst case every value grows by one */
  if (!out) return NULL;

  size_t o = 0;
  int fixed = 0;
  for (size_t i = 0; i < n; ) {
    /* Looking for:  "key"  optional-ws  :  optional-ws  ""  */
    if (in[i] == '"') {
      const size_t ks = i + 1;
      size_t ke = ks;
      while (ke < n && in[ke] != '"') ke++;
      if (ke < n) {
        size_t p = ke + 1;
        while (p < n && (in[p]==' '||in[p]=='\t'||in[p]=='\n'||in[p]=='\r')) p++;
        if (p < n && in[p] == ':') {
          p++;
          while (p < n && (in[p]==' '||in[p]=='\t'||in[p]=='\n'||in[p]=='\r')) p++;
          /* "" and "null" both mean "not set", and the engine parses both
           * numerically. The stoull hook caught it doing exactly that:
           *
           *     probe: stoull("null", base 10) has nothing to convert -> 0
           *     probe: stoull("null", base 16) has nothing to convert -> 0
           *
           * "null" only ever appears as an EventStringValue, which is not
           * purely textual: for the entries that use it -- HeartCount and
           * PreferredLanguage carry unix timestamps there -- it is a number. So
           * it gets zeroed, while a genuinely textual value like
           * LastPlayedNormalLevel's "s_new_beginnings" is left alone. */
          const int empty_str = (p + 1 < n && in[p] == '"' && in[p+1] == '"');
          const int is_evsv   = (ke - ks == 16 &&
                                 memcmp(in + ks, "EventStringValue", 16) == 0);
          const int null_str  = (p + 5 < n && memcmp(in + p, "\"null\"", 6) == 0);
          if ((empty_str && !field_is_textual(in + ks, ke - ks)) ||
              (null_str && is_evsv)) {
            memcpy(out + o, in + i, (p - i));      /* key, colon, spacing */
            o += (p - i);
            out[o++] = '"'; out[o++] = '0'; out[o++] = '"';
            i = p + (empty_str ? 2 : 6);           /* past "" or "null" */
            fixed++;
            continue;
          }
        }
      }
    }
    out[o++] = in[i++];
  }
  out[o] = '\0';
  if (out_len) *out_len = o;
  if (fixed) debugPrintf("profile: %d empty numeric field(s) normalised to 0\n", fixed);
  return out;
}

/* PlayerData events the engine knows about but factory_profile.json omits.
 *
 * Recovered from the name block in .rodata that sits alongside "PlayerData",
 * "EventValue" and "EventStringValue". Cross-referencing it against the shipped
 * profile leaves eight names the engine can look up and the document has no
 * record for:
 *
 *     PoisonWaterLosses      FirstCollectibleSighted  LastHourCount
 *     SwampyTouched          DoofIAPState             SocialHasConnectedToFB
 *     SynergyDuckID          SynergyDuckStamp
 *
 * That did not matter while the port answered the profile request with "{}" --
 * the engine fell back to its own defaults for everything. It matters now that
 * a real document is delivered, because the document becomes authoritative: a
 * lookup that finds no record yields an empty Property, and
 * WMW2SaveEntryProvider feeds that to std::stoull. InterstitialManager's
 * constructor does exactly this, in base 10, on one fixed slot.
 *
 * So supply the missing records with a zero value. The engine would have used
 * zero anyway; the difference is that it can now read it. */
static const char *const k_missing_playerdata[] = {
  "PoisonWaterLosses", "FirstCollectibleSighted", "LastHourCount",
  "SwampyTouched", "DoofIAPState", "SocialHasConnectedToFB",
  "SynergyDuckID", "SynergyDuckStamp", NULL
};

/* Append any of the above that the document lacks. Returns a NEW buffer, or
 * NULL if nothing needed adding (in which case keep the original). */
static char *add_missing_playerdata(const char *in) {
  char key[64];
  int need = 0;
  for (int i = 0; k_missing_playerdata[i]; i++) {
    snprintf(key, sizeof(key), "\"PlayerData__%s\"", k_missing_playerdata[i]);
    if (!strstr(in, key)) need++;
  }
  if (!need) return NULL;

  const size_t in_len = strlen(in);
  char *out = malloc(in_len + (size_t)need * 192 + 8);
  if (!out) return NULL;

  const char *close = strrchr(in, '}');
  if (!close) { free(out); return NULL; }
  const size_t head = (size_t)(close - in);
  memcpy(out, in, head);
  size_t o = head;

  for (int i = 0; k_missing_playerdata[i]; i++) {
    const char *name = k_missing_playerdata[i];
    snprintf(key, sizeof(key), "\"PlayerData__%s\"", name);
    if (strstr(in, key)) continue;
    o += (size_t)snprintf(out + o, 192,
        ",\n   \"PlayerData__%s\" : {\n"
        "      \"EventName\" : \"%s\",\n"
        "      \"EventValue\" : \"0\",\n"
        "      \"EventStringValue\" : \"null\"\n   }",
        name, name);
  }
  out[o++] = '\n';
  out[o++] = '}';
  out[o] = '\0';
  debugPrintf("profile: added %d missing PlayerData record(s)\n", need);
  return out;
}

static void save_path(char *out, size_t n) {
  snprintf(out, n, "%s/" SAVE_NAME, wmw_game_dir());
}

static void profile_store(const char *doc) {
  if (!doc || !*doc) return;
  char path[WMW_PATH_MAX];
  save_path(path, sizeof(path));
  FILE *f = fopen(path, "wb");
  if (!f) { debugPrintf("profile: could not write %s\n", path); return; }
  const size_t len = strlen(doc);
  const size_t got = fwrite(doc, 1, len, f);
  fclose(f);
  if (got != len) debugPrintf("profile: short write to %s\n", path);
}

/* Returns a heap copy of the saved profile, or NULL if there is not one. */
static char *profile_load(void) {
  char path[WMW_PATH_MAX];
  save_path(path, sizeof(path));
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 2 || n > 4 * 1024 * 1024) { fclose(f); return NULL; }
  char *buf = malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  /* Must at least look like the object the engine will be handed. */
  if (got < 2 || buf[0] != '{') { free(buf); return NULL; }
  debugPrintf("profile: loaded %s (%zu bytes)\n", SAVE_NAME, got);
  return buf;
}

void wmw2_profile_note_modify(const char *delta) {
  if (!delta || !*delta) return;
  char *copy = strdup(delta);
  if (!copy) return;
  free(s_pending_delta);
  s_pending_delta = copy;
  s_dirty = 1;
  /* Fold it in and write straight away. The engine sends one of these per
   * meaningful change -- a level completed, a setting toggled -- so this is a
   * handful of writes a minute, not a hot path, and anything less means losing
   * the session if the console is put to sleep or the game is exited from the
   * home menu rather than quit. */
  wmw2_profile_current();
  if (s_current) profile_store(s_current);
}

const char *wmw2_profile_current(void) {
  /* Straight-line, and deliberately so. The previous version had two early
   * returns and both were wrong:
   *
   *   - with no pending delta it did free(s_current); s_current = strdup(base),
   *     which threw away the save that had just been loaded and handed back the
   *     factory profile instead. A save can be read perfectly and still be
   *     discarded one line later.
   *   - both early returns skipped the empty-field normalisation, so the
   *     document the engine actually received was never the normalised one and
   *     it aborted in stoull exactly as before.
   *
   * The order that matters: load once, fall back only if there is nothing to
   * load, apply any pending change, normalise whatever came out. */

  if (!s_loaded) {
    s_loaded = 1;
    s_current = profile_load();          /* NULL if this is a first run */
    if (s_current) s_dirty = 1;          /* loaded text still needs normalising */
  }

  if (!s_current) {
    s_current = strdup(wmw2_factory_profile_json());
    s_dirty = 1;
  }
  if (!s_current) return wmw2_factory_profile_json();   /* out of memory */

  if (s_pending_delta) {
    size_t n = 0;
    char *merged = wmw2_profile_merge(s_current, s_pending_delta, &n);
    if (merged) {
      free(s_current);
      s_current = merged;
      debugPrintf("profile: current document is now %zu bytes\n", n);
    }
    free(s_pending_delta);
    s_pending_delta = NULL;
    s_dirty = 1;
  }

  if (s_dirty) {
    s_dirty = 0;
    char *filled = add_missing_playerdata(s_current);
    if (filled) { free(s_current); s_current = filled; }
    /* Idempotent -- no empty numeric fields survive it, so running it again is
     * free. Note this DOES reach the file too: note_modify() calls this and
     * then stores the result, so a save written after the first change holds
     * "0" where the engine wrote "". That is deliberate and harmless -- the
     * engine reads "0" back happily, which is more than can be said for its own
     * "" -- but it does mean the file is the port's normalised form rather than
     * a verbatim transcript of the deltas. */
    size_t n = 0;
    char *norm = normalise_empties(s_current, &n);
    if (norm) { free(s_current); s_current = norm; }
  }
  return s_current;
}
