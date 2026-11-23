// SPDX-License-Identifier: MIT
// Profile backups to a folder the player picks (a OneDrive, Dropbox or other sync folder): docs/sync-backups.md.
//   One zip per backup, <folder>\SnapRecomp-<profile id>-<YYYYMMDD-HHMMSS>.zip, stored (no compression: photos and saves are already dense), ZIP64
//   when a file or the archive passes 4 GB. Entries: save/<uid>/CURRENT, save/<uid>/gens/<n>.ok and save/<uid>/gens/<n>/<files> (the one consistent
//   generation per user), settings/profile/settings.cfg, settings/global/<window|audio|input>.json, photos/<label>/<relative path>, and manifest.json
//   written LAST (file list, sizes, SHA-256, save generations, what was included).
//   Written to <name>.zip.part in the target folder, re-read against its manifest, and only then renamed to <name>.zip, so a sync client never
//   sees a half file and a finished zip is always one that verified. The oldest copies are deleted only after that, down to the "copies to keep".
//   A save that is in use, mid-write or flagged for recovery is never backed up: the run reports a warning instead.
//   Restore puts a backup into a NEW profile by default. Replacing the current profile needs an explicit confirmation flag from the caller
//   and takes a local safety copy (save_backup.h) first.
// Settings: <config dir>\backup.json (the launcher's Backups page writes it); env SNAP_SYNC_BACKUP, SNAP_SYNC_BACKUP_FOLDER, SNAP_SYNC_BACKUP_KEEP and
// SNAP_SYNC_BACKUP_INCLUDE (comma list of saves, settings, photos) win for one run (host/cli_flags.h --backup, --backup-folder, --backup-keep,
// --backup-include). HD packs and shader caches are never included: they can be rebuilt.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "save_port.h"

namespace hle {
namespace syncbackup {

constexpr int kDefaultKeep = 3;
constexpr int kMaxKeep = 100;

struct Settings {
  bool enabled = false;
  std::string folder;           // where the zips go; empty = not chosen yet
  int keep = kDefaultKeep;      // copies kept per profile, 1..kMaxKeep
  bool saves = true, settings = true, photos = true;
};

std::string config_dir();                                             // SNAP_CONFIG_DIR, else %APPDATA%\SnapRecomp, else the data root
std::string settings_path(const std::string& dir);                    // <dir>\backup.json
Settings read_settings(const std::string& dir, bool with_env = true); // the file, then the env overrides; a bad or missing file gives the defaults
bool write_settings(const std::string& dir, const Settings& s, std::string* err);   // atomic; never writes the env overrides
std::string include_text(const Settings& s);                          // "saves, settings, photos"

// What a backup is made from. Everything is resolved by the caller (resolve_source for the real app, a temp folder in tests).
struct Source {
  std::string profile_id = "default", profile_name = "Default";
  std::string profiles_root;                                          // the profile store (empty: profiles are off, so no new profile can be made)
  std::string save_root;                                              // <profiles>\<id>\save
  std::string profile_cfg;                                            // the profile's settings.cfg
  std::vector<std::pair<std::string, std::string>> globals;           // zip name under settings/global, file path
  std::vector<std::pair<std::string, std::string>> photo_dirs;        // label (photos, screenshots), folder
};
// The profile the next game start uses (or `profile`), with the real folders (hle config, profile store, %APPDATA%). False with err when none can be chosen.
bool resolve_source(const std::string& profile, Source* out, std::string* err);

enum class Health { Ok, NoSave, InUse, MidWrite, NeedsRecovery };
struct SaveState {
  Health health = Health::NoSave;
  std::string detail;                                                 // a sentence for the player when health is not Ok / NoSave
  std::vector<std::pair<std::string, uint64_t>> gens;                 // uid folder -> committed generation
  std::string signature() const;                                      // "<uid>:<gen>;..." what "the save changed" compares
};
// Looks at a save root without writing to it. In use = a running game holds the lock; mid-write = a generation without its .ok, a replaced CURRENT.tmp,
// or a queued restore; needs recovery = CURRENT missing or torn (the next start repairs it), an orphaned generation.
SaveState inspect_save(const std::string& save_root);

enum class Outcome { Made, Unchanged, Skipped, Failed };   // Skipped: a warning (save in use, mid-write or flagged for recovery)
struct RunResult {
  Outcome outcome = Outcome::Failed;
  std::string zip, message;                                           // the new zip (Made); a sentence for the player
  uint64_t bytes = 0;
  int files = 0, removed = 0;
  std::vector<std::string> notes;                                     // files that could not be read and were left out, and similar
};
// One backup run. `force`: make one even when the save did not change since the newest copy. Writes <config dir>\backup_last.txt (see last_result).
RunResult run_backup(const Source& src, const Settings& set, bool force);

struct Backup {
  std::string file, path;                                             // file name, full path
  std::string profile_id, profile_name;
  int64_t when = 0;                                                   // unix seconds (from the file name)
  uint64_t bytes = 0;                                                 // the zip's size
  bool valid = false;                                                 // the zip lists and its manifest reads (it is not re-hashed here; verify() does that)
  std::string problem;                                                // when not valid
  std::string summary;                                                // "save: 15 files, 4.3 MB; settings; photos: 120 files, 380 MB"
  std::string signature;
  bool has_saves = false, has_settings = false, has_photos = false;
};
// The backups in `folder` (only files this feature wrote), newest first. `profile_id` empty: every profile. A leftover .part is never listed.
std::vector<Backup> list_backups(const std::string& folder, const std::string& profile_id = "");
// Re-reads the zip and checks every file against the manifest (size, SHA-256, the zip's own CRC-32) and that nothing is missing or extra.
bool verify_zip(const std::string& zip, std::string* err);
std::string describe(const Backup& b);                                // "2026-11-21 14:05  4.1 MB  (profile Ash)"
void remove_leftovers(const std::string& folder);                     // deletes this feature's *.part files (what an interrupted write leaves)

struct RestoreOptions {
  bool settings = true;       // the profile's settings.cfg
  bool photos = true;         // photo files that are missing (an existing file is never overwritten)
};
struct RestoreReport {
  std::string profile_id, profile_name;                               // the profile the backup went into
  saveport::Report save;
  int photos_restored = 0, photos_kept = 0;
  bool settings_restored = false;
  std::vector<std::string> notes;
};
// Puts a backup into a NEW profile named "<name> (backup YYYY-MM-DD)" (made unique). Verifies the zip first; on any failure the new profile is removed again.
bool restore_new_profile(const std::string& zip, const Source& cur, const RestoreOptions& opt, RestoreReport* rep, std::string* err);
// Replaces the current profile's save with the backup (a new generation; the old one stays as an older generation and as a local "restore" backup).
// Refuses unless `confirmed` is true, and while a game holds the save.
bool restore_replace_current(const std::string& zip, const Source& cur, const RestoreOptions& opt, bool confirmed, RestoreReport* rep, std::string* err,
                             bool* refused_running = nullptr);

// <config dir>\backup_last.txt: "status\nunix time\nmessage". status: made | unchanged | warning | failed.
struct Last { std::string status, message; int64_t when = 0; };
Last last_result(const std::string& dir);
void write_last(const std::string& dir, const RunResult& r);          // what the command line and the after-game run record for the launcher to show

// Pieces the tests use on their own.
std::string sha256_hex(const void* data, size_t n);
void set_corrupt_part_for_test(bool on);                              // flips one byte of the new zip before it is verified
void set_zip64_threshold_for_test(uint64_t t);                        // 0xFFFFFFFF normally; a small value makes every entry take the ZIP64 paths

}  // namespace syncbackup
}  // namespace hle
