#!/usr/bin/env python3
"""F1 menu translations (runtime/ui/ui_i18n_table.cpp).

Checks, with no game run:
  * every row has all 9 columns (en fr de es it ja ko zh-hans zh-hant), none empty, no duplicate English key;
  * the % placeholders (and "\n" line breaks) of every translation equal the English ones, in the same order;
  * every English text the sources hand to tr("...") or keep in the option tables of ui_options.cpp has a row.
The run-time half is the test script command `expect_i18n` (runtime/ui/ui_test.cpp): after `i18n_collect 1` and a visit to every page it
lists any drawn text the table lacks (that catches text that reaches tr() through a variable).

  python -I tests/hle/ui_i18n_check.py              exit 0 = ok
  python -I tests/hle/ui_i18n_check.py --strict     also list unused rows and drawn literals that skip tr()
  python -I tests/hle/ui_i18n_check.py --list-keys  JSON list of every English text that may need a row (superset, for translators)
"""
import bisect
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "runtime" / "ui"
TABLE = UI / "ui_i18n_table.cpp"
LANGS = ["en", "fr", "de", "es", "it", "ja", "ko", "zh-hans", "zh-hant"]
SKIP_FILES = {"ui_i18n.cpp", "ui_i18n_table.cpp", "ui_test.cpp", "ui_tune.cpp", "ui_font.cpp"}

STR = r'"(?:[^"\\\n]|\\.)*"'
STRS = rf"(?:{STR}\s*)+"   # adjacent literals concatenate
STR_RE = re.compile(STR)
CHAR_RE = re.compile(r"'(?:[^'\\\n]|\\.)+'")

ESC = {"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'", "r": "\r", "0": "\0"}


def unescape(lit: str) -> str:
    body = lit[1:-1]
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            n = body[i + 1]
            if n in ESC:
                out.append(ESC[n]); i += 2; continue
            if n == "x":
                j = i + 2
                while j < len(body) and body[j] in "0123456789abcdefABCDEF": j += 1
                out.append(chr(int(body[i + 2:j], 16))); i = j; continue
            if n == "u":
                out.append(chr(int(body[i + 2:i + 6], 16))); i += 6; continue
        out.append(c); i += 1
    return "".join(out)


def join_lits(blob: str) -> str:
    return "".join(unescape(m) for m in STR_RE.findall(blob))


def strip_comments(src: str) -> str:
    """Blank // and /* */ comments, keep string and char literals (and every newline) intact."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            m = STR_RE.match(src, i)
            if m: out.append(m.group(0)); i = m.end(); continue
        elif c == "'":
            m = CHAR_RE.match(src, i)
            if m: out.append(m.group(0)); i = m.end(); continue
        elif src.startswith("//", i):
            j = src.find("\n", i); j = n if j < 0 else j
            out.append(" " * (j - i)); i = j; continue
        elif src.startswith("/*", i):
            j = src.find("*/", i); j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j])); i = j; continue
        out.append(c); i += 1
    return "".join(out)


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def table_rows():
    src = strip_comments(read(TABLE))
    body = src[src.index("g_i18n_rows[]"):]
    rows = []
    for m in re.finditer(r"\{\{(?:" + STR + r"|[\s,])*\}\}", body):
        rows.append([unescape(x) for x in STR_RE.findall(m.group(0))])
    return rows


PLACE = re.compile(r"%(?:%|[-+0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfFgGaAcspn])")


def placeholders(s: str):
    return [p for p in PLACE.findall(s) if p != "%%"]


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def opt_keys(src: str, fname: str):
    """Rows of the Opt tables in ui_options.cpp:  {"key", "Label", kKind, kWhen, "help", "default", {{"value", "Choice name"}, ...}, ...}"""
    out = {}
    row = re.compile(rf"\{{\s*{STR}\s*,\s*({STR})\s*,\s*k(?:Section|Choice|Slider|Toggle|Path)\s*,\s*k\w+\s*,\s*({STRS})", re.S)
    for m in row.finditer(src):
        line = line_of(src, m.start())
        out.setdefault(unescape(m.group(1)), []).append(f"{fname}:{line}")
        h = join_lits(m.group(2))
        if h: out.setdefault(h, []).append(f"{fname}:{line}")
        rest = src[m.end():m.end() + 6000]
        mm = re.match(rf"\s*,\s*{STR}\s*,\s*\{{((?:\s*\{{\s*{STR}\s*,\s*{STR}\s*\}}\s*,?)+)\s*\}}", rest, re.S)
        if mm:
            for pair in re.finditer(rf"\{{\s*{STR}\s*,\s*({STR})\s*\}}", mm.group(1)):
                out.setdefault(unescape(pair.group(1)), []).append(f"{fname}:{line}")
    return out


def source_keys():
    """English texts the sources ask for: tr("...") calls, the option tables, the tab names."""
    keys = {}
    for p in sorted(UI.glob("*.cpp")):
        if p.name in SKIP_FILES: continue
        src = strip_comments(read(p))
        for m in re.finditer(rf"\btr\(\s*({STRS})\s*[,)]", src):
            keys.setdefault(join_lits(m.group(1)), []).append(f"{p.name}:{line_of(src, m.start())}")
        if p.name == "ui_options.cpp":
            for k, where in opt_keys(src, p.name).items():
                keys.setdefault(k, []).extend(where)
        if p.name == "ui_controls.cpp":
            m = re.search(r"kTabs\[\]\s*=\s*\{(.*?)\};", src, re.S)
            if m:
                for lit in STR_RE.findall(m.group(1)):
                    keys.setdefault(unescape(lit), []).append(f"{p.name}:kTabs")
    return keys


SKIP_LINE = re.compile(
    r"hle::log|fprintf\(|\bout\(|\bcheck\(|getenv|st::(?:get|set|erase)|settings::(?:get|set|erase)|strcmp|==\s*\"|\"\s*==|\.find\(|\.count\(|"
    r"#include|#define|PushID|SetNextWindow|BeginChild|ImGui::Begin\(|InvisibleButton|GetID|key_from_name|button_from_name|_wgetenv|popen|system\(")
IDENT = re.compile(r"[A-Z0-9_]+|[a-z0-9_]+|[a-z0-9_.]+\.[a-z]+|[A-Za-z]:[\\/].*|[%.0-9a-z]+")


NOT_TEXT = {"XInputGetState", "Segoe UI", "AsiaKNGD-E", "SnapProfilePicker", "WAVEfmt", "profiles are off", "<unknown option>", "Snap Recomp",
            "%.0f FPS", "CPU %.2f ms  GPU %.2f ms", "%.0f nits"}


def visual_default_texts():
    p = ROOT / "runtime" / "hle" / "visual_defaults.h"
    out = {}
    if p.exists():
        src = strip_comments(read(p))
        for m in re.finditer(rf'\{{\s*"SNAP_\w+"\s*,\s*{STR}\s*,\s*{STR}\s*,\s*({STR})\s*,\s*({STR})\s*\}}', src):
            for g in (m.group(1), m.group(2)):
                out.setdefault(unescape(g), []).append("visual_defaults.h")
    return out


def candidate_keys():
    """Superset for the translators: the required keys, the visual-default phrases, and every literal in the menu sources that looks like
    text a player could read (helpers such as desc() / checkbox() / slider() translate their argument inside, so their call sites hold plain
    literals). The authority on completeness is the run-time check (`expect_i18n`)."""
    keys = dict(source_keys())
    for k, w in visual_default_texts().items(): keys.setdefault(k, []).extend(w)
    for p in sorted(UI.glob("*.cpp")):
        if p.name in SKIP_FILES: continue
        text = strip_comments(read(p))
        lines = text.split("\n")
        starts = [0]
        for ln in lines: starts.append(starts[-1] + len(ln) + 1)
        for m in re.finditer(STRS, text):
            n = bisect.bisect_right(starts, m.start())
            if SKIP_LINE.search(lines[n - 1]): continue
            t = join_lits(m.group(0))
            core = t.split("##")[0]
            if not re.search(r"[A-Za-z]{3}", core) or t.startswith("##"): continue
            if IDENT.fullmatch(t): continue
            if " " not in t and not t[:1].isupper(): continue
            if "SNAP_" in t and " " not in t: continue
            if t != t.strip() or t in NOT_TEXT or "--" in t or t.startswith("navid="): continue   # fragments, program names, debug lines
            if "##" in t: t = t.split("#")[0]
            keys.setdefault(t, []).append(f"{p.name}:{n}")
    return keys


DRAW = re.compile(r"\bImGui::(?:Text|TextDisabled|TextColored|TextUnformatted|TextWrapped|Button|SmallButton|Checkbox|RadioButton|Selectable|MenuItem|"
                  rf"SliderFloat|SliderInt|BeginPopupModal|TableSetupColumn|SeparatorText|LabelText|BulletText)\(\s*(?:[A-Za-z_.]+\s*,\s*)?({STR})")


def unwrapped():
    out = []
    for p in sorted(UI.glob("*.cpp")):
        if p.name in SKIP_FILES: continue
        src = strip_comments(read(p))
        for m in DRAW.finditer(src):
            s = unescape(m.group(1))
            if not re.search(r"[A-Za-z]{3}", s.split("##")[0]) or s.startswith("##") or s == "%s": continue
            out.append(f"{p.name}:{line_of(src, m.start())}: {s[:70]}")
    return out


def main():
    argv = sys.argv[1:]
    strict = "--strict" in argv
    if "--list-keys" in argv:
        out = [{"key": k, "kind": "help" if len(k) > 70 else "label", "where": w[0]} for k, w in sorted(candidate_keys().items())]
        print(json.dumps(out, ensure_ascii=False, indent=0))
        return 0
    keys = source_keys()
    rows = table_rows()
    errs, warns, seen = [], [], {}
    for r in rows:
        if len(r) != len(LANGS):
            errs.append(f"row has {len(r)} texts, want {len(LANGS)}: {r[:1]}"); continue
        en = r[0]
        if en in seen: errs.append(f"duplicate key: {en[:70]}")
        seen[en] = r
        for lang, t in zip(LANGS, r):
            if not t.strip(): errs.append(f"[{lang}] empty: {en[:70]}")
        ph = placeholders(en)
        for lang, t in zip(LANGS[1:], r[1:]):
            if placeholders(t) != ph: errs.append(f"[{lang}] placeholders {placeholders(t)} != {ph}: {en[:70]}")
            if t.count("\n") != en.count("\n"): errs.append(f"[{lang}] line breaks differ: {en[:70]}")
    for k, where in sorted(keys.items()):
        if k not in seen: errs.append(f"no table row for {k[:80]!r} ({where[0]})")
    if strict:
        for k in sorted(set(seen) - set(keys)): warns.append(f"unused row (fine when a helper translates it): {k[:80]}")
        warns += ["drawn without tr(): " + w for w in unwrapped()]
    print(f"{len(rows)} table rows, {len(keys)} keys asked for in the sources, {len(LANGS)} languages")
    for w in warns: print("warn:", w)
    for e in errs: print("ERROR:", e)
    ok = not errs
    print("OK" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
