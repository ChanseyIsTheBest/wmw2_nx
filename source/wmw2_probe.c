/* wmw2_probe.c -- main-menu loading: instrumentation, and the offline fix
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * WHAT THIS IS FOR
 * ----------------
 * The main menu shows its loading screen, fills the bar, and never hands over.
 * Disassembly established the mechanism but not the cause:
 *
 *   Screen_MainMenu::update() runs ONE task per frame from a vector, and only
 *   advances the index when the task sets param.done. When the index reaches
 *   the end it sets the "finished" flag and calls _goEnter(), which is the
 *   handover. The whole block is gated on that flag being 0.
 *
 *   Exactly two things write the index (this+0x1b0):
 *       Screen_MainMenu::enter()   -> 0        (and BUILDS the task vector)
 *       Screen_MainMenu::update()  -> index+1
 *
 *   enter() resets the index but NOT the step counter at this+0x1b4;
 *   loadPropertyList() is what zeroes that. So a bar that climbs to full and
 *   stays there while one task repeats is precisely "index keeps returning to
 *   zero, steps keep accumulating" -- i.e. enter() running more than once.
 *
 * That is a hypothesis with a clean prediction, and static analysis cannot
 * settle it: enter() has no direct callers, no PLT stub, and is dispatched
 * through a vtable slot, so the caller is not reachable by reading the binary.
 * One boot with counters answers it.
 *
 * HOW IT HOOKS
 * ------------
 * These four are reached through relocated slots, which the port owns:
 *
 *   0x00ff4378  ABS64      vtable  Screen_MainMenu::enter
 *   0x00ff43b0  ABS64      vtable  Screen_MainMenu::update
 *   0x00ff43c0  ABS64      vtable  Screen_MainMenu::loadPropertyList
 *   0x01006360  JUMP_SLOT  GOT     Screen_MainMenu::_goEnter
 *
 * After so_relocate()/so_resolve() each slot holds the real function address.
 * Overwriting it with a trampoline that logs and tail-calls the original gives
 * exact call counts with no guessing. The addresses are link-time vaddrs, so
 * they are offset by the module's load base.
 *
 * WHAT THE OUTPUT MEANS
 * ---------------------
 *   enter() once, _goEnter() once      -> the loop is elsewhere; the load list
 *                                         itself completed
 *   enter() many times                 -> confirmed: something re-enters the
 *                                         screen. The interesting question
 *                                         becomes what, and whether
 *                                         loadPropertyList tracks it
 *   enter() once, _goEnter() never     -> the index never reaches the end, so
 *                                         a task is not setting done, or the
 *                                         vector is growing after enter()
 *
 * The probe also reports the loading state straight out of the object each
 * time update() runs, which distinguishes those cases on its own.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "wmw2_probe.h"
#include "util.h"

/* Link-time addresses of the relocated slots, from
 *     readelf -r libwalaber.so | grep Screen_MainMenu
 * If a future game build moves them, the probe reports that rather than
 * patching something random -- see the sanity check in install(). */
#define SLOT_ENTER              0x00ff4378
#define SLOT_UPDATE             0x00ff43b0
#define SLOT_LOADPROPERTYLIST   0x00ff43c0
#define SLOT_GOENTER            0x01006360

/* GOT entry that Screen_MainMenu::enter() reads when it builds the load-task
 * vector. Patching it before enter() runs puts our own function in the list. */
#define SLOT_TASK_RELOADPROFILE 0x0100e2c0

/* PLT/GOT slot for InterstitialManager::ShouldShowInterstitial. */
#define SLOT_SHOULDSHOWINTERSTITIAL 0x01006040

/* PLT/GOT slot for ScreenSettings::wrapTextInLabel. */
#define SLOT_WRAPTEXTINLABEL        0x010058b8

/* PLT/GOT slot for WalaberGame::copyDatabaseFromBundle(src, dst). */
#define SLOT_COPYDATABASE           0x0100a770

/* PLT/GOT slot for std::__ndk1::stoull(const string&, size_t*, int). */
#define SLOT_STOULL                 0x0100a508

/* PLT/GOT slot for FileHelper::deleteFile(const std::string&). */
#define SLOT_DELETEFILE             0x0100bfc8

/* .text bounds of libwalaber.so, used only to sanity-check what we find in the
 * slots before overwriting anything. */
#define TEXT_LO 0x0021c9b0
#define TEXT_HI 0x00db7140

/* Field offsets within Screen_MainMenu, recovered from update(). */
#define OFF_TASKVEC_BEGIN 0x198
#define OFF_TASKVEC_END   0x1a0
#define OFF_INDEX         0x1b0
#define OFF_STEPS_DONE    0x1b4
#define OFF_STEPS_TOTAL   0x1b8
#define OFF_FINISHED      0x1bc

typedef void (*fn_void_this)(void *thiz);
typedef void (*fn_update)(void *thiz, float dt, int b);
typedef void (*fn_loadprops)(void *thiz, const void *props);

static fn_void_this s_orig_enter;
static fn_update    s_orig_update;
static fn_loadprops s_orig_loadprops;
static fn_void_this s_orig_goenter;

static unsigned s_n_enter, s_n_goenter, s_n_loadprops, s_n_update;

/* Read the loading state out of the screen object. */
static void report_state(const char *tag, void *thiz) {
  const uint8_t *p = thiz;
  int32_t idx    = *(const int32_t *)(p + OFF_INDEX);
  int32_t done   = *(const int32_t *)(p + OFF_STEPS_DONE);
  int32_t total  = *(const int32_t *)(p + OFF_STEPS_TOTAL);
  uint8_t fin    = *(p + OFF_FINISHED);
  uintptr_t b    = *(const uintptr_t *)(p + OFF_TASKVEC_BEGIN);
  uintptr_t e    = *(const uintptr_t *)(p + OFF_TASKVEC_END);
  long tasks     = (b && e >= b) ? (long)((e - b) / 16) : -1;

  debugPrintf("probe: %-14s tasks=%ld index=%d steps=%d/%d finished=%d\n",
              tag, tasks, (int)idx, (int)done, (int)total, (int)fin);
}

static void probe_enter(void *thiz) {
  s_n_enter++;
  /* Loud for the first few, then thinned out -- if this is being called every
   * frame the first ten lines already prove it, and the rest would drown the
   * log the way the profile trace did. */
  if (s_n_enter <= 10 || (s_n_enter % 100) == 0) {
    debugPrintf("probe: Screen_MainMenu::enter() call #%u\n", s_n_enter);
    report_state("enter-before", thiz);
    debugLogFlush();
  }
  s_orig_enter(thiz);
  if (s_n_enter <= 10 || (s_n_enter % 100) == 0) {
    report_state("enter-after", thiz);
    debugLogFlush();
  }
}

static void probe_goenter(void *thiz) {
  s_n_goenter++;
  debugPrintf("probe: Screen_MainMenu::_goEnter() call #%u  <-- the handover\n",
              s_n_goenter);
  debugLogFlush();
  s_orig_goenter(thiz);
}

static void probe_loadprops(void *thiz, const void *props) {
  s_n_loadprops++;
  debugPrintf("probe: Screen_MainMenu::loadPropertyList() call #%u\n", s_n_loadprops);
  debugLogFlush();
  s_orig_loadprops(thiz, props);
  report_state("loadprops-after", thiz);
  debugLogFlush();
}

static void probe_update(void *thiz, float dt, int b) {
  s_n_update++;

  /* Report BEFORE and AFTER, because the previous run produced a genuine
   * contradiction and only this can resolve it.
   *
   * The loop was sitting on index=12 of 13 with steps=35/36 -- one step from
   * finishing -- and task[12] is _reloadProfileToGetLatestFromServer, whose
   * disassembly sets param.done = 1 and param.steps++ unconditionally, with
   * exactly the same instruction sequence as _loadBannerControlJson and the
   * eleven other tasks that DID complete. The trace also shows it being called
   * once per update(), so it is running.
   *
   * So either the state advances inside update() and something puts it back
   * before the next frame, or it never advances at all. Those need completely
   * different fixes, and a before/after pair separates them in one boot:
   *
   *   before 12/35, after 13/36  -> it does advance; something resets it, and
   *                                 the culprit runs between updates
   *   before 12/35, after 12/35  -> the task's tail is never reached, i.e. the
   *                                 call into MIGS does not return normally
   *                                 (an exception unwinding past it would do
   *                                 this and leave the frame rate untouched)
   */
  const int loud = (s_n_update <= 8 || (s_n_update % 60) == 0);
  if (loud) {
    report_state("update-before", thiz);
    debugPrintf("probe: counts  enter=%u goEnter=%u loadProps=%u update=%u\n",
                s_n_enter, s_n_goenter, s_n_loadprops, s_n_update);
  }
  s_orig_update(thiz, dt, b);
  if (loud) {
    report_state("update-after", thiz);
    debugLogFlush();
  }
}

/* ---------------------------------------------------------------------------
 * The fix: the "reload profile from server" load step has no server.
 *
 * The probe settled what was happening. The main menu's loading screen runs a
 * vector of 13 tasks, one per frame, advancing only when a task reports itself
 * done. It sits forever on task[12] at 35 of 36 steps -- which is the full-
 * looking bar -- and task[12] is
 * Screen_MainMenu::_reloadProfileToGetLatestFromServer.
 *
 * That task asks MIGS for the player's server-side profile and then marks
 * itself complete. There is no MIGS service here and there never will be, so
 * the step has nothing to do -- but it also never reports done, and the menu
 * waits on it indefinitely.
 *
 * Rather than keep guessing at what document would satisfy a service that does
 * not exist -- three were tried, all parse, none helped -- this replaces the
 * step with what it means offline: nothing to fetch, step complete.
 *
 * The replacement is deliberately the original's own tail, and nothing else:
 *
 *     ldr  w8, [x19]        ; steps
 *     mov  w9, #1
 *     strb w9, [x19, #4]    ; done = 1
 *     add  w8, w8, #1       ; steps++
 *     str  w8, [x19]
 *
 * so the loop sees exactly what it sees from the twelve tasks that do work,
 * and the progress bar reaches 36/36 honestly. What is skipped is only the
 * MigsMessages call in between.
 *
 * Note this is NOT patching game logic in any lasting sense -- the engine's own
 * code is untouched. It is the port answering a platform question ("what does
 * the server say?") the only way it truthfully can, which is the same thing the
 * fake JNI does everywhere else.
 * ------------------------------------------------------------------------- */

/* The task parameter, from Screen_MainMenu::update():
 *     str  w8, [sp, #0x18]     -> steps   (int,  offset 0)
 *     strb wzr, [sp, #0x1c]    -> done    (bool, offset 4) */
typedef struct { int32_t steps; uint8_t done; } LoadTaskParam;

typedef void (*fn_task)(void *param);
static fn_task  s_orig_task_reload;
static unsigned s_n_skipped;
static int      s_profile_delivered;

static void task_reload_profile_offline(void *param) {
  LoadTaskParam *p = param;
  const int32_t before = p->steps;

  /* Run the real task ONCE.
   *
   * This step is how the engine loads its profile: it calls
   * MigsMessages::PerformMigsImmediate_Profile_GetProfile, which asks the port
   * for the document through jniMigsImmediateProfileGetProfile and feeds the
   * answer to UpdateGameWithMigsProfile. Progress, unlocked levels and audio
   * settings all arrive that way and nowhere else.
   *
   * An earlier revision replaced this task outright with "nothing to do, step
   * complete", because it never reported done and the main menu waited on it
   * forever. That unstuck the menu and quietly removed the only load path in
   * the game -- saves were written correctly and then never read, which looked
   * exactly like saving being broken.
   *
   * Why it used to hang is now explicable: back then the port answered the
   * profile request with "{}", so the engine was handed an empty document every
   * frame and the step had nothing to conclude. It has a real profile to load
   * now.
   *
   * Called once rather than every frame because it is a load step, not a poll,
   * and one delivery is all the engine needs. */
  if (s_orig_task_reload && !s_profile_delivered) {
    s_profile_delivered = 1;
    debugPrintf("probe: delivering the saved profile to the engine\n");
    debugLogFlush();
    s_orig_task_reload(param);
  }

  /* Complete regardless of what the real task decided, so the loading screen
   * cannot stall here again. Idempotent: if it already reported done and
   * counted its step, nothing is added twice. */
  if (!p->done) {
    p->done = 1;
    if (p->steps == before) p->steps = before + 1;
    if (s_n_skipped < 4) {
      s_n_skipped++;
      debugPrintf("probe: reload-profile step forced complete\n");
      debugLogFlush();
    }
  }
}

/* ---------------------------------------------------------------------------
 * Finishing a level crashed: the interstitial-ad manager does not exist.
 *
 *     jniRenderDrawPreDraw -> WaterGame_Android::update -> ScreenManager::update
 *       -> Screen_Game::update -> Screen_Game::_gameWon(float)
 *         -> Screen_Game::ShowInterstitial(bool,int)
 *           -> InterstitialManager::ShouldShowInterstitial(ctx)   x0 = NULL
 *
 * Data Abort at 0x18. Screen_Game::ShowInterstitial fetches the manager from
 * WaterGame+0xf8 and calls straight through it:
 *
 *     bl   WaterGame::getInstance
 *     ldr  x21, [x0, #0xf8]        ; the InterstitialManager -- NULL here
 *     ...
 *     mov  x0, x21
 *     bl   ShouldShowInterstitial
 *
 * and the callee dereferences this+0x18 on its second instruction. It is never
 * constructed because there is no ad stack: InterstitialManager's constructor
 * is one of only two std::stoull call sites in the engine, reading ad state out
 * of the save database that a real ironSource/LevelPlay init would have
 * populated.
 *
 * On Android that pointer is always valid, so the missing null check is not a
 * bug there. Here it is guaranteed NULL for the life of the process, and the
 * honest answer to "should we show an interstitial?" with no ad manager is no.
 * ------------------------------------------------------------------------- */

typedef int (*fn_should_show)(void *thiz, const void *ctx);
static fn_should_show s_orig_should_show;
static unsigned s_n_interstitial;

static int probe_should_show_interstitial(void *thiz, const void *ctx) {
  if (!thiz) {
    if (s_n_interstitial == 0) {
      debugPrintf("probe: ShouldShowInterstitial with no ad manager -> no "
                  "(would have faulted)\n");
      debugLogFlush();
    }
    s_n_interstitial++;
    return 0;
  }
  return s_orig_should_show(thiz, ctx);
}

/* ---------------------------------------------------------------------------
 * A dialogue with an unfonted label takes the process down.
 *
 *     FileManager::readFile -> FH_ZipFileSystem::readFileSucceeded
 *       -> WidgetHelper::_fileReadCallback
 *         -> Screen_Dialogue::_finishedLoadingWidgets
 *           -> ScreenSettings::wrapTextInLabel(label, width)
 *
 *              ldr x20, [x19, #0x178]    ; label->font
 *              ldr s9, [x20, #0x94]      ; x20 = NULL  -> Data Abort at 0x94
 *
 * The dialogue in question is the rate-this-app prompt -- Screen_Dialogue's
 * widget code has RATE_TITLE / RATE_PROMPT / RATE_LATER right beside the two
 * wrapTextInLabel calls. It fires on a play-count threshold
 * (PlayerData__RateMePromptNumToPrompt starts at 10), which is why it appeared
 * only after a level was finished rather than on a fresh save.
 *
 * wrapTextInLabel does no measuring at all without a font, so guarding it costs
 * nothing that was going to happen anyway: the label keeps whatever text it was
 * given, unwrapped. The alternative -- letting a dialogue nobody can act on
 * (there is no store to rate in) kill the process -- is clearly worse.
 * ------------------------------------------------------------------------- */

#define WIDGET_LABEL_FONT_OFFSET 0x178

typedef void (*fn_wrap_text)(void *label, float width);
static fn_wrap_text s_orig_wrap_text;
static unsigned s_n_unfonted;

static void probe_wrap_text_in_label(void *label, float width) {
  if (!label) return;
  const void *font = *(void *const *)((const uint8_t *)label + WIDGET_LABEL_FONT_OFFSET);
  if (!font) {
    if (s_n_unfonted == 0) {
      debugPrintf("probe: wrapTextInLabel on a label with no font -> skipped "
                  "(would have faulted)\n");
      debugLogFlush();
    }
    s_n_unfonted++;
    return;
  }
  s_orig_wrap_text(label, width);
}

/* ---------------------------------------------------------------------------
 * Progress was not being saved: WalaberGame::updateDatabase() replaces
 * levelinfo.db from the bundle on launch, and that is where the game keeps its
 * level records.
 *
 * The traffic during play settles it -- 5631 positional writes after the main
 * loop starts, essentially all of them to the live database and its journal:
 *
 *     fd=4  levelinfo.db           1221 writes
 *     fd=7  levelinfo.db-journal   4401 writes
 *
 * and at the next launch, before any of it can be read back:
 *
 *     probe: engine is replacing database .../levelinfo.db
 *     probe: copyDatabaseFromBundle -> .../levelinfo.db
 *
 * So the writes were never the problem. The file is simply overwritten with the
 * shipped copy before the game reads it.
 *
 * These guards were removed once, on a reading of an earlier log that took the
 * writes to checked_tmp.db for a data migration -- fresh schema, player data
 * copied across, then installed -- which would have made blocking the install
 * exactly wrong. It is not a migration; checked_tmp.db is only ever the staging
 * copy updateDatabase() reads the shipped DatabaseVersion out of. Removing them
 * put the wipe straight back.
 *
 * A copy is still allowed when:
 *   - the destination is checked_tmp.db, which updateDatabase() needs in order
 *     to read the bundled version at all; or
 *   - the destination does not exist, i.e. genuine first-run seeding; or
 *   - the destination is not valid SQLite, so a truncated database repairs.
 *
 * Anything else is the player's save.
 *
 * Worth separating from this: the engine used to be unable to open perry.db a
 * second time at all (section 6.1, fixed with alias descriptors in
 * libc_shim.c). That was a real and different bug, and fixing it is why writes
 * now reach the databases instead of dying in a rolled-back transaction.
 * ------------------------------------------------------------------------- */

typedef int (*fn_copy_db)(void *thiz, const void *src_str, const void *dst_str);
static fn_copy_db s_orig_copy_db;
static unsigned s_n_db_kept;

typedef int (*fn_delete_file)(const void *path_str);
static fn_delete_file s_orig_delete_file;

/* libc++ std::string: short form keeps the text inline from byte 1 and the low
 * bit of byte 0 clear; long form holds a heap pointer at +0x10. */
static const char *cxx_string_cstr(const void *sv) {
  if (!sv) return NULL;
  const uint8_t *p = sv;
  if (p[0] & 1) return *(const char *const *)(p + 0x10);
  return (const char *)(p + 1);
}

/* True when `dst` is live player state that must not be overwritten.
 *
 * Two kinds qualify, and the second was missed for several rounds:
 *
 *   - a valid SQLite database: levelinfo.db holds level progress, perry.db
 *     holds the Settings table (Age lands there at runtime);
 *
 *   - anything the engine keeps under migs/. factory_profile.json is not just
 *     a template -- the engine reads its own audio settings back out of it:
 *
 *         "Settings__MusicOn": { "Name": "MusicOn", "Value": "1" },
 *         "Settings__AudioOn": { "Name": "AudioOn", "Value": "1" },
 *
 *     so restoring it from the bundle each launch put music and sound back to
 *     on every time. That is exactly why age survived and audio settings did
 *     not: Age has no entry in the factory document, so nothing reset it.
 *
 * In both cases "already exists and is not empty" is the test. First-run
 * creation still happens because the file genuinely is not there yet. */
static int dest_is_live_database(const char *dst) {
  if (!dst) return 0;
  const char *base = strrchr(dst, '/');
  base = base ? base + 1 : dst;
  if (strcmp(base, "checked_tmp.db") == 0) return 0;   /* staging: always allow */

  const int under_migs = (strstr(dst, "/migs/") != NULL);

  FILE *f = fopen(dst, "rb");
  if (!f) return 0;                                    /* absent: first-run seed */
  char magic[16] = {0};
  const size_t got = fread(magic, 1, sizeof(magic), f);
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fclose(f);

  if (under_migs) return size > 0;                     /* the player's settings */
  return (got == sizeof(magic) && memcmp(magic, "SQLite format 3", 15) == 0);
}

/* FileHelper::deleteFile(const std::string&) -- static, so the string is x0.
 *
 * updateDatabase() removes the live database BEFORE asking for a fresh copy:
 *
 *     bl   fileExists(x23)
 *     tbz  w0, #0, <skip>
 *     mov  x0, x23
 *     bl   FileHelper::deleteFile        ; the save is gone here
 *     ...
 *     mov  x2, x23                       ; destination
 *     bl   copyDatabaseFromBundle
 *
 * which is why guarding only the copy did nothing: by the time it ran the file
 * had already been unlinked, so "does the destination exist?" was correctly
 * answering no. Both halves have to be refused, and the same predicate serves:
 * a valid SQLite file that is not the staging copy is the player's save. */
static unsigned s_n_delete_kept;

static int probe_delete_file(const void *path_str) {
  const char *p = cxx_string_cstr(path_str);
  if (dest_is_live_database(p)) {
    if (s_n_delete_kept < 4) {
      debugPrintf("probe: refusing to delete live database %s\n", p ? p : "?");
      debugLogFlush();
    }
    s_n_delete_kept++;
    return 1;                       /* report success; nothing was removed */
  }
  return s_orig_delete_file(path_str);
}

static int probe_copy_database(void *thiz, const void *src_str, const void *dst_str) {
  const char *dst = cxx_string_cstr(dst_str);
  if (dest_is_live_database(dst)) {
    if (s_n_db_kept < 4) {
      debugPrintf("probe: keeping existing database %s (not restoring from bundle)\n",
                  dst ? dst : "?");
      debugLogFlush();
    }
    s_n_db_kept++;
    return 1;                                          /* report success */
  }
  return s_orig_copy_db(thiz, src_str, dst_str);
}

/* ---------------------------------------------------------------------------
 * std::stoull, wrapped.
 *
 * Four separate fixes have gone in for "terminating with uncaught exception of
 * type std::invalid_argument: stoull: no conversion" -- a UUID-shaped user
 * identifier, empty numeric fields, missing PlayerData records, and the profile
 * being served at all -- and each was a real defect that changed the log
 * without ending the abort. Guessing which value is next is not working.
 *
 * There are exactly two call sites in the engine, Water::_getUserIdentifier and
 * InterstitialManager's constructor, and both reach stoull through this one
 * relocated slot. Wrapping it does two things:
 *
 *   - says what the offending string actually is, once, instead of inferring it
 *     from which document was in play;
 *   - converts what it can and returns 0 otherwise, rather than throwing.
 *
 * The second half is not a workaround dressed up as a fix. std::stoull throws
 * only when it can convert NOTHING at position zero; every caller here is
 * reading a counter or a timestamp out of the player profile, where an
 * unreadable value means "not set" and zero is what the engine would have used.
 * The alternative behaviour -- terminate the process -- is never what anyone
 * wants from a save file that is merely incomplete.
 *
 * Note this is a genuine C++ ABI boundary: the argument is a const std::string&
 * and the return is unsigned long long, so the wrapper has to read libc++'s
 * string layout directly. cxx_string_cstr() already does that for the database
 * hooks.
 * ------------------------------------------------------------------------- */

typedef unsigned long long (*fn_stoull)(const void *str, size_t *idx, int base);
static fn_stoull s_orig_stoull;
static unsigned  s_n_stoull_rescued;

static unsigned long long probe_stoull(const void *str, size_t *idx, int base) {
  const char *p = cxx_string_cstr(str);

  /* strtoull rather than the original stoull: same conversion, same result, but
   * it reports failure by return value instead of by throwing, which is the
   * whole point. s_orig_stoull is kept only so the hook can be verified as
   * having captured something real. */
  (void)s_orig_stoull;
  if (p) {
    char *end = NULL;
    const unsigned long long v = strtoull(p, &end, base ? base : 10);
    if (end != p) {
      if (idx) *idx = (size_t)(end - p);
      return v;                       /* convertible: same answer, no throw */
    }
  }

  if (s_n_stoull_rescued < 8) {
    s_n_stoull_rescued++;
    debugPrintf("probe: stoull(\"%s\", base %d) has nothing to convert -> 0\n",
                p ? p : "(null)", base);
    debugLogFlush();
  }
  if (idx) *idx = 0;
  return 0;
}

/* ------------------------------------------------------------------------- */

static int patch(so_module *mod, uintptr_t slot_vaddr, void *replacement,
                 void **out_orig, const char *what) {
  /* Write through load_base, NOT load_virtbase.
   *
   * so_relocate() and so_resolve() both edit the module through load_base --
   * load_virtbase is only a reservation until so_finalize() calls
   * svcMapProcessCodeMemory, and touching it before that faults. (Afterwards
   * the relationship inverts: the mapping revokes access to the load_base
   * range, so this must happen before finalize either way.)
   *
   * The VALUE in the slot is already rebased, though: relocation adds
   * load_virtbase to it, so the sanity check below compares against that. */
  uintptr_t *slot = (uintptr_t *)((uintptr_t)mod->load_base + slot_vaddr);
  const uintptr_t cur = *slot;
  const uintptr_t base = (uintptr_t)mod->load_virtbase;

  /* The slot must currently hold something inside libwalaber's .text. If it
   * does not, this is a different game build and the addresses have moved --
   * say so and change nothing rather than corrupting a live pointer. */
  if (cur < base + TEXT_LO || cur >= base + TEXT_HI) {
    debugPrintf("probe: slot for %s holds 0x%lx, outside .text -- NOT patching\n",
                what, (unsigned long)cur);
    return 0;
  }
  *out_orig = (void *)cur;
  *slot = (uintptr_t)replacement;
  debugPrintf("probe: hooked %-38s (was +0x%lx)\n", what,
              (unsigned long)(cur - base));
  return 1;
}

void wmw2_probe_install(so_module *mod) {
  int n = 0;
  /* Do this one FIRST: enter() reads this slot to build the task vector, so it
   * has to hold the replacement before the vector is built. The original is
   * kept -- the replacement calls it once to load the profile. */
  patch(mod, SLOT_TASK_RELOADPROFILE, (void *)task_reload_profile_offline,
        (void **)&s_orig_task_reload,
        "Screen_MainMenu::_reloadProfileToGetLatestFromServer [task]");

  n += patch(mod, SLOT_ENTER,            (void *)probe_enter,
             (void **)&s_orig_enter,     "Screen_MainMenu::enter");
  n += patch(mod, SLOT_GOENTER,          (void *)probe_goenter,
             (void **)&s_orig_goenter,   "Screen_MainMenu::_goEnter");
  n += patch(mod, SLOT_LOADPROPERTYLIST, (void *)probe_loadprops,
             (void **)&s_orig_loadprops, "Screen_MainMenu::loadPropertyList");
  n += patch(mod, SLOT_UPDATE,           (void *)probe_update,
             (void **)&s_orig_update,    "Screen_MainMenu::update");
  n += patch(mod, SLOT_STOULL, (void *)probe_stoull,
             (void **)&s_orig_stoull, "std::stoull(string)");
  n += patch(mod, SLOT_DELETEFILE, (void *)probe_delete_file,
             (void **)&s_orig_delete_file, "FileHelper::deleteFile");
  n += patch(mod, SLOT_COPYDATABASE, (void *)probe_copy_database,
             (void **)&s_orig_copy_db, "WalaberGame::copyDatabaseFromBundle");
  n += patch(mod, SLOT_WRAPTEXTINLABEL, (void *)probe_wrap_text_in_label,
             (void **)&s_orig_wrap_text, "ScreenSettings::wrapTextInLabel");
  n += patch(mod, SLOT_SHOULDSHOWINTERSTITIAL,
             (void *)probe_should_show_interstitial,
             (void **)&s_orig_should_show,
             "InterstitialManager::ShouldShowInterstitial");
  debugPrintf("probe: %d/9 hooks installed\n", n);
  debugLogFlush();
}
