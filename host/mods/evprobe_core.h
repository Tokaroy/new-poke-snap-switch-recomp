// Pure logic of the pose-event / request tag probe (host/mods/evprobe.cpp), kept apart so tests/mods/evprobe_test.cpp can run it
// without the game. No Windows or loader dependency.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace evprobe {

// The two substrings the game's film evaluator looks for in a photo's motion-event name.
inline bool is_literal(const void* b, size_t n) {
  if (!b) return false;
  if (n == 9) return memcmp(b, "EV_Mot_Sp", 9) == 0;
  if (n == 10) return memcmp(b, "EV_Mot_Mis", 10) == 0;
  return false;
}

// The name that starts at s: letters, digits and underscores, at most 63 of them and never past `avail` readable bytes.
inline std::string name_at(const char* s, size_t avail) {
  std::string t;
  if (!s) return t;
  for (size_t i = 0; i < avail && i < 63; i++) {
    const unsigned char ch = (unsigned char)s[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_')) break;
    t += (char)ch;
  }
  return t;
}

// 0 = not an event name (the evaluator tries every 'E' in the text, so most candidates are fragments), 1 = a plain event
// (EV_Mot_Com_006_Walk_01), 2 = a request or special pose tag (EV_Mot_Mis_..., EV_Mot_Sp_...).
inline int classify(const std::string& name) {
  if (name.size() < 4 || name.compare(0, 3, "EV_") != 0) return 0;
  if (name.compare(0, 10, "EV_Mot_Mis") == 0 || name.compare(0, 9, "EV_Mot_Sp") == 0) return 2;
  return 1;
}

// The evaluator compares the same text against both literals, so every record shows up twice in a row. A record is new when its
// text pointer or its name differs from the one before.
struct Dedup {
  uintptr_t last_p = 0;
  std::string last;
  bool fresh(uintptr_t p, const std::string& name) {
    if (p == last_p && name == last) return false;
    last_p = p;
    last = name;
    return true;
  }
};

}  // namespace evprobe
