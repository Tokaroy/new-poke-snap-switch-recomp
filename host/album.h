// SPDX-License-Identifier: MIT
// PC photo album: reads the game's album out of a save folder, read-only, and exports it as PNG files with a JSON and CSV sidecar.
//
// What the save holds (analysis/save/album.md): the 1200 album slots (file 6 = per-photo record, 7 = full JPEG, 10 = thumbnail) and the
// Photodex (file 2 = one best-shot record per Pokemon, 3 = its JPEG, file 9 = research entries per course). An album slot does NOT store its
// Pokemon, course or score. They are recovered by matching the picture against the Photodex best shots (a 32x20 luma signature of the decoded
// JPEG), and the course and star rating from the research entry that holds the same Pokemon and score. Photos that are not a best shot show
// as "Not rated". The game's clock is a millisecond tick since boot, not a date, so a date is only known for photos that have a PNG export.
//
// The save is only ever opened for reading. The 128-bit save key is not shipped: it is found in the player's own converted main ELF.
// No game text is shipped either: Pokemon and course names are read from the player's extracted text files at run time.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace album {

struct Photo {
  int slot = 0;                   // album slot, 0-based (the game shows slot + 1)
  unsigned long long tick = 0;    // the game's clock in ms when the photo was saved; orders photos, is not a date
  int species = 0;                // Photodex number, 0 = unknown
  int course = 0;                 // course (research route) 1..30, 0 = unknown
  int stars = 0;                  // 1..4, 0 = unknown
  int score = -1;                 // total score, -1 = unknown
  std::string caption;            // the game's comment on the photo, UTF-8, may be empty
  std::string hd_path;            // <save root>\photos_hd\<crc>.jpg, empty = none
  std::string png_path;           // a PNG export of this photo, empty = none
  std::string taken;              // "YYYY-MM-DD HH:MM" from the PNG export's name, empty = unknown
  unsigned jpeg_size = 0, jpeg_crc = 0;
  bool rated = false;             // matched to a Photodex best shot
  bool probed = false;            // the background matching reached this photo
};

struct Library {
  std::vector<Photo> photos;      // newest first
  std::string error;              // empty when the album could be read
  std::string save_dir;           // the generation folder that was read
  int generation = 0;
  int total = 0, done = 0;        // matching progress (steps)
  int species_known = 0;
  struct Impl;
  std::shared_ptr<Impl> impl;
};

struct Inputs {
  std::string save_root;          // the profile's save folder (the one that holds <uid>\CURRENT and photos_hd); also accepts the <uid> folder
  std::string main_elf;           // the converted main ELF (holds the save key)
  std::string romfs_root;         // for the Pokemon and course names (optional)
  std::string png_dir;            // the PNG exports (%APPDATA%\SnapRecomp\photos), optional
};

// Reads the slot list (quick: metadata only). Photos are listed straight away; step() fills in the matches.
bool load(const Inputs& in, Library& out);
// Does up to budget_ms of matching work. Returns true when everything is matched.
bool step(Library& lib, double budget_ms);

const std::string& species_name(const Library& lib, int species);   // "Pokemon 007" when the text file is missing
std::string course_name(const Library& lib, int course);            // "Park (Day)"
std::string island_name(const Library& lib, int course);            // "Florio"
std::string course_label(const std::string& romfs_root, int course);   // "Florio - Park (Day)" (research course 1..30) read from the player's own text; "" when missing (Discord presence)

// The album JPEG of a slot (thumb = the 204x136 thumbnail). false when the slot cannot be read.
bool read_jpeg(const Library& lib, int slot, bool thumb, std::vector<uint8_t>& out);
// The picture to show or export at best quality: PNG export, else HD copy, else the album JPEG. Returns the encoded bytes and "png"/"jpg".
bool best_image(const Library& lib, const Photo& p, std::vector<uint8_t>& out, const char** kind);

struct Bitmap { int w = 0, h = 0; std::vector<uint8_t> rgba; };
bool decode_image(const uint8_t* data, size_t n, Bitmap& out);   // JPEG or PNG -> RGBA

// Export: a PNG per photo plus album.json and album.csv in the chosen folder. Never writes to the save. Run it in slices so a window can keep
// drawing: export_step() returns true when the last photo is done and the sidecar files are written.
struct ExportJob {
  std::string dest, error, json, csv;
  std::vector<int> slots;
  size_t next = 0;
  int exported = 0, failed = 0;
  bool started = false;
};
bool export_begin(ExportJob& job, const std::string& dest, const std::vector<int>& slots);
bool export_step(const Library& lib, ExportJob& job, double budget_ms);
// Runs a whole job in one call.
ExportJob export_photos(const Library& lib, const std::vector<int>& slots, const std::string& dest);

}  // namespace album
