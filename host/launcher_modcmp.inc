// SPDX-License-Identifier: MIT
// The "Which pack wins" page of the themed launcher, opened from the Mods page. Included by launcher_ui.cpp inside its anonymous namespace after
// launcher_album.inc (it uses ATex and a_image from there). It lists the files two enabled packs both change (g_mod_clash, the game's own loader
// run on mods.json), and for the selected file shows both packs' versions side by side: a thumbnail of each texture (runtime/mods/mods_thumb.cpp
// reads the .nutexb and Direct3D 11 samples the blocks, local only), or the file sizes for anything else. Enter puts the losing pack above the
// winner in mods.json (the same move as the arrows on the Mods page).

struct ModCmp {
  std::string scope;                // pack id the list is limited to; "" = every conflict
  std::vector<int> idx;             // indexes into g_mod_clash, in path order
  int sel = 0, top = 0;
  std::string loaded;               // path + pack ids of the pair shown in `a` / `b`
  ATex a, b;                        // winner / loser thumbnails
  std::string ia, ib;               // one line each under the thumbnails ("BC7 2048x2048, mip 2 shown" / why there is no picture)
};
ModCmp g_cmp;

std::string cmp_leaf(const std::string& p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}
std::string cmp_name(const std::string& id) {
  for (const ModPack& m : g.mods) if (m.id == id) return m.name;
  return id;
}
bool cmp_is_texture(const std::string& path) {
  std::string l = lower_s(path);
  return l.size() > 7 && l.compare(l.size() - 7, 7, ".nutexb") == 0;
}

// The file one side of a conflict is read from. A zip pack is unpacked first (the game does the same at its start; the copy lives under .unpacked).
std::string cmp_file(const ModClash& c, bool winner) {
  const std::string& f = winner ? c.winner_file : c.loser_file;
  if (!f.empty()) return f;
  const std::string& id = winner ? c.winner : c.loser;
  std::vector<std::string> warn;
  const std::string dir = mods::unpack_zip(g.mods_root, id, warn);
  return dir.empty() ? std::string() : dir + "/" + c.path;
}

std::string cmp_kb(unsigned long long b) {
  char t[32];
  if (b >= (1ull << 20)) std::snprintf(t, sizeof t, "%.1f MB", (double)b / (1ull << 20));
  else std::snprintf(t, sizeof t, "%d KB", (int)((b + 512) >> 10));
  return t;
}

// A texture's blocks as a Direct3D 11 texture. The block formats need the sizes to be whole blocks, which Thumb guarantees.
bool cmp_make(const mods::Thumb& t, ATex& out) {
  a_free(out);
  if (!g.dev || t.w <= 0 || t.h <= 0) return false;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = (UINT)t.w; d.Height = (UINT)t.h; d.MipLevels = 1; d.ArraySize = 1;
  d.Format = (DXGI_FORMAT)t.dxgi; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sd{};
  sd.pSysMem = t.blocks.data(); sd.SysMemPitch = t.pitch;
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(g.dev->CreateTexture2D(&d, &sd, &tex))) return false;
  const HRESULT hr = g.dev->CreateShaderResourceView(tex, nullptr, &out.srv);
  tex->Release();
  if (FAILED(hr)) { out.srv = nullptr; return false; }
  out.w = t.w; out.h = t.h;
  return true;
}

void cmp_side(const std::string& file, bool texture, ATex& tex, std::string& info) {
  a_free(tex);
  info.clear();
  std::error_code ec;
  if (file.empty() || !fs::exists(P(file), ec)) { info = "The file could not be read"; return; }
  const unsigned long long sz = (unsigned long long)fs::file_size(P(file), ec);
  if (!texture) { info = "No preview for this kind of file, " + cmp_kb(sz); return; }
  mods::Thumb t;
  std::string err;
  if (!mods::nutexb_thumb_file(file, 512, &t, &err)) { info = "No picture: " + err + ", " + cmp_kb(sz); return; }
  if (!cmp_make(t, tex)) { info = "No picture: this graphics card cannot show " + t.format; return; }
  info = t.format + " " + std::to_string(t.src_w) + "x" + std::to_string(t.src_h) + ", " + cmp_kb(sz) + (t.mip ? ", smaller copy shown" : "");
}

// Reads the two files of the selected conflict (once per selection).
void cmp_load() {
  if (g_cmp.sel < 0 || g_cmp.sel >= (int)g_cmp.idx.size()) { a_free(g_cmp.a); a_free(g_cmp.b); g_cmp.loaded.clear(); return; }
  const ModClash& c = g_mod_clash[(size_t)g_cmp.idx[(size_t)g_cmp.sel]];
  const std::string key = c.path + "|" + c.winner + "|" + c.loser;
  if (key == g_cmp.loaded) return;
  g_cmp.loaded = key;
  const bool tex = cmp_is_texture(c.path);
  cmp_side(cmp_file(c, true), tex, g_cmp.a, g_cmp.ia);
  cmp_side(cmp_file(c, false), tex, g_cmp.b, g_cmp.ib);
}

void cmp_rebuild(const std::string& keep_path) {
  g_cmp.idx.clear();
  for (size_t i = 0; i < g_mod_clash.size(); ++i) {
    const ModClash& c = g_mod_clash[i];
    if (!g_cmp.scope.empty() && c.winner != g_cmp.scope && c.loser != g_cmp.scope) continue;
    g_cmp.idx.push_back((int)i);
  }
  std::stable_sort(g_cmp.idx.begin(), g_cmp.idx.end(), [](int a, int b) {
    const bool ta = cmp_is_texture(g_mod_clash[(size_t)a].path), tb = cmp_is_texture(g_mod_clash[(size_t)b].path);
    if (ta != tb) return ta;   // textures first
    return lower_s(g_mod_clash[(size_t)a].path) < lower_s(g_mod_clash[(size_t)b].path);
  });
  g_cmp.sel = 0;
  if (!keep_path.empty()) for (size_t i = 0; i < g_cmp.idx.size(); ++i) if (g_mod_clash[(size_t)g_cmp.idx[i]].path == keep_path) g_cmp.sel = (int)i;
  g_cmp.loaded.clear();
  cmp_load();
}

// Opened from the Mods page with the selected pack (row >= 1) or every conflict (row 0).
void modcmp_open(int mods_row) {
  g_cmp.scope = mods_row >= 1 && mods_row <= (int)g.mods.size() ? g.mods[(size_t)mods_row - 1].id : std::string();
  g_cmp.top = 0;
  cmp_rebuild("");
  g.view = G::MODCMP;
}

// Moves the losing pack of the selected file just above the winner.
void cmp_make_loser_win() {
  if (g_cmp.sel < 0 || g_cmp.sel >= (int)g_cmp.idx.size()) return;
  const ModClash c = g_mod_clash[(size_t)g_cmp.idx[(size_t)g_cmp.sel]];
  int w = -1, l = -1;
  for (int i = 0; i < (int)g.mods.size(); ++i) { if (g.mods[(size_t)i].id == c.winner) w = i; if (g.mods[(size_t)i].id == c.loser) l = i; }
  if (w < 0 || l < 0 || l < w) return;
  const ModPack moved = g.mods[(size_t)l];
  g.mods.erase(g.mods.begin() + l);
  g.mods.insert(g.mods.begin() + w, moved);
  save_mods();
  mods_toast(moved.name + " now wins over " + cmp_name(c.winner) + ". It loads at the next start.");
  cmp_rebuild(c.path);
}

void modcmp_input(bool up, bool dn, bool ok, bool back) {
  if (back) { g.view = G::MODS; return; }
  const int n = (int)g_cmp.idx.size();
  if (n == 0) return;
  if (up) g_cmp.sel = (g_cmp.sel + n - 1) % n;
  if (dn) g_cmp.sel = (g_cmp.sel + 1) % n;
  if (ok) cmp_make_loser_win();
  cmp_load();
}

void draw_modcmp() {
  draw_home_bg();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  diamond(122, 82, 46, C(0x8f8e90));
  aperture(122, 82, 22, C(kCream));
  text(180, 70, 42, C(0x6a6561), "Which pack wins");
  const int n = (int)g_cmp.idx.size();
  std::string head = n == 0 ? "No file is changed by two enabled packs" : std::to_string(n) + (n == 1 ? " file is" : " files are") + " changed by two packs" +
                                                                      (g_cmp.scope.empty() ? "" : " (" + cmp_name(g_cmp.scope) + ")");
  text(182, 106, 17, C(n == 0 ? kGrey : 0xb0662e), head.c_str());
  const bool moved = ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0;
  const bool click = ImGui::IsMouseClicked(0);
  // the list
  const int vis = 9;
  const float lx = 60, ly = 142, lw = 470, rh = 50;
  if (hot(lx, ly, lw, vis * rh) && ImGui::GetIO().MouseWheel != 0) g_cmp.top -= (int)(ImGui::GetIO().MouseWheel * 3);
  if (g_cmp.sel < g_cmp.top) g_cmp.top = g_cmp.sel;
  if (g_cmp.sel >= g_cmp.top + vis) g_cmp.top = g_cmp.sel - vis + 1;
  g_cmp.top = std::clamp(g_cmp.top, 0, std::max(0, n - vis));
  for (int k = 0; k < vis && g_cmp.top + k < n; ++k) {
    const int i = g_cmp.top + k;
    const ModClash& c = g_mod_clash[(size_t)g_cmp.idx[(size_t)i]];
    const float y = ly + k * rh;
    const bool sel = i == g_cmp.sel;
    if (hot(lx, y, lw, rh - 4)) { if (moved && g_cmp.sel != i) { g_cmp.sel = i; cmp_load(); } if (click) { g_cmp.sel = i; cmp_load(); } }
    rrect(lx, y, lw, rh - 4, 14, C(sel ? kYellow : 0xffffff));
    if (sel) dl->AddRect(V(lx, y), V(lx + lw, y + rh - 4), C(kOrange), 14 * g.s, 0, 3 * g.s);
    text_clip(lx + 16, y + 15, 17, C(sel ? kBrown : 0x6a6561), cmp_leaf(c.path).c_str(), lw - 32);
    text_clip(lx + 16, y + 34, 13, C(sel ? 0x7a6440 : 0x9a9795), (cmp_name(c.winner) + " beats " + cmp_name(c.loser)).c_str(), lw - 32);
  }
  if (n > vis) {
    char t[32];
    std::snprintf(t, sizeof t, "%d - %d of %d", g_cmp.top + 1, std::min(n, g_cmp.top + vis), n);
    text(lx + lw, ly + vis * rh + 10, 14, C(kGrey), t, 2);
  }
  // the two versions
  const float cw = 316, ch = 440, cy = 142;
  const float cx[2] = {560, 560 + cw + 18};
  const ModClash* cc = (g_cmp.sel >= 0 && g_cmp.sel < n) ? &g_mod_clash[(size_t)g_cmp.idx[(size_t)g_cmp.sel]] : nullptr;
  for (int s = 0; s < 2; ++s) {
    const bool win = s == 0;
    shadow(cx[s], cy, cw, ch, 22);
    rrect(cx[s], cy, cw, ch, 22, C(0xffffff));
    rrect(cx[s], cy, cw, 48, 22, C(win ? 0x96c283 : 0xd49a6a), ImDrawFlags_RoundCornersTop);
    text(cx[s] + 20, cy + 24, 20, C(0xffffff), win ? "Wins (used in game)" : "Loses (hidden)");
    if (!cc) continue;
    text_clip(cx[s] + 20, cy + 72, 20, C(0x6a6561), cmp_name(win ? cc->winner : cc->loser).c_str(), cw - 40);
    const ATex& t = win ? g_cmp.a : g_cmp.b;
    const float bx = cx[s] + 14, by = cy + 100, bs = cw - 28;
    rrect(bx, by, bs, bs, 12, C(0xe9e6e2));
    if (t.srv && t.w > 0 && t.h > 0) {
      const float k = std::min(bs / (float)t.w, bs / (float)t.h);
      const float iw = t.w * k, ih = t.h * k;
      a_image(t, bx + (bs - iw) / 2, by + (bs - ih) / 2, iw, ih, 6);
    }
    text_wrap2(cx[s] + 20, by + bs + 22, 15, C(kGrey), win ? g_cmp.ia : g_cmp.ib, cw - 40);
  }
  if (cc) {
    const float bx = 560, by = cy + ch + 12, bw = 2 * cw + 18, bh = 36;
    const bool can = !g_cmp.idx.empty();
    if (hot(bx, by, bw, bh) && click && can) cmp_make_loser_win();
    rrect(bx, by, bw, bh, 18, C(hot(bx, by, bw, bh) ? 0xb6d291 : 0x96c283));
    text_clip(bx + bw / 2, by + bh / 2, 17, C(0xffffff), ("Make " + cmp_name(cc->loser) + " win (moves it above " + cmp_name(cc->winner) + ")").c_str(), bw - 24, 1);
  }
  help_bar("Up and down pick a file. Enter puts the losing pack above the winner. Changes apply at the next start. B goes back to the pack list.");
  if (click && hot(1186, 656, 94, 64)) g.view = G::MODS;
}
