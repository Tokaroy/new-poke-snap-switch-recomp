// Native test of the pose-event probe logic (host/mods/evprobe_core.h). Built and run by tests/mods/run_tests.ps1.
#include <cstdio>
#include <cstring>
#include <string>

#include "../../host/mods/evprobe_core.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

int main() {
  using namespace evprobe;

  // Only the two literals the film evaluator searches for switch the probe on.
  CHECK(is_literal("EV_Mot_Sp", 9));
  CHECK(is_literal("EV_Mot_Mis", 10));
  CHECK(!is_literal("EV_Mot_Mis", 9));      // right text, wrong length
  CHECK(!is_literal("EV_Mot_Com", 10));
  CHECK(!is_literal("EV_Mot_Sx", 9));
  CHECK(!is_literal(nullptr, 9));
  CHECK(!is_literal("EV_Mot_Sp", 0));

  // Names stop at the first byte that is not a letter, digit or underscore, at the readable limit, and at 63 bytes.
  CHECK(name_at("EV_Mot_Sp_P097_01", 64) == "EV_Mot_Sp_P097_01");
  CHECK(name_at("EV_Mot_Com_006_Walk_01 next", 64) == "EV_Mot_Com_006_Walk_01");
  CHECK(name_at("EV_Mot_Mis_Stage001_048_01", 6) == "EV_Mot");
  CHECK(name_at(nullptr, 64).empty());
  CHECK(name_at("", 64).empty());
  CHECK(name_at("EV_Mot", 0).empty());
  const std::string longname(100, 'A');
  CHECK(name_at(longname.c_str(), 100).size() == 63);

  // Request and special pose tags are kind 2, other EV_ names kind 1, fragments kind 0.
  CHECK(classify("EV_Mot_Mis_Stage001_097_01") == 2);
  CHECK(classify("EV_Mot_Sp_P016_03") == 2);
  CHECK(classify("EV_Mot_Com_Ex_Sleep_01") == 1);
  CHECK(classify("EV_Mot_Com_009_Eat_01") == 1);
  CHECK(classify("Ex_Sleep_01") == 0);       // the evaluator also tries the 'E' of "Ex"
  CHECK(classify("Eat_01") == 0);
  CHECK(classify("EV_") == 0);
  CHECK(classify("") == 0);

  // The two compares of one record give one line; a new record or a new name gives another.
  Dedup d;
  CHECK(d.fresh(0x1000, "EV_Mot_Com_006_Walk_01"));
  CHECK(!d.fresh(0x1000, "EV_Mot_Com_006_Walk_01"));
  CHECK(d.fresh(0x2000, "EV_Mot_Com_006_Walk_01"));     // next photo, same event
  CHECK(!d.fresh(0x2000, "EV_Mot_Com_006_Walk_01"));
  CHECK(d.fresh(0x2000, "EV_Mot_Sp_P097_01"));          // same slot, new text
  CHECK(d.fresh(0x1000, "EV_Mot_Com_006_Walk_01"));     // back to the first record: evaluated again

  if (g_fail) { printf("evprobe_test: %d FAILED\n", g_fail); return 1; }
  printf("evprobe_test: all passed\n");
  return 0;
}
