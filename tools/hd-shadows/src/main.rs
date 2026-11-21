// HD shadows bake tool. Reads the player's own unpacked game data (extracted/drp-v262144), never writes to it.
// Output: 8-bit sun-shadow masks (PNG, one per game Shadow texture) in the folder given by --out, which the packer
// (scripts/hd/hdshadows_pack.py) turns into nutexb files for a local mod pack. Nothing here is committed or distributed.
//
//   hd-shadows survey --course LC_001_A
//   hd-shadows calib  --course LC_001_A --orig <masks of the originals> [--alpha <cut-out masks>] --sun-out sun.tsv
//   hd-shadows bake   --course LC_001_A --orig <...> [--alpha <...>] --sun-in sun.tsv --out <folder> --scale 2
//
// A course pack has one or more legs (variants); each leg names its environment levels (`LV_ENV_*`), and every level is
// baked with its own ray scene and its own fitted sun.
#![allow(dead_code)]
#[path = "../../level-editor/src/course.rs"]
mod course;
#[path = "../../level-editor/src/fb.rs"]
mod fb;
#[path = "../../level-editor/src/geom.rs"]
mod geom;
#[path = "../../level-editor/src/grow.rs"]
mod grow;
#[path = "../../level-editor/src/level.rs"]
mod level;
/// course.rs keeps an editor-only terrain grid; the bake never makes one.
mod ground {
    pub struct Terrain;
}
mod scene;
mod vk;

use std::collections::{HashMap, HashSet};
use std::io::Write;
use std::path::{Path, PathBuf};

struct Args {
    cmd: String,
    course: String,
    out: PathBuf,
    orig: PathBuf,
    alpha: Option<PathBuf>,
    alpha_ref: u32,
    scale: u32,
    samples: u32,
    sun: Option<(f32, f32)>, // azimuth, elevation degrees
    sun_in: Option<PathBuf>,
    sun_out: Option<PathBuf>,
    radius_deg: f32,
    bias: f32,
    uvset: Option<usize>,
    flipv: bool,
    invert: Option<bool>,
    only: Option<String>,
    level: Option<String>,
    max_size: u32,
    data: Option<PathBuf>,
    points_per: usize,
    report: Option<PathBuf>,
    skip_existing: bool,
    flags: Option<u32>,
    fit_two_sided: bool,
    bake_radius: Option<f32>,
}

fn parse_args() -> Args {
    let mut a = Args {
        cmd: "survey".into(),
        course: "LC_001_A".into(),
        out: PathBuf::from("hdshadows_out"),
        orig: PathBuf::from("hdshadows_orig"),
        alpha: None,
        alpha_ref: 102,
        scale: 2,
        samples: 32,
        sun: None,
        sun_in: None,
        sun_out: None,
        radius_deg: 0.5,
        bias: 0.03,
        uvset: None,
        flipv: false,
        invert: None,
        only: None,
        level: None,
        max_size: 4096,
        data: None,
        points_per: 3000,
        report: None,
        skip_existing: false,
        flags: None,
        fit_two_sided: false,
        bake_radius: None,
    };
    let v: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    if let Some(c) = v.first() {
        if !c.starts_with("--") {
            a.cmd = c.clone();
            i = 1;
        }
    }
    while i < v.len() {
        let k = v[i].as_str();
        let mut val = || {
            i += 1;
            v.get(i).cloned().unwrap_or_default()
        };
        match k {
            "--course" => a.course = val(),
            "--out" => a.out = val().into(),
            "--orig" => a.orig = val().into(),
            "--alpha" => a.alpha = Some(val().into()),
            "--alpha-ref" => a.alpha_ref = val().parse().unwrap_or(102),
            "--scale" => a.scale = val().parse().unwrap_or(2),
            "--samples" => a.samples = val().parse().unwrap_or(32),
            "--sun" => {
                let p: Vec<f32> = val().split(',').filter_map(|x| x.parse().ok()).collect();
                if p.len() == 2 {
                    a.sun = Some((p[0], p[1]));
                }
            }
            "--sun-in" => a.sun_in = Some(val().into()),
            "--sun-out" => a.sun_out = Some(val().into()),
            "--radius" => {
                a.radius_deg = val().parse().unwrap_or(0.5);
                a.bake_radius = Some(a.radius_deg);
            }
            "--bias" => a.bias = val().parse().unwrap_or(0.03),
            "--uv" => a.uvset = val().parse().ok(),
            "--flipv" => a.flipv = true,
            "--invert" => a.invert = Some(true),
            "--no-invert" => a.invert = Some(false),
            "--only" => a.only = Some(val()),
            "--level" => a.level = Some(val()),
            "--max-size" => a.max_size = val().parse().unwrap_or(4096),
            "--data" => a.data = Some(val().into()),
            "--points" => a.points_per = val().parse().unwrap_or(3000),
            "--report" => a.report = Some(val().into()),
            "--skip-existing" => a.skip_existing = true,
            "--flags" => a.flags = val().parse().ok(),
            "--fit-two-sided" => a.fit_two_sided = true,
            _ => eprintln!("unknown argument {k}"),
        }
        i += 1;
    }
    a
}

/// `<level>^<model>^<hash>^<shape>^<sub>^Shadow.nutexb` -> (level, model, shape, sub)
fn parse_shadow_name(f: &str) -> Option<(String, String, String, u64)> {
    let stem = f.strip_suffix("^Shadow.nutexb")?;
    let p: Vec<&str> = stem.split('^').collect();
    if p.len() != 5 {
        return None;
    }
    Some((p[0].into(), p[1].into(), p[3].into(), p[4].parse().ok()?))
}

fn read_png_gray(path: &Path) -> Option<(u32, u32, Vec<u8>)> {
    let f = std::fs::File::open(path).ok()?;
    let mut r = png::Decoder::new(std::io::BufReader::new(f)).read_info().ok()?;
    let mut buf = vec![0u8; r.output_buffer_size()?];
    let info = r.next_frame(&mut buf).ok()?;
    let ch = info.color_type.samples();
    let n = (info.width * info.height) as usize;
    let g: Vec<u8> = (0..n).map(|i| buf[i * ch]).collect();
    Some((info.width, info.height, g))
}

fn write_png_gray(path: &Path, w: u32, h: u32, data: &[u8]) {
    let tmp = path.with_extension("png.part");
    {
        let f = std::fs::File::create(&tmp).unwrap();
        let mut e = png::Encoder::new(std::io::BufWriter::new(f), w, h);
        e.set_color(png::ColorType::Grayscale);
        e.set_depth(png::BitDepth::Eight);
        e.set_compression(png::Compression::Fast);
        e.write_header().unwrap().write_image_data(data).unwrap();
    }
    std::fs::rename(&tmp, path).unwrap();
}

/// Triangle index per texel (0xFFFFFFFF = none), sampled at texel centres; `bleed` texels of gutter so filtering never
/// reads a gap.
fn raster(uv: &[[f32; 2]], idx: &[u32], size: u32, flipv: bool, bleed: u32) -> Vec<u32> {
    let s = size as i64;
    let mut out = vec![u32::MAX; (size * size) as usize];
    let fs = size as f32;
    for ti in 0..idx.len() / 3 {
        let i0 = idx[ti * 3] as usize;
        let i1 = idx[ti * 3 + 1] as usize;
        let i2 = idx[ti * 3 + 2] as usize;
        let f = |p: [f32; 2]| [p[0] * fs, (if flipv { 1.0 - p[1] } else { p[1] }) * fs];
        let (a, b, c) = (f(uv[i0]), f(uv[i1]), f(uv[i2]));
        let minx = a[0].min(b[0]).min(c[0]).floor().max(0.0) as i64;
        let maxx = (a[0].max(b[0]).max(c[0]).ceil() as i64).min(s - 1);
        let miny = a[1].min(b[1]).min(c[1]).floor().max(0.0) as i64;
        let maxy = (a[1].max(b[1]).max(c[1]).ceil() as i64).min(s - 1);
        let det = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        if det.abs() < 1e-12 || maxx - minx > 8192 || maxy - miny > 8192 {
            continue;
        }
        for y in miny..=maxy {
            for x in minx..=maxx {
                let p = [x as f32 + 0.5, y as f32 + 0.5];
                let w1 = ((p[0] - a[0]) * (c[1] - a[1]) - (p[1] - a[1]) * (c[0] - a[0])) / det;
                let w2 = ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])) / det;
                let w0 = 1.0 - w1 - w2;
                if w0 >= -1e-4 && w1 >= -1e-4 && w2 >= -1e-4 {
                    out[(y * s + x) as usize] = ti as u32;
                }
            }
        }
    }
    for _ in 0..bleed {
        let src = out.clone();
        for y in 0..s {
            for x in 0..s {
                let i = (y * s + x) as usize;
                if src[i] != u32::MAX {
                    continue;
                }
                'n: for (dx, dy) in [(-1i64, 0i64), (1, 0), (0, -1), (0, 1)] {
                    let (nx, ny) = (x + dx, y + dy);
                    if nx >= 0 && ny >= 0 && nx < s && ny < s && src[(ny * s + nx) as usize] != u32::MAX {
                        out[i] = src[(ny * s + nx) as usize];
                        break 'n;
                    }
                }
            }
        }
    }
    out
}

/// Fills texels no triangle covers from their neighbours (so mips and bilinear reads near UV seams see the island's own
/// value, as the game's own gutters do), then sets anything still empty to the mean.
fn dilate(v: &mut [u8], covered: &[bool], size: u32, steps: u32) {
    let s = size as usize;
    let mut have: Vec<bool> = covered.to_vec();
    for _ in 0..steps {
        let prev = have.clone();
        let src = v.to_vec();
        let mut any = false;
        for y in 0..s {
            for x in 0..s {
                let i = y * s + x;
                if prev[i] {
                    continue;
                }
                let (mut sum, mut n) = (0u32, 0u32);
                for (dx, dy) in [(-1i64, 0i64), (1, 0), (0, -1), (0, 1)] {
                    let (nx, ny) = (x as i64 + dx, y as i64 + dy);
                    if nx >= 0 && ny >= 0 && (nx as usize) < s && (ny as usize) < s && prev[ny as usize * s + nx as usize] {
                        sum += src[ny as usize * s + nx as usize] as u32;
                        n += 1;
                    }
                }
                if n > 0 {
                    v[i] = ((sum + n / 2) / n) as u8;
                    have[i] = true;
                    any = true;
                }
            }
        }
        if !any {
            break;
        }
    }
    let (mut sum, mut n) = (0u64, 0u64);
    for i in 0..v.len() {
        if covered[i] {
            sum += v[i] as u64;
            n += 1;
        }
    }
    let mean = if n > 0 { (sum / n) as u8 } else { 0 };
    for i in 0..v.len() {
        if !have[i] {
            v[i] = mean;
        }
    }
}

/// Triangle table in the layout of bake.comp (Tri: 8 x vec4 = 128 bytes).
fn tri_table(o: &scene::Obj, uvset: usize, flipv: bool, two_sided: bool) -> Vec<u8> {
    let uv = &o.uvs[uvset].1;
    let n = o.idx.len() / 3;
    let mut bytes = Vec::with_capacity(n * 128);
    let f = |x: f32| x.to_le_bytes();
    for t in 0..n {
        let ids = [o.idx[t * 3] as usize, o.idx[t * 3 + 1] as usize, o.idx[t * 3 + 2] as usize];
        for (k, i) in ids.iter().enumerate() {
            for c in o.pos[*i] {
                bytes.extend_from_slice(&f(c));
            }
            bytes.extend_from_slice(&f(if k == 0 && two_sided { 1.0 } else { 0.0 }));
        }
        for i in ids {
            for c in o.nrm[i] {
                bytes.extend_from_slice(&f(c));
            }
            bytes.extend_from_slice(&f(0.0));
        }
        let g = |p: [f32; 2]| [p[0], if flipv { 1.0 - p[1] } else { p[1] }];
        let (a, b, c) = (g(uv[ids[0]]), g(uv[ids[1]]), g(uv[ids[2]]));
        for v in [a[0], a[1], b[0], b[1], c[0], c[1], 0.0, 0.0] {
            bytes.extend_from_slice(&f(v));
        }
    }
    bytes
}

fn sun_vec(az: f32, el: f32) -> [f32; 3] {
    let (az, el) = (az.to_radians(), el.to_radians());
    [el.cos() * az.cos(), el.sin(), el.cos() * az.sin()]
}

struct Level {
    name: String,
    objs: Vec<scene::Obj>,
    /// (shadow texture file name, object index)
    shadows: Vec<(String, usize)>,
    /// shadow files of this level with no mesh object
    unmatched: Vec<String>,
}

/// Every environment level of every leg of the course, each once.
fn load_levels(root: &mut course::DataRoot, c: &course::CourseEntry, only_level: Option<&str>) -> Vec<Level> {
    let mut files: HashMap<(String, String, String, u64), String> = HashMap::new();
    for e in std::fs::read_dir(&c.dir).unwrap().flatten() {
        let n = e.file_name().to_string_lossy().to_string();
        if let Some(k) = parse_shadow_name(&n) {
            files.insert(k, n);
        }
    }
    let variants: Vec<u32> = root.variants(c).map(|v| v.iter().map(|x| x.no).collect()).unwrap_or_else(|_| vec![1]);
    let mut seen = HashSet::new();
    let mut levels: Vec<Level> = vec![];
    for vno in variants {
        let lc = match root.load(c, vno) {
            Ok(x) => x,
            Err(e) => {
                eprintln!("{} leg {vno}: {e}", c.name);
                continue;
            }
        };
        if std::env::var("HDS_LEVELS").is_ok() {
            for (li, l) in lc.levels.iter().enumerate() {
                let ms: Vec<&str> = lc.models.iter().filter(|m| m.level_idx == li).map(|m| m.inst.name.as_str()).collect();
                println!("leg {vno} level {} models {} sourced {} e.g. {:?}", l.name, ms.len(),
                    lc.models.iter().filter(|m| m.level_idx == li && m.source.is_some()).count(), &ms[..ms.len().min(6)]);
            }
        }
        for (li, l) in lc.levels.iter().enumerate() {
            if !l.name.starts_with("LV_ENV") || !seen.insert(l.name.clone()) {
                continue;
            }
            if only_level.map_or(false, |f| !l.name.contains(f)) {
                continue;
            }
            let mut objs = vec![];
            for pm in lc.models.iter().filter(|m| m.level_idx == li) {
                let n = pm.inst.name.to_lowercase();
                if n.contains("lightingmodel") {
                    continue;
                }
                let Some(src) = &pm.source else { continue };
                let xf = scene::instance_matrix(pm.inst.pos, pm.inst.rot, pm.inst.scale);
                match scene::load_objects(src, &pm.inst.name, xf, pm.inst_idx) {
                    Ok(mut o) => objs.append(&mut o),
                    Err(e) => eprintln!("{}: {e}", pm.inst.name),
                }
            }
            // exact (level, model, shape, sub); else the one file of this level whose model name extends the
            // instance name (the game names some baked models `<instance><n>`, e.g. GrassX3 -> GrassX31)
            let mut used = HashSet::new();
            let mut shadows = vec![];
            for (i, o) in objs.iter().enumerate() {
                let key = (l.name.clone(), o.model.clone(), o.name.clone(), o.subindex);
                let f = files.get(&key).cloned().or_else(|| {
                    let c: Vec<&String> = files
                        .iter()
                        .filter(|(k, _)| k.0 == l.name && k.2 == o.name && k.3 == o.subindex && k.1.starts_with(&o.model) && k.1 != o.model)
                        .map(|(_, v)| v)
                        .collect();
                    if c.len() == 1 { Some(c[0].clone()) } else { None }
                });
                if let Some(f) = f {
                    if used.insert(f.clone()) {
                        shadows.push((f, i));
                    }
                }
            }
            let mut unmatched: Vec<String> = files.iter().filter(|(k, v)| k.0 == l.name && !used.contains(*v)).map(|(_, v)| v.clone()).collect();
            unmatched.sort();
            levels.push(Level { name: l.name.clone(), objs, shadows, unmatched });
        }
    }
    levels
}

/// Cut-out mask per material (`<alpha dir>/<material>.png`), loaded once.
struct MaskSet {
    index: HashMap<String, u32>,
    cards: vk::Cards,
}

impl MaskSet {
    /// Whether the material's cut-out is solid at `uv` (TextureCoordinate0, wrapped), as trace.glsl tests it.
    fn solid_at(&self, material: &str, uv: [f32; 2]) -> bool {
        let Some(&id) = self.index.get(material) else { return true };
        let m = &self.cards.masks[id as usize * 16..id as usize * 16 + 16];
        let r = |k: usize| u32::from_le_bytes([m[k], m[k + 1], m[k + 2], m[k + 3]]);
        let (off, w, h, rf) = (r(0), r(4), r(8), r(12));
        let fu = uv[0] - uv[0].floor();
        let fv = uv[1] - uv[1].floor();
        let x = ((fu * w as f32) as u32).min(w - 1);
        let y = ((fv * h as f32) as u32).min(h - 1);
        self.cards.data[(off + y * w + x) as usize] as u32 > rf
    }
}

fn load_masks(dir: Option<&Path>, objs: &[scene::Obj], alpha_ref: u32) -> MaskSet {
    let mut m = MaskSet { index: HashMap::new(), cards: vk::Cards::default() };
    let Some(dir) = dir else { return m };
    for o in objs {
        if m.index.contains_key(&o.material) {
            continue;
        }
        if let Some((w, h, g)) = read_png_gray(&dir.join(format!("{}.png", o.material))) {
            let id = (m.cards.masks.len() / 16) as u32;
            for v in [m.cards.data.len() as u32, w, h, alpha_ref] {
                m.cards.masks.extend_from_slice(&v.to_le_bytes());
            }
            m.cards.data.extend_from_slice(&g);
            while m.cards.data.len() % 4 != 0 {
                m.cards.data.push(0);
            }
            m.index.insert(o.material.clone(), id);
        }
    }
    m
}

fn is_card(o: &scene::Obj, masks: &MaskSet) -> bool {
    masks.index.contains_key(&o.material) && !o.uvs.is_empty()
}

/// The level's ray scene: every lightmapped model except sky and water; cut-out materials as alpha-tested cards;
/// models named in `two_sided` block light from both sides even when FLAG_CULL_* is set.
fn build_scene(gpu: &mut vk::Gpu, l: &Level, masks: &mut MaskSet, two_sided: &HashSet<String>) {
    let mut solid: Vec<[f32; 3]> = vec![];
    let mut two: Vec<[f32; 3]> = vec![];
    let mut card: Vec<[f32; 3]> = vec![];
    masks.cards.tris.clear();
    for o in &l.objs {
        let m = o.model.to_lowercase();
        if m.contains("_sky") || m.contains("water") {
            continue;
        }
        if let (true, Some(&id)) = (is_card(o, masks), masks.index.get(&o.material)) {
            let uv = &o.uvs[0].1;
            for t in o.idx.chunks_exact(3) {
                for &i in t {
                    card.push(o.pos[i as usize]);
                }
                let (a, b, c) = (uv[t[0] as usize], uv[t[1] as usize], uv[t[2] as usize]);
                for v in [a[0], a[1], b[0], b[1], c[0], c[1], f32::from_bits(id), 0.0] {
                    masks.cards.tris.extend_from_slice(&v.to_le_bytes());
                }
            }
        } else {
            let dst = if two_sided.contains(&o.model) { &mut two } else { &mut solid };
            for &i in &o.idx {
                dst.push(o.pos[i as usize]);
            }
        }
    }
    if std::env::var("HDS_QUIET").is_err() {
        eprintln!("{}: {} solid, {} two-sided and {} card triangles in the ray scene", l.name, solid.len() / 3, two.len() / 3, card.len() / 3);
    }
    gpu.set_scene(&solid, &two, &card, &masks.cards).expect("scene");
}

/// One shadow map with its original mask, for calibration and scoring.
struct Item<'a> {
    file: String,
    obj: &'a scene::Obj,
    w: u32,
    orig: Vec<u8>,
}

fn items<'a>(a: &Args, l: &'a Level) -> Vec<Item<'a>> {
    let mut v = vec![];
    for (f, oi) in &l.shadows {
        let o = &l.objs[*oi];
        if o.uvs.is_empty() || a.only.as_ref().map_or(false, |x| !o.model.contains(x.as_str())) {
            continue;
        }
        let stem = f.trim_end_matches(".nutexb");
        let Some((w, h, g)) = read_png_gray(&a.orig.join(format!("{stem}.png"))) else { continue };
        if w != h {
            continue;
        }
        v.push(Item { file: f.clone(), obj: o, w, orig: g });
    }
    v
}

fn uvset_of(a: &Args, o: &scene::Obj) -> usize {
    a.uvset.unwrap_or(o.uvs.len().saturating_sub(1)).min(o.uvs.len().saturating_sub(1))
}

/// One texel of a map at its own resolution: world position, geometric normal, shading normal (w = 1 on a card).
struct Texel {
    index: usize,
    pos: [f32; 3],
    gn: glam::Vec3,
    nrm: [f32; 4],
}

/// The texels of object `o` in a `w` x `w` map that the mesh covers and the game can show (see-through parts of a card
/// are left out: the original holds anything there). `keep(k, n)` chooses which of the n covered texels to look at.
fn texels(a: &Args, o: &scene::Obj, w: u32, masks: &MaskSet, keep: &dyn Fn(usize, usize) -> bool) -> Vec<Texel> {
    let us = uvset_of(a, o);
    let uv = &o.uvs[us].1;
    let tt = raster(uv, &o.idx, w, a.flipv, 0);
    let covered: Vec<usize> = (0..tt.len()).filter(|&i| tt[i] != u32::MAX).collect();
    let two = is_card(o, masks);
    let mut out = vec![];
    for (k, &i) in covered.iter().enumerate() {
        if !keep(k, covered.len()) {
            continue;
        }
        let t = tt[i] as usize;
        let ids = [o.idx[t * 3] as usize, o.idx[t * 3 + 1] as usize, o.idx[t * 3 + 2] as usize];
        let s = w as f32;
        let p = [((i as u32 % w) as f32 + 0.5) / s, ((i as u32 / w) as f32 + 0.5) / s];
        let g = |q: [f32; 2]| [q[0], if a.flipv { 1.0 - q[1] } else { q[1] }];
        let (u0, u1, u2) = (g(uv[ids[0]]), g(uv[ids[1]]), g(uv[ids[2]]));
        let e1 = [u1[0] - u0[0], u1[1] - u0[1]];
        let e2 = [u2[0] - u0[0], u2[1] - u0[1]];
        let d = [p[0] - u0[0], p[1] - u0[1]];
        let det = e1[0] * e2[1] - e1[1] * e2[0];
        if det.abs() < 1e-20 {
            continue;
        }
        let bv = (d[0] * e2[1] - d[1] * e2[0]) / det;
        let bw = (e1[0] * d[1] - e1[1] * d[0]) / det;
        let mut b = [(1.0 - bv - bw).max(0.0), bv.max(0.0), bw.max(0.0)];
        let sb = b[0] + b[1] + b[2];
        b = [b[0] / sb, b[1] / sb, b[2] / sb];
        let lerp = |arr: &Vec<[f32; 3]>| {
            let mut r = [0.0f32; 3];
            for c in 0..3 {
                r[c] = arr[ids[0]][c] * b[0] + arr[ids[1]][c] * b[1] + arr[ids[2]][c] * b[2];
            }
            r
        };
        if two {
            let t0 = &o.uvs[0].1;
            let uv0 = [
                t0[ids[0]][0] * b[0] + t0[ids[1]][0] * b[1] + t0[ids[2]][0] * b[2],
                t0[ids[0]][1] * b[0] + t0[ids[1]][1] * b[1] + t0[ids[2]][1] * b[2],
            ];
            if !masks.solid_at(&o.material, uv0) {
                continue;
            }
        }
        let pos = lerp(&o.pos);
        let nn = glam::Vec3::from(lerp(&o.nrm)).normalize_or_zero();
        let (p0, p1, p2) = (glam::Vec3::from(o.pos[ids[0]]), glam::Vec3::from(o.pos[ids[1]]), glam::Vec3::from(o.pos[ids[2]]));
        let mut gn = (p1 - p0).cross(p2 - p0).normalize_or_zero();
        if gn.dot(nn) < 0.0 {
            gn = -gn;
        }
        out.push(Texel { index: i, pos, gn, nrm: [nn.x, nn.y, nn.z, if two { 1.0 } else { 0.0 }] });
    }
    out
}

/// Sampled texels of every item: 48-byte points for points.comp, the original value, the item index and the normal.
fn make_points(a: &Args, its: &[Item], masks: &MaskSet) -> (Vec<u8>, Vec<u8>, Vec<u32>, Vec<[f32; 4]>) {
    let mut bytes = vec![];
    let mut label = vec![];
    let mut nrms = vec![];
    let mut owner = vec![];
    let f = |x: f32| x.to_le_bytes();
    for (ii, it) in its.iter().enumerate() {
        let per = a.points_per;
        let keep = move |k: usize, n: usize| {
            let step = (n as f64 / per as f64).max(1.0);
            // the first texel of each run of `step`
            k == 0 || (k as f64 / step).floor() != ((k - 1) as f64 / step).floor()
        };
        for t in texels(a, it.obj, it.w, masks, &keep) {
            for v in [t.pos[0], t.pos[1], t.pos[2], 0.0, t.gn.x, t.gn.y, t.gn.z, t.nrm[3], t.nrm[0], t.nrm[1], t.nrm[2], 0.0] {
                bytes.extend_from_slice(&f(v));
            }
            label.push(it.orig[t.index]);
            owner.push(ii as u32);
            nrms.push(t.nrm);
        }
    }
    (bytes, label, owner, nrms)
}

#[derive(Clone, Copy, Default)]
struct Conf {
    tp: u64,
    tn: u64,
    fp: u64,
    fnn: u64,
}

impl Conf {
    fn add(&mut self, pred: bool, truth: bool) {
        match (pred, truth) {
            (true, true) => self.tp += 1,
            (false, false) => self.tn += 1,
            (true, false) => self.fp += 1,
            (false, true) => self.fnn += 1,
        }
    }
    /// balanced accuracy, 1 = identical, 0.5 = chance
    fn bacc(&self) -> f64 {
        let p = self.tp + self.fnn;
        let n = self.tn + self.fp;
        if p == 0 || n == 0 {
            return self.agree();
        }
        0.5 * (self.tp as f64 / p as f64 + self.tn as f64 / n as f64)
    }
    fn agree(&self) -> f64 {
        (self.tp + self.tn) as f64 / (self.tp + self.tn + self.fp + self.fnn).max(1) as f64
    }
    fn total(&self) -> u64 {
        self.tp + self.tn + self.fp + self.fnn
    }
}

/// Score of an occlusion result against the original values; `invert`: the original stores light, not shadow.
/// Only texels that face the sun count: on a texel facing away the game's own N.L makes it dark whatever the mask says,
/// so the original bakes leave anything there.
fn faces(n: [f32; 4], sun: [f32; 3]) -> bool {
    let d = n[0] * sun[0] + n[1] * sun[1] + n[2] * sun[2];
    (if n[3] > 0.5 { d.abs() } else { d }) > 0.02
}

fn score(occ: &[u8], label: &[u8], nrm: &[[f32; 4]], sun: [f32; 3], invert: bool) -> Conf {
    let mut c = Conf::default();
    for i in 0..occ.len() {
        if faces(nrm[i], sun) {
            c.add((occ[i] > 127) != invert, label[i] > 127);
        }
    }
    c
}

#[derive(Clone)]
struct Sun {
    /// `<course>:<level>`
    key: String,
    az: f32,
    el: f32,
    radius: f32,
    bias: f32,
    invert: bool,
    flags: u32,
    bacc: f64,
    /// models whose faces block light from both sides (fitted when FLAG_CULL_* is on)
    two: Vec<String>,
}

fn read_suns(p: &Path) -> Vec<Sun> {
    let mut v = vec![];
    for line in std::fs::read_to_string(p).unwrap_or_default().lines() {
        let c: Vec<&str> = line.split('\t').collect();
        if c.len() < 8 || c[0] == "level" {
            continue;
        }
        let f = |s: &str| s.parse::<f32>().unwrap_or(0.0);
        v.push(Sun {
            key: c[0].into(),
            az: f(c[1]),
            el: f(c[2]),
            radius: f(c[3]),
            bias: f(c[4]),
            invert: c[5] == "1",
            flags: c[6].parse().unwrap_or(0),
            bacc: c[7].parse().unwrap_or(0.0),
            two: c.get(8).map(|x| x.split(',').filter(|m| !m.is_empty()).map(String::from).collect()).unwrap_or_default(),
        });
    }
    v
}

fn write_suns(p: &Path, suns: &[Sun]) {
    let mut all = read_suns(p);
    for s in suns {
        all.retain(|x| x.key != s.key);
        all.push(s.clone());
    }
    all.sort_by(|a, b| a.key.cmp(&b.key));
    let mut t = String::from("level\taz\tel\tradius\tbias\tinvert\tflags\tbacc\ttwo_sided\n");
    for s in &all {
        t += &format!("{}\t{:.2}\t{:.2}\t{:.3}\t{:.4}\t{}\t{}\t{:.4}\t{}\n", s.key, s.az, s.el, s.radius, s.bias, s.invert as u8, s.flags, s.bacc, s.two.join(","));
    }
    std::fs::write(p, t).unwrap();
}

fn calib(a: &Args, gpu: &mut vk::Gpu, course: &str, l: &Level, masks: &mut MaskSet) -> Option<Sun> {
    let its = items(a, l);
    if its.is_empty() {
        println!("{}: no originals to calibrate against", l.name);
        return None;
    }
    let mut two: HashSet<String> = HashSet::new();
    build_scene(gpu, l, masks, &two);
    let (pb, label, owner, nrm) = make_points(a, &its, masks);
    let n = label.len();
    if n == 0 || n > 4_000_000 {
        println!("{}: {n} points, skipped", l.name);
        return None;
    }
    let ps = gpu.points(&pb);
    // a thinned set for the coarse search
    let thin = (n / 150_000).max(1);
    let mut pb2 = vec![];
    let mut label2 = vec![];
    let mut nrm2 = vec![];
    for i in (0..n).step_by(thin) {
        pb2.extend_from_slice(&pb[i * 48..i * 48 + 48]);
        label2.push(label[i]);
        nrm2.push(nrm[i]);
    }
    let ps2 = gpu.points(&pb2);
    let push = |samples: u32, radius: f32, bias: f32, flags: u32| vk::Push {
        sun: [0.0, 1.0, 0.0],
        cone: radius.to_radians().tan(),
        size: 0,
        samples,
        bias,
        tmin: 0.002,
        row_start: 0,
        seed: 1,
        flags,
    };
    let t0 = std::time::Instant::now();
    let invs: Vec<bool> = match a.invert {
        Some(x) => vec![x],
        None => vec![false, true],
    };
    let flag_set: Vec<u32> = match a.flags {
        Some(f) => vec![f],
        None => vec![vk::FLAG_NO_NDL | vk::FLAG_CULL_BACK, 0, vk::FLAG_NO_NDL, vk::FLAG_NO_NDL | vk::FLAG_CULL_FRONT],
    };
    // (score, az, el, invert, flags)
    type Cand = (f64, f32, f32, bool, u32);
    let mut best: Cand = (0.0, 0.0, 45.0, invs[0], flag_set[0]);
    // scores every direction of `grid` with one flag set on the given points, keeps the best per polarity
    let search = |ps: &vk::PointSet, label: &[u8], nrm: &[[f32; 4]], grid: &[(f32, f32)], fl: u32, bias: f32| -> Vec<Cand> {
        let dirs: Vec<[f32; 3]> = grid.iter().map(|&(az, el)| sun_vec(az, el)).collect();
        let rows = gpu.trace_points(ps, &dirs, push(1, 0.0, bias, fl));
        let mut out = vec![];
        for (k, occ) in rows.iter().enumerate() {
            for &inv in &invs {
                out.push((score(occ, label, nrm, dirs[k], inv).bacc(), grid[k].0, grid[k].1, inv, fl));
            }
        }
        out
    };
    if let Some((az, el)) = a.sun {
        for &fl in &flag_set {
            for c in search(&ps, &label, &nrm, &[(az, el)], fl, a.bias) {
                if c.0 > best.0 {
                    best = c;
                }
            }
        }
    } else {
        // coarse grid on the thinned points with every flag set, then refine the best few on all points
        let mut coarse = vec![];
        let mut el = 4.0f32;
        while el <= 88.0 {
            let mut az = 0.0f32;
            while az < 360.0 {
                coarse.push((az, el));
                az += 4.0;
            }
            el += 4.0;
        }
        let mut grid: Vec<Cand> = vec![];
        for &fl in &flag_set {
            grid.extend(search(&ps2, &label2, &nrm2, &coarse, fl, a.bias));
        }
        grid.sort_by(|x, y| y.0.partial_cmp(&x.0).unwrap());
        let mut cands: Vec<Cand> = vec![];
        for g in &grid {
            if cands.len() >= 4 {
                break;
            }
            if !cands.iter().any(|c| c.3 == g.3 && c.4 == g.4 && (c.1 - g.1).abs() <= 8.0 && (c.2 - g.2).abs() <= 8.0) {
                cands.push(*g);
            }
        }
        for c0 in cands {
            let mut c = c0;
            c.0 = 0.0;
            for (span, step) in [(4.0f32, 1.0f32), (1.0, 0.25)] {
                let mut g = vec![];
                let steps = (2.0 * span / step).round() as i32;
                for i in 0..=steps {
                    for j in 0..=steps {
                        let e = c.2 - span + i as f32 * step;
                        let z = (c.1 - span + j as f32 * step).rem_euclid(360.0);
                        if e > 0.5 && e < 90.0 {
                            g.push((z, e));
                        }
                    }
                }
                for r in search(&ps, &label, &nrm, &g, c.4, a.bias) {
                    if r.3 == c.3 && r.0 > c.0 {
                        c = r;
                    }
                }
            }
            if c.0 > best.0 {
                best = c;
            }
        }
    }
    let (mut b0, az, el, inv, flags) = best;
    let sun = [sun_vec(az, el)];
    // with culling on: which models block light from behind as well (walls, single-sheet props); one greedy pass
    if flags & (vk::FLAG_CULL_BACK | vk::FLAG_CULL_FRONT) != 0 && a.fit_two_sided {
        let mut models: Vec<String> = l.objs.iter().filter(|o| !is_card(o, masks)).map(|o| o.model.clone()).collect();
        models.sort();
        models.dedup();
        models.retain(|m| !m.to_lowercase().contains("_sky") && !m.to_lowercase().contains("water"));
        std::env::set_var("HDS_QUIET", "1");
        for m in models {
            two.insert(m.clone());
            build_scene(gpu, l, masks, &two);
            let s = score(&gpu.trace_points(&ps, &sun, push(1, 0.0, a.bias, flags))[0], &label, &nrm, sun[0], inv).bacc();
            if s > b0 + 0.0005 {
                b0 = s;
            } else {
                two.remove(&m);
            }
        }
        std::env::remove_var("HDS_QUIET");
        build_scene(gpu, l, masks, &two);
    }
    // normal offset: largest agreement
    let mut bias = a.bias;
    let mut bb = b0;
    for b in [0.005f32, 0.01, 0.02, 0.05, 0.1, 0.2, 0.4] {
        let s = score(&gpu.trace_points(&ps, &sun, push(1, 0.0, b, flags))[0], &label, &nrm, sun[0], inv).bacc();
        if s > bb + 1e-4 {
            bb = s;
            bias = b;
        }
    }
    // penumbra: sun radius that best matches the soft values (mean absolute difference)
    let mut radius = a.radius_deg;
    let mut best_mad = f64::MAX;
    let mut rows = vec![];
    for r in [0.1f32, 0.27, 0.5, 1.0, 2.0] {
        let occ = gpu.trace_points(&ps, &sun, push(16, r, bias, flags)).remove(0);
        let (mut sum, mut cnt) = (0.0f64, 0u64);
        for i in 0..n {
            if faces(nrm[i], sun[0]) {
                let o = if inv { 255 - occ[i] } else { occ[i] } as f64;
                sum += (o - label[i] as f64).abs();
                cnt += 1;
            }
        }
        let mad = sum / cnt.max(1) as f64 / 255.0;
        rows.push(format!("{r}:{mad:.4}"));
        if mad < best_mad {
            best_mad = mad;
            radius = r;
        }
    }
    let occ = gpu.trace_points(&ps, &sun, push(1, 0.0, bias, flags)).remove(0);
    let all = score(&occ, &label, &nrm, sun[0], inv);
    println!(
        "SUN {}:{} az {:.2} el {:.2} invert {} flags {} bias {} radius {} | balanced accuracy {:.4} agreement {:.4} over {} points of {} maps ({:.1} s) | penumbra mad {}",
        course, l.name, az, el, inv as u8, flags, bias, radius, all.bacc(), all.agree(), n, its.len(), t0.elapsed().as_secs_f32(), rows.join(" ")
    );
    // per map and per model
    let mut per: Vec<Conf> = vec![Conf::default(); its.len()];
    for i in 0..n {
        if faces(nrm[i], sun[0]) {
            per[owner[i] as usize].add((occ[i] > 127) != inv, label[i] > 127);
        }
    }
    let mut models: HashMap<String, Conf> = HashMap::new();
    for (i, it) in its.iter().enumerate() {
        let m = models.entry(it.obj.model.clone()).or_default();
        m.tp += per[i].tp;
        m.tn += per[i].tn;
        m.fp += per[i].fp;
        m.fnn += per[i].fnn;
    }
    let mut mv: Vec<_> = models.into_iter().collect();
    mv.sort_by(|x, y| x.1.bacc().partial_cmp(&y.1.bacc()).unwrap());
    for (m, c) in &mv {
        println!("  model {:<40} bacc {:.3} agree {:.3} points {}", m, c.bacc(), c.agree(), c.total());
    }
    if let Some(rp) = &a.report {
        let mut f = std::fs::OpenOptions::new().create(true).append(true).open(rp).unwrap();
        for (i, it) in its.iter().enumerate() {
            let c = per[i];
            writeln!(f, "{}\t{}\t{}\t{}\t{:.4}\t{:.4}\t{}\t{}", course, l.name, it.file, it.w, c.bacc(), c.agree(), c.total(), is_card(it.obj, masks) as u8).ok();
        }
    }
    gpu.free_points(ps);
    gpu.free_points(ps2);
    let mut two: Vec<String> = two.into_iter().collect();
    two.sort();
    println!("  two-sided: {}", two.join(","));
    Some(Sun { key: format!("{course}:{}", l.name), az, el, radius, bias, invert: inv, flags, bacc: all.bacc(), two })
}

fn bake(a: &Args, gpu: &mut vk::Gpu, course: &str, l: &Level, masks: &mut MaskSet, sun: &Sun) {
    std::fs::create_dir_all(&a.out).unwrap();
    build_scene(gpu, l, masks, &sun.two.iter().cloned().collect());
    let sv = sun_vec(sun.az, sun.el);
    // the fitted radius includes the blur of the original's low resolution; an HD bake uses the real sun's size
    let tan_r = a.bake_radius.unwrap_or(0.27).to_radians().tan();
    let rp = a.report.clone().unwrap_or_else(|| a.out.join("report.tsv"));
    let mut rf = std::fs::OpenOptions::new().create(true).append(true).open(&rp).unwrap();
    let t_level = std::time::Instant::now();
    let mut done = 0;
    for (f, oi) in &l.shadows {
        let o = &l.objs[*oi];
        if a.only.as_ref().map_or(false, |x| !o.model.contains(x.as_str())) || o.uvs.is_empty() {
            continue;
        }
        let stem = f.trim_end_matches(".nutexb").to_string();
        let dst = a.out.join(format!("{stem}.png"));
        if a.skip_existing && dst.exists() {
            continue;
        }
        let orig = read_png_gray(&a.orig.join(format!("{stem}.png")));
        let osz = orig.as_ref().map(|x| x.0).unwrap_or(512);
        let size = (osz * a.scale).min(a.max_size).max(osz);
        let us = uvset_of(a, o);
        let two = is_card(o, masks);
        let tb = tri_table(o, us, a.flipv, two);
        let t0 = std::time::Instant::now();
        let tt = raster(&o.uvs[us].1, &o.idx, size, a.flipv, 2);
        let push = vk::Push { sun: sv, cone: tan_r, size, samples: a.samples, bias: sun.bias, tmin: 0.002, row_start: 0, seed: 1, flags: sun.flags };
        let rows = (4_000_000 / (size * a.samples.max(1))).clamp(8, size);
        let mut r = gpu.bake(&tb, &tt, size, push, rows);
        if sun.invert {
            for v in r.iter_mut() {
                *v = 255 - *v;
            }
        }
        let filled: Vec<bool> = tt.iter().map(|&t| t != u32::MAX).collect();
        dilate(&mut r, &filled, size, (size / osz.max(1)) * 4 + 4);
        // score: the bake shrunk to the original size against the original, on the texels the game shows lit by the
        // sun (covered, facing the sun, not on a see-through part of a card), as calib scores
        let (mut c, mut mad, mut nmad) = (Conf::default(), 0.0f64, 0u64);
        if let Some((w, _, g)) = &orig {
            let k = (size / w) as usize;
            let s = size as usize;
            for t in texels(a, o, *w, masks, &|_, _| true) {
                if !faces(t.nrm, sv) {
                    continue;
                }
                let (x, y) = (t.index % *w as usize, t.index / *w as usize);
                let mut sum = 0u32;
                for dy in 0..k {
                    for dx in 0..k {
                        sum += r[(y * k + dy) * s + x * k + dx] as u32;
                    }
                }
                let v = sum as f64 / (k * k) as f64;
                c.add(v > 127.5, g[t.index] > 127);
                mad += (v - g[t.index] as f64).abs() / 255.0;
                nmad += 1;
            }
        }
        write_png_gray(&dst, size, size, &r);
        writeln!(rf, "{}\t{}\t{}\t{}\t{}\t{:.4}\t{:.4}\t{:.4}\t{}\t{}", course, l.name, f, osz, size, c.bacc(), c.agree(), if nmad > 0 { mad / nmad as f64 } else { 0.0 }, c.total(), two as u8).ok();
        done += 1;
        if std::env::var("HDS_VERBOSE").is_ok() {
            println!("{} {}px bacc {:.3} agree {:.3} {} ms", stem, size, c.bacc(), c.agree(), t0.elapsed().as_millis());
        }
    }
    println!("BAKED {} {} maps in {:.1} s", l.name, done, t_level.elapsed().as_secs_f32());
}

fn main() {
    let a = parse_args();
    let mut root = course::DataRoot::locate(a.data.as_deref()).expect("data root");
    let courses = root.courses();
    let c = courses.iter().find(|c| c.name == a.course).unwrap_or_else(|| panic!("course {} not found", a.course)).clone();
    let levels = load_levels(&mut root, &c, a.level.as_deref());

    if a.cmd == "survey" {
        for l in &levels {
            let masks = load_masks(a.alpha.as_deref(), &l.objs, a.alpha_ref);
            let cards = l.objs.iter().filter(|o| is_card(o, &masks)).count();
            println!("{}: {} objects, {} with a Shadow texture, {} cut-out objects", l.name, l.objs.len(), l.shadows.len(), cards);
            let covered: HashSet<usize> = l.shadows.iter().map(|s| s.1).collect();
            if a.only.is_some() {
                for (i, o) in l.objs.iter().enumerate() {
                    if a.only.as_ref().map_or(true, |f| o.model.contains(f.as_str())) {
                        println!(
                            "  {} / {} #{} mat={} tris={} uv={:?} shadow={} card={}",
                            o.model, o.name, o.subindex, o.material, o.idx.len() / 3,
                            o.uvs.iter().map(|u| u.0.as_str()).collect::<Vec<_>>(), covered.contains(&i), is_card(o, &masks)
                        );
                    }
                }
            }
            println!("  shadow files with no mesh object: {}; objects without a shadow file: {}", l.unmatched.len(), l.objs.len() - covered.len());
            for m in l.unmatched.iter().take(10) {
                println!("    no object for {m}");
            }
        }
        return;
    }

    let mut gpu = vk::Gpu::new().expect("gpu");
    eprintln!("GPU: {}", gpu.name);
    let suns = a.sun_in.as_deref().map(read_suns).unwrap_or_default();
    let mut fitted = vec![];
    for l in &levels {
        let mut masks = load_masks(a.alpha.as_deref(), &l.objs, a.alpha_ref);
        match a.cmd.as_str() {
            "calib" => {
                if let Some(s) = calib(&a, &mut gpu, &c.name, l, &mut masks) {
                    fitted.push(s);
                }
            }
            "bake" => {
                let key = format!("{}:{}", c.name, l.name);
                let sun = if let Some((az, el)) = a.sun {
                    Sun { key, az, el, radius: a.radius_deg, bias: a.bias, invert: a.invert.unwrap_or(false), flags: a.flags.unwrap_or(0), bacc: 0.0, two: vec![] }
                } else if let Some(s) = suns.iter().find(|s| s.key == key) {
                    s.clone()
                } else {
                    eprintln!("{}: no sun (pass --sun or --sun-in with a calibrated level), skipped", l.name);
                    continue;
                };
                bake(&a, &mut gpu, &c.name, l, &mut masks, &sun);
            }
            x => panic!("unknown command {x}"),
        }
    }
    if let (Some(p), false) = (&a.sun_out, fitted.is_empty()) {
        write_suns(p, &fitted);
    }
}
