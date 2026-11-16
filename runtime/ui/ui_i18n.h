// SPDX-License-Identifier: MIT
// F1 menu text in the game's languages (docs/player-options.md "Game language"). The English text of a string is its key:
// tr("Game language") returns the line for the language the menu is in, or the English text itself when no line exists, so a new
// row works in English the moment it is written and shows up in tests/hle/ui_i18n_check.py until it is translated.
#pragma once
#include <string>

namespace snap::ui {

// Columns of the table (ui_i18n_table.cpp), in this order.
enum I18nLang { kLangEn, kLangFr, kLangDe, kLangEs, kLangIt, kLangJa, kLangKo, kLangZhHans, kLangZhHant, kLangCount };
constexpr const char* kI18nTags[kLangCount] = {"en", "fr", "de", "es", "it", "ja", "ko", "zh-hans", "zh-hant"};

struct I18nRow { const char* t[kLangCount]; };   // t[0] is the English key
extern const I18nRow g_i18n_rows[];
extern const unsigned g_i18n_count;

// The language the menu draws in: the Game language setting (explicit value), else the language the game started with (Auto = Windows
// display language, else English). Cheap: call it as often as needed.
int i18n_lang();
// Tests: force a language by tag ("ja"), or "" to follow the setting again. Returns false for an unknown tag.
bool i18n_force(const std::string& tag);
// The font fallback the language needs: true for Japanese, Korean and both Chinese scripts.
bool i18n_is_cjk(int lang);

const char* tr(const char* en);          // translated line, or `en`
const char* tr(const std::string& en);   // same; the result stays valid as long as `en` does when no line exists
inline const char* tr(const std::string* en) { return tr(*en); }

// Tests. Every key asked for since the last i18n_collect_reset() with collection on is remembered; i18n_check() lists the ones the table
// lacks (or lacks in some language) and returns how many problems it found. `report` receives one line per problem.
void i18n_collect(bool on);
void i18n_collect_reset();
size_t i18n_check(std::string* report);

}  // namespace snap::ui
