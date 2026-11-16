// SPDX-License-Identifier: MIT
// Whole-save backups, kept beside the live save and never inside it:
//   <save_root>\..\backup\<YYYYMMDD-HHMMSS>-<reason>\<uid>\gens\<n>, CURRENT   (the layout copy_consistent writes: one consistent
//   generation per user, so a backup is itself a valid save root)
// <save_root> is <profiles>\<id>\save for a profile (so the backups sit in <profiles>\<id>\backup), else <save_root>-backup.
// Reasons: "launch" (every start, before the game mounts the save; skipped when the newest backup already holds the same
// generations), "import" and "restore" (before a save import or a restore replaces the current save).
// Retention: the newest `keep` launch backups, and separately the newest `keep` import/restore backups (so ten launches never push out
// the copy taken before an import). keep = hle.json "save_backup_keep" / env SNAP_SAVE_BACKUP_KEEP, default 10; 0 turns it all off.
// Nothing here deletes or rewrites the live save: restore goes through saveport::import_save, which writes a new generation and
// keeps the one that was current.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "save_port.h"

namespace hle {
namespace savebackup {

constexpr int kDefaultKeep = 10;

struct Entry {
  std::string folder;   // directory name under backup_root (also what restore() takes)
  std::string reason;   // launch | import | restore
  int64_t when = 0;     // unix seconds (from the folder name)
  int64_t bytes = 0;
};

int keep_count();                                          // env SNAP_SAVE_BACKUP_KEEP, else config().save_backup_keep, else 10
std::string backup_root(const std::string& save_root);     // where the backups of that save live
// Copies the current save. `made` gets the new folder name, or stays empty when there was nothing to copy or the newest backup is
// identical. False (err set) only when a copy was needed and failed; callers log it and carry on, a failed backup never blocks the game.
bool make(const std::string& save_root, const char* reason, std::string* made, std::string* err, int keep = -1);
std::vector<Entry> list(const std::string& save_root);     // newest first
// Puts a backup back as the current save (a new generation; the one that was current stays as a backup generation). Takes a
// "restore" backup of the current save first. Refused while a game holds the save folder.
bool restore(const std::string& save_root, const std::string& folder, saveport::Report* rep, std::string* err, bool* refused_running = nullptr);
std::string describe(const Entry& e);                      // "2026-11-15 23:10 (at start)"

// Called once at start, after the save profile is chosen and before the save mounts. Logs one line. Does nothing for a test run
// that sets SNAP_SAVE_DIR unless SNAP_SAVE_BACKUP=1.
void backup_at_launch();

}  // namespace savebackup
}  // namespace hle
