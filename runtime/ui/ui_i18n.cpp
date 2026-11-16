// SPDX-License-Identifier: MIT
#include "ui_i18n.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "hle/hle_config.h"
#include "hle/settings.h"

namespace snap::ui {

namespace {

std::atomic<int> g_forced{-1};
std::atomic<bool> g_collect{false};
std::mutex g_mu;
std::set<std::string> g_seen;   // under g_mu

// game language index (nn::settings::Language) -> table column
int column_of_game_lang(int gl) {
  switch (gl) {
    case 0: return kLangJa;
    case 2: return kLangFr;
    case 3: return kLangDe;
    case 4: return kLangIt;
    case 5: return kLangEs;
    case 6: return kLangZhHans;
    case 7: return kLangKo;
    case 11: return kLangZhHant;
    default: return kLangEn;
  }
}

int column_of_tag(const std::string& tag) {
  for (int i = 0; i < kLangCount; ++i) if (tag == kI18nTags[i]) return i;
  const int gl = hle::game_language_from_tag(tag);
  return gl < 0 ? -1 : column_of_game_lang(gl);
}

const std::unordered_map<std::string_view, const I18nRow*>& index() {
  static const auto m = [] {
    std::unordered_map<std::string_view, const I18nRow*> r;
    r.reserve(g_i18n_count * 2);
    for (unsigned i = 0; i < g_i18n_count; ++i) r.emplace(g_i18n_rows[i].t[0], &g_i18n_rows[i]);
    return r;
  }();
  return m;
}

const char* lookup(const char* en, int lang) {
  if (!en || !*en) return en;
  if (g_collect.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_seen.insert(en);
  }
  if (lang == kLangEn) return en;
  const auto& m = index();
  const auto it = m.find(std::string_view(en));
  if (it == m.end()) return en;
  const char* s = it->second->t[lang];
  return (s && *s) ? s : en;
}

}  // namespace

int i18n_lang() {
  const int f = g_forced.load(std::memory_order_relaxed);
  if (f >= 0) return f;
  // the setting as chosen in F1 or the launcher (explicit language only); Auto keeps what the game started with
  const std::string v = hle::settings::get("game_language", "");
  if (!v.empty() && v != "auto") {
    const int c = column_of_tag(v);
    if (c >= 0) return c;
  }
  static const int boot = column_of_game_lang(hle::resolve_game_language());
  return boot;
}

bool i18n_force(const std::string& tag) {
  if (tag.empty()) { g_forced = -1; return true; }
  const int c = column_of_tag(tag);
  if (c < 0) return false;
  g_forced = c;
  return true;
}

bool i18n_is_cjk(int lang) { return lang == kLangJa || lang == kLangKo || lang == kLangZhHans || lang == kLangZhHant; }

const char* tr(const char* en) { return lookup(en, i18n_lang()); }

namespace {

// Text the menu builds at run time around a size or an upscaler name ("Automatic (3200 x 1800)", "DLSS draws 1280 x 720, shown at 1920 x 1080") is
// looked up with those parts as %s: the table holds "Automatic (%s)" and the parts are put back in order.
bool dyn_split(const std::string& s, std::string* pat, std::vector<std::string>* args) {
  static const char* const names[] = {"DLAA", "DLSS", "FSR", "XeSS"};
  const auto digit = [&](size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
  std::string out;
  for (size_t i = 0; i < s.size();) {
    size_t j = i;
    while (digit(j)) ++j;
    if (j > i && j + 3 < s.size() && s[j] == ' ' && s[j + 1] == 'x' && s[j + 2] == ' ' && digit(j + 3)) {
      size_t k = j + 3;
      while (digit(k)) ++k;
      args->push_back(s.substr(i, k - i));
      out += "%s";
      i = k;
      continue;
    }
    bool hit = false;
    for (const char* n : names) {
      const size_t len = std::strlen(n);
      if (s.compare(i, len, n) == 0 && (i == 0 || !std::isalnum((unsigned char)s[i - 1])) && (i + len >= s.size() || !std::isalnum((unsigned char)s[i + len]))) {
        args->push_back(n);
        out += "%s";
        i += len;
        hit = true;
        break;
      }
    }
    if (hit) continue;
    if (j > i) { out.append(s, i, j - i); i = j; continue; }
    out += s[i++];
  }
  if (args->empty()) return false;
  *pat = out;
  return true;
}

const char* keep(std::string s) {   // a small ring: the caller draws the text right away
  static thread_local std::string ring[64];
  static thread_local unsigned next = 0;
  std::string& slot = ring[next++ % 64];
  slot = std::move(s);
  return slot.c_str();
}

}  // namespace

const char* tr(const std::string& en) {
  const int lang = i18n_lang();
  std::string pat;
  std::vector<std::string> args;
  if (en.size() < 240 && !index().count(std::string_view(en)) && dyn_split(en, &pat, &args)) {
    const char* t = lookup(pat.c_str(), lang);
    if (t == pat.c_str()) return en.c_str();   // no line for the pattern: show the English as is
    std::string out;
    size_t a = 0;
    for (const char* c = t; *c; ++c) {
      if (c[0] == '%' && c[1] == 's' && a < args.size()) { out += args[a++]; ++c; }
      else out += *c;
    }
    return keep(std::move(out));
  }
  return lookup(en.c_str(), lang);
}

void i18n_collect(bool on) { g_collect = on; }
void i18n_collect_reset() { std::lock_guard<std::mutex> lk(g_mu); g_seen.clear(); }

size_t i18n_check(std::string* report) {
  size_t bad = 0;
  auto add = [&](const std::string& s) { ++bad; if (report) { *report += s; *report += '\n'; } };
  std::set<std::string> seen;
  { std::lock_guard<std::mutex> lk(g_mu); seen = g_seen; }
  const auto& m = index();
  // text with no words to translate (key caps "A", "F1", "ZL", sizes "960 x 540", "%s (4K)") stays as it is
  const auto neutral = [](const std::string& k) {
    int letters = 0, lower = 0;
    for (size_t i = 0; i < k.size(); ++i) {
      if (k[i] == '%' && i + 1 < k.size()) { ++i; continue; }   // a placeholder
      if (std::isalpha((unsigned char)k[i])) { ++letters; if (std::islower((unsigned char)k[i])) ++lower; }
    }
    return letters <= 1 || lower == 0 || k.rfind("Num ", 0) == 0;   // one letter, or capitals only (ZL, SMAA, HDR)
  };
  for (const std::string& k : seen) if (!m.count(std::string_view(k)) && !neutral(k)) add("missing key: " + k);
  for (unsigned i = 0; i < g_i18n_count; ++i)
    for (int l = 1; l < kLangCount; ++l)
      if (!g_i18n_rows[i].t[l] || !*g_i18n_rows[i].t[l]) add(std::string("no ") + kI18nTags[l] + " text: " + g_i18n_rows[i].t[0]);
  return bad;
}

}  // namespace snap::ui
