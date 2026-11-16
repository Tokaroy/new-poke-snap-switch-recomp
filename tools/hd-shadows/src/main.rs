// HD shadows bake tool. Reads the player's own unpacked game data (extracted/drp-v262144), never writes to it.
// Output: 8-bit sun-shadow masks (PNG, one per game Shadow texture) in the folder given by --out, which the packer
// (scripts/hd/hdshadows.py) turns into nutexb files for a local mod pack. Nothing here is committed or distributed.
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
mod scene;
mod vk;

use std::collections::HashMap;
use std::path::{Path, PathBuf};

struct Args {
    cmd: String,
    course: String,
    variant: u32,
    out: PathBuf,
    orig: PathBuf,
    scale: u32,
    samples: u32,
    sun: Option<(f32, f32)>, // azimuth, elevation degrees
    radius_deg: f32,
    uvset: Option<usize>,
    flipv: bool,
    invert: bool,
    only: Option<String>,
    max_size: u32,
    data: Option<PathBuf>,
    step: usize,
    az_range: (i32, i32),
    el_range: (i32, i32),
}

fn parse_args() -> Args {
    let mut a = Args {
        cmd: "survey".into(),
        course: "LC_001_A".into(),
        variant: 1,
        out: PathBuf::from("hdshadows_out"),
        orig: PathBuf::from("hdshadows_orig"),
        scale: 2,
        samples: 32,
        sun: None,
        radius_deg: 1.0,
        uvset: None,
        flipv: false,
        invert: false,
        only: None,
        max_size: 4096,
        data: None,
        step: 10,
        az_range: (0, 360),
        el_range: (10, 90),
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
            "--variant" => a.variant = val().parse().unwrap_or(1),
            "--out" => a.out = val().into(),
            "--orig" => a.orig = val().into(),
            "--scale" => a.scale = val().parse().unwrap_or(2),
            "--samples" => a.samples = val().parse().unwrap_or(32),
            "--sun" => {
                let s = val();
                let p: Vec<f32> = s.split(',').filter_map(|x| x.parse().ok()).collect();
                if p.len() == 2 {
                    a.sun = Some((p[0], p[1]));
                }
            }
            "--radius" => a.radius_deg = val().parse().unwrap_or(1.0),
            "--uv" => a.uvset = val().parse().ok(),
            "--flipv" => a.flipv = true,
            "--invert" => a.invert = true,
            "--only" => a.only = Some(val()),
            "--max-size" => a.max_size = val().parse().unwrap_or(4096),
            "--data" => a.data = Some(val().into()),
            "--step" => a.step = val().parse().unwrap_or(10),
            "--az" | "--el" => {
                let r: Vec<i32> = val().split(',').filter_map(|x| x.parse().ok()).collect();
                if r.len() == 2 {
                    if k == "--az" { a.az_range = (r[0], r[1]) } else { a.el_range = (r[0], r[1]) }
                }
            }
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
    let f = std::fs::File::create(path).unwrap();
    let mut e = png::Encoder::new(std::io::BufWriter::new(f), w, h);
    e.set_color(png::ColorType::Grayscale);
    e.set_depth(png::BitDepth::Eight);
    e.set_compression(png::Compression::Fast);
    e.write_header().unwrap().write_image_data(data).unwrap();
}

/// Triangle index per texel (0xFFFFFFFF = none): centre sampling, then a 2 texel bleed so filtering never reads a gap.
fn raster(uv: &[[f32; 2]], idx: &[u32], tri_ids: &[u32], size: u32, flipv: bool) -> Vec<u32> {
    let s = size as i64;
    let mut out = vec![u32::MAX; (size * size) as usize];
    let fs = size as f32;
    for (ti, &tid) in tri_ids.iter().enumerate() {
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
                    out[(y * s + x) as usize] = tid;
                }
            }
        }
    }
    // bleed
    for _ in 0..2 {
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

struct TriTable {
    bytes: Vec<u8>,
    idx: Vec<u32>,
}

/// Triangle table in the layout of bake.comp (Tri: 8 x vec4 = 128 bytes).
fn tri_table(o: &scene::Obj, uvset: usize, flipv: bool) -> (Vec<u8>, Vec<u32>) {
    let uv = &o.uvs[uvset].1;
    let n = o.idx.len() / 3;
    let mut bytes = Vec::with_capacity(n * 128);
    let f = |x: f32| x.to_le_bytes();
    for t in 0..n {
        let ids = [o.idx[t * 3] as usize, o.idx[t * 3 + 1] as usize, o.idx[t * 3 + 2] as usize];
        for i in ids {
            for c in o.pos[i] {
                bytes.extend_from_slice(&f(c));
            }
            bytes.extend_from_slice(&f(0.0));
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
    (bytes, o.idx.clone())
}

fn sun_vec(az: f32, el: f32) -> [f32; 3] {
    let (az, el) = (az.to_radians(), el.to_radians());
    [el.cos() * az.cos(), el.sin(), el.cos() * az.sin()]
}

struct Level {
    name: String,
    objs: Vec<scene::Obj>,
    /// shadow texture file stem (without ^Shadow.nutexb) -> object index
    shadows: Vec<(String, usize, u32, u32)>, // file, obj, orig w, orig h (from the python dump when available)
}

fn main() {
    let a = parse_args();
    let mut root = course::DataRoot::locate(a.data.as_deref()).expect("data root");
    let courses = root.courses();
    let c = courses.iter().find(|c| c.name == a.course).unwrap_or_else(|| panic!("course {} not found", a.course)).clone();
    let lc = root.load(&c, a.variant).expect("load course");

    // shadow texture files of the pack
    let mut files: HashMap<(String, String, String, u64), String> = HashMap::new();
    for e in std::fs::read_dir(&c.dir).unwrap().flatten() {
        let n = e.file_name().to_string_lossy().to_string();
        if let Some((lv, m, s, sub)) = parse_shadow_name(&n) {
            files.insert((lv, m, s, sub), n);
        }
    }

    let mut levels: Vec<Level> = vec![];
    for (li, l) in lc.levels.iter().enumerate() {
        if !l.name.starts_with("LV_ENV") {
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
        let mut shadows = vec![];
        for (i, o) in objs.iter().enumerate() {
            if let Some(f) = files.get(&(l.name.clone(), o.model.clone(), o.name.clone(), o.subindex)) {
                shadows.push((f.clone(), i, 0, 0));
            }
        }
        levels.push(Level { name: l.name.clone(), objs, shadows });
    }

    if a.cmd == "survey" {
        for l in &levels {
            println!("{}: {} objects, {} with a Shadow texture", l.name, l.objs.len(), l.shadows.len());
            for o in l.objs.iter().take(400) {
                let has = l.shadows.iter().any(|s| std::ptr::eq(&l.objs[s.1], o));
                if a.only.as_ref().map_or(true, |f| o.model.contains(f.as_str())) {
                    println!(
                        "  {} / {} #{} mat={} tris={} uv={:?} shadow={}",
                        o.model, o.name, o.subindex, o.material, o.idx.len() / 3,
                        o.uvs.iter().map(|u| u.0.as_str()).collect::<Vec<_>>(), has
                    );
                }
            }
            let covered: std::collections::HashSet<usize> = l.shadows.iter().map(|s| s.1).collect();
            let missing: Vec<_> = files.keys().filter(|k| k.0 == l.name && !l.objs.iter().any(|o| o.model == k.1 && o.name == k.2 && o.subindex == k.3)).collect();
            println!("  objects with a Shadow texture: {}; shadow files with no mesh object: {}; unreferenced objs: {}", covered.len(), missing.len(), l.objs.len() - covered.len());
            for m in missing.iter().take(10) {
                println!("    no object for {:?}", m);
            }
        }
        return;
    }

    let mut gpu = vk::Gpu::new().expect("gpu");
    eprintln!("GPU: {}", gpu.name);
    let uvset = |o: &scene::Obj| -> usize { a.uvset.unwrap_or(o.uvs.len().saturating_sub(1)).min(o.uvs.len().saturating_sub(1)) };

    for l in &levels {
        // scene: every lightmapped model except sky and water
        let mut verts: Vec<[f32; 3]> = vec![];
        for o in &l.objs {
            let m = o.model.to_lowercase();
            if m.contains("_sky") || m.contains("water") {
                continue;
            }
            for &i in &o.idx {
                verts.push(o.pos[i as usize]);
            }
        }
        eprintln!("{}: {} triangles in the ray scene", l.name, verts.len() / 3);
        gpu.set_scene(&verts).expect("scene");

        let (az, el) = a.sun.unwrap_or((0.0, 60.0));
        let tan_r = a.radius_deg.to_radians().tan();
        let push = |sun: [f32; 3], samples: u32, size: u32| vk::Push { sun, cone: tan_r, size, samples, bias: 0.03, tmin: 0.002, row_start: 0, seed: 1 };

        if a.cmd == "calib" {
            // compare a bake at the game's own resolution with the original masks over a sun direction grid
            let mut items = vec![];
            for (f, oi, _, _) in &l.shadows {
                let stem = f.trim_end_matches(".nutexb").to_string();
                let Some((w, h, g)) = read_png_gray(&a.orig.join(format!("{stem}.png"))) else { continue };
                if w != h || w > 1024 || w < 128 {
                    continue;
                }
                if a.only.as_ref().map_or(false, |f| !l.objs[*oi].model.contains(f.as_str())) {
                    continue;
                }
                let o = &l.objs[*oi];
                if o.uvs.is_empty() {
                    continue;
                }
                items.push((o, w, g));
            }
            println!("{} calibration masks", items.len());
            for flip in if a.flipv { vec![true] } else { vec![false] } {
                let mut prep = vec![];
                for (o, w, g) in &items {
                    let us = uvset(o);
                    let (tb, idx) = tri_table(o, us, flip);
                    let ids: Vec<u32> = (0..(idx.len() / 3) as u32).collect();
                    let tt = raster(&o.uvs[us].1, &o.idx, &ids, *w, flip);
                    prep.push((tb, tt, *w, g, idx.len()));
                }
                let mut best = (0.0f64, 0.0f32, 0.0f32);
                for azd in (a.az_range.0..a.az_range.1).step_by(a.step) {
                    for eld in (a.el_range.0..=a.el_range.1).step_by(a.step) {
                        let sun = sun_vec(azd as f32, eld as f32);
                        let (mut tp, mut tn, mut fp, mut fnn) = (0u64, 0u64, 0u64, 0u64);
                        for (tb, tt, w, g, _) in &prep {
                            let r = gpu.bake(tb, tt, *w, push(sun, 1, *w), 1024);
                            for i in 0..r.len() {
                                if tt[i] == u32::MAX {
                                    continue;
                                }
                                match (r[i] > 127, g[i] > 127) {
                                    (true, true) => tp += 1,
                                    (false, false) => tn += 1,
                                    (true, false) => fp += 1,
                                    (false, true) => fnn += 1,
                                }
                            }
                        }
                        // balanced accuracy (1 = identical, 0.5 = chance); report the better polarity
                        let tpr = tp as f64 / ((tp + fnn).max(1)) as f64;
                        let tnr = tn as f64 / ((tn + fp).max(1)) as f64;
                        let s = 0.5 * (tpr + tnr);
                        let s2 = s.max(1.0 - s);
                        if s2 > best.0 {
                            best = (s2, azd as f32, eld as f32);
                        }
                    }
                }
                println!("flipv={flip}: best balanced accuracy {:.4} at az {} el {}", best.0, best.1, best.2);
            }
            continue;
        }

        if a.cmd == "bake" {
            std::fs::create_dir_all(&a.out).unwrap();
            let sun = sun_vec(az, el);
            for (f, oi, _, _) in &l.shadows {
                let o = &l.objs[*oi];
                if a.only.as_ref().map_or(false, |x| !o.model.contains(x.as_str())) || o.uvs.is_empty() {
                    continue;
                }
                let stem = f.trim_end_matches(".nutexb").to_string();
                let osz = read_png_gray(&a.orig.join(format!("{stem}.png"))).map(|x| x.0).unwrap_or(512);
                let size = (osz * a.scale).min(a.max_size);
                let us = uvset(o);
                let (tb, idx) = tri_table(o, us, a.flipv);
                let ids: Vec<u32> = (0..(idx.len() / 3) as u32).collect();
                let t0 = std::time::Instant::now();
                let tt = raster(&o.uvs[us].1, &o.idx, &ids, size, a.flipv);
                let mut r = gpu.bake(&tb, &tt, size, push(sun, a.samples, size), 256);
                if a.invert {
                    for v in r.iter_mut() {
                        *v = 255 - *v;
                    }
                }
                write_png_gray(&a.out.join(format!("{stem}.png")), size, size, &r);
                println!("{} {}px {} tris {} ms", stem, size, idx.len() / 3, t0.elapsed().as_millis());
            }
        }
    }
}
