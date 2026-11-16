//! New Pokémon Snap recomp level editor.
//!
//! level-editor [--data <extracted/drp-v262144>] [--course LC_001_A] [--variant 1]
//!              [--select L:I] [--move L:I:dx,dy,dz]... [--export DIR --pack NAME]
//!              [--shot out.png [--size 1600x900] [--advanced]] [--summary] [--selftest]
//!
//! Without --shot, --summary or --selftest it opens a window. --shot renders the same UI offscreen and writes a PNG
//! (no window). --move edits spawn I of level L (indices as in the Pokémon list), --export writes the changed levels
//! as a mod pack under DIR. --selftest checks that export is byte-identical without edits and exact with edits.
//! Nothing is ever written to the data folder.

mod app;
mod course;
mod edit;
mod fb;
mod geom;
mod gizmo;
mod headless;
mod grow;
mod layout;
mod level;
mod pick;
mod render;

use edit::Sel;
use glam::Vec3;
use std::path::PathBuf;

struct Args {
    data: Option<PathBuf>,
    course: Option<String>,
    variant: u32,
    shot: Option<PathBuf>,
    size: (u32, u32),
    summary: bool,
    selftest: bool,
    advanced: bool,
    select: Option<(usize, usize)>,
    moves: Vec<(usize, usize, Vec3)>,
    export: Option<PathBuf>,
    pack: Option<String>,
    rail_view: Option<f32>,
    layout: bool,
    rail_info: bool,
    rail_bends: Vec<(usize, Vec3, usize)>,
    model_adds: Vec<(String, Vec3)>,
    model_moves: Vec<(String, Vec3)>,
    spawn_copies: Vec<(usize, usize, Vec3)>,
}

fn parse_vec3(d: &str) -> Result<Vec3, String> {
    let f: Vec<f32> = d.split(',').map(|x| x.parse::<f32>()).collect::<Result<_, _>>().map_err(|_| "bad numbers")?;
    if f.len() != 3 {
        return Err("expected dx,dy,dz".into());
    }
    Ok(Vec3::new(f[0], f[1], f[2]))
}

/// Clones named model instances (offset by a delta), checks the grown files and exports them as a mod pack.
fn add_models(data: &mut course::DataRoot, name: &str, variant: u32, adds: &[(String, Vec3)], dir: &std::path::Path, pack: &str) -> Result<bool, String> {
    let c = data.courses().into_iter().find(|c| c.name.eq_ignore_ascii_case(name)).ok_or("course not found")?;
    let mut lc = data.load(&c, variant)?;
    let mut per_level: std::collections::BTreeMap<usize, Vec<grow::Add>> = Default::default();
    for (model, d) in adds {
        let mut found = None;
        'outer: for (li, l) in lc.levels.iter().enumerate() {
            let root = fb::Buf(&l.data).root().ok_or("bad level")?;
            for (ei, e) in root.tables(2).iter().enumerate() {
                for comp in e.tables(0) {
                    if let Some(t) = comp.table(5) {
                        if t.str(1).is_some_and(|n| n.eq_ignore_ascii_case(model)) {
                            let p = t.floats::<3>(3).ok_or("model without position")?;
                            found = Some((li, ei, Vec3::from(p) + *d));
                            break 'outer;
                        }
                    }
                }
            }
        }
        let (li, ei, p) = found.ok_or_else(|| format!("no model instance named {model}"))?;
        println!("clone {model} (level {li}, entity {ei}) to {p}");
        per_level.entry(li).or_default().push(grow::Add { entity: ei, set: vec![(5, 3, p.to_array().to_vec())] });
    }
    for (li, a) in &per_level {
        let l = &mut lc.levels[*li];
        let (out, ids) = grow::grow(&l.data, a)?;
        println!("{}: {}", l.name, grow::verify(&l.data, &out, a)?);
        for id in &ids {
            println!("  new entity {:08x} node {:08x?}", id.hash, id.node_id);
        }
        let re = level::parse(&out, &l.file)?;
        println!("  re-parse: {} models (was {}), {} spawns (was {})", re.models.len(), l.models.len(), re.spawns.len(), l.spawns.len());
        l.data = out;
        l.dirty = true;
    }
    let files = edit::export(&lc, dir, pack, "Level editor test: added objects", false)?;
    println!("exported {} file(s) to {}", files.len(), dir.join(pack).display());
    Ok(true)
}

/// Camera position and heading at distance `d` along rail segment 0, and the models in front of it.
fn rail_view(data: &mut course::DataRoot, name: &str, variant: u32, d: f32) -> Result<(), String> {
    let c = data.courses().into_iter().find(|c| c.name.eq_ignore_ascii_case(name)).ok_or("course not found")?;
    let lc = data.load(&c, variant)?;
    let seg = &lc.rail().ok_or("no rail")?.segments[0];
    let mut left = d;
    let (mut at, mut fwd) = (Vec3::from(seg[0]), Vec3::Z);
    for w in seg.windows(2) {
        let (a, b) = (Vec3::from(w[0]), Vec3::from(w[1]));
        let l = a.distance(b);
        fwd = (b - a).normalize_or_zero();
        if left <= l {
            at = a + fwd * left;
            break;
        }
        left -= l;
        at = b;
    }
    println!("rail at {d}: pos {at:.2} heading {fwd:.2}");
    let mut rows = vec![];
    for m in &lc.models {
        let Some(mesh) = &m.mesh else { continue };
        let q = glam::Quat::from_array(m.inst.rot);
        let q = if q.length_squared() < 1e-6 { glam::Quat::IDENTITY } else { q.normalize() };
        let xf = glam::Mat4::from_scale_rotation_translation(Vec3::from(m.inst.scale), q, Vec3::from(m.inst.pos));
        let (mut mn, mut mx) = (Vec3::splat(f32::MAX), Vec3::splat(f32::MIN));
        for p in &mesh.positions {
            let w = xf.transform_point3(Vec3::from(*p));
            mn = mn.min(w);
            mx = mx.max(w);
        }
        let ctr = (mn + mx) * 0.5;
        let to = ctr - at;
        let flat = Vec3::new(to.x, 0.0, to.z);
        let ang = Vec3::new(fwd.x, 0.0, fwd.z).normalize_or_zero().angle_between(flat.normalize_or_zero()).to_degrees();
        rows.push((flat.length(), ang, m.inst.name.clone(), mx - mn, ctr));
    }
    rows.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap());
    for (dist, ang, n, size, ctr) in rows.iter().filter(|r| r.1 < 40.0 && r.0 < 90.0) {
        println!("  {n:<40} dist {dist:6.1} angle {ang:5.1}  size {size:.1}  center {ctr:.1}");
    }
    Ok(())
}

/// Everything stored on the rail path component: scalars, points, spacing, turn angles and per-point records.
fn rail_info(data: &mut course::DataRoot, name: &str, variant: u32) -> Result<(), String> {
    let c = data.courses().into_iter().find(|c| c.name.eq_ignore_ascii_case(name)).ok_or("course not found")?;
    let lc = data.load(&c, variant)?;
    for l in &lc.levels {
        let Some(rail) = l.rail else { continue };
        let root = fb::Buf(&l.data).root().ok_or("bad level")?;
        // also list every other entity that points at the rail node
        let rail_nodes: Vec<u32> = root.tables(3).iter().filter(|n| n.u32(2) == Some(rail)).filter_map(|n| n.u32(1)).collect();
        println!("{}: rail entity {rail:08x}, nodes {rail_nodes:08x?}", l.name);
        if let Some(p) = root.table(6) {
            println!("  course params: slots {}", p.slot_count());
            for s in 0..p.slot_count() {
                if let Some(o) = p.field(s) {
                    println!("    s{s} @{o:#x} u32 {:?} f32 {:?}", p.u32(s), p.f32(s));
                }
            }
        }
        for e in root.tables(2).iter().filter(|e| e.u32(1) == Some(rail)) {
            for comp in e.tables(0) {
                let Some((slot, t)) = (1..comp.slot_count()).find_map(|s| comp.table(s).map(|t| (s, t))) else { continue };
                println!("  component kind {slot}: slots {}", t.slot_count());
                if slot != 8 {
                    continue;
                }
                for s in 2..t.slot_count() {
                    if t.has(s) {
                        println!("    s{s}: u8 {:?} u32 {:?} f32 {:?}", t.u8(s), t.u32(s), t.f32(s));
                    }
                }
                for (si, seg) in t.tables(1).iter().enumerate() {
                    let pts = seg.vec3s(1);
                    let evs = seg.tables(2);
                    let names: Vec<String> = match seg.vec(3) {
                        Some((p0, n)) => (0..n).filter_map(|i| fb::Buf(&l.data).u32_at(p0 + 4 * i).map(|r| p0 + 4 * i + r as usize)).map(|sp| {
                            let len = fb::Buf(&l.data).u32_at(sp).unwrap_or(0) as usize;
                            String::from_utf8_lossy(&l.data[sp + 4..sp + 4 + len]).into_owned()
                        }).collect(),
                        None => vec![],
                    };
                    println!("    segment {si}: s0 {:?}, {} points, {} point records, {} names", seg.u32(0), pts.len(), evs.len(), names.len());
                    let mut total = 0.0;
                    for (i, p) in pts.iter().enumerate() {
                        let step = if i > 0 { level::dist(pts[i - 1], *p) } else { 0.0 };
                        total += step;
                        let turn = if i > 0 && i + 1 < pts.len() {
                            let a = Vec3::from(*p) - Vec3::from(pts[i - 1]);
                            let b = Vec3::from(pts[i + 1]) - Vec3::from(*p);
                            a.angle_between(b).to_degrees()
                        } else {
                            0.0
                        };
                        let recs: Vec<String> = evs.get(i).map(|ev| ev.tables(0).iter().map(|r| {
                            let inner: Vec<String> = r.tables(0).iter().map(|x| format!("{:?}/{:?}/{:?}", x.u32(0), x.u8(1), x.u32(2))).collect();
                            format!("{{s1 {:?} [{}]}}", r.u32(1), inner.join(" "))
                        }).collect()).unwrap_or_default();
                        println!("      {i:3} ({:8.2},{:7.2},{:8.2}) step {step:5.2} at {total:6.1} turn {turn:5.1} {} {}", p[0], p[1], p[2], names.get(i).map(String::as_str).unwrap_or(""), recs.join(" "));
                    }
                }
            }
        }
    }
    Ok(())
}

fn parse_li(s: &str) -> Result<(usize, usize), String> {
    let (l, i) = s.split_once(':').ok_or("expected L:I")?;
    Ok((l.parse().map_err(|_| "bad level index")?, i.parse().map_err(|_| "bad spawn index")?))
}

fn parse_args() -> Result<Args, String> {
    let mut a = Args {
        data: None,
        course: None,
        variant: 1,
        shot: None,
        size: (1600, 900),
        summary: false,
        selftest: false,
        advanced: false,
        select: None,
        moves: vec![],
        export: None,
        pack: None,
        rail_view: None,
        layout: false,
        rail_info: false,
        rail_bends: vec![],
        model_adds: vec![],
        spawn_copies: vec![],
        model_moves: vec![],
    };
    let mut it = std::env::args().skip(1);
    while let Some(k) = it.next() {
        let mut val = || it.next().ok_or(format!("{k} needs a value"));
        match k.as_str() {
            "--data" => a.data = Some(val()?.into()),
            "--course" => a.course = Some(val()?),
            "--variant" => a.variant = val()?.parse().map_err(|_| "--variant: number expected")?,
            "--shot" => a.shot = Some(val()?.into()),
            "--size" => {
                let v = val()?;
                let (w, h) = v.split_once('x').ok_or("--size WxH")?;
                a.size = (w.parse().map_err(|_| "--size WxH")?, h.parse().map_err(|_| "--size WxH")?);
            }
            "--summary" => a.summary = true,
            "--selftest" => a.selftest = true,
            "--advanced" => a.advanced = true,
            "--select" => a.select = Some(parse_li(&val()?)?),
            "--move" => {
                let v = val()?;
                let (li, d) = v.rsplit_once(':').ok_or("--move L:I:dx,dy,dz")?;
                let (l, i) = parse_li(li)?;
                let f: Vec<f32> = d.split(',').map(|x| x.parse::<f32>()).collect::<Result<_, _>>().map_err(|_| "--move: bad numbers")?;
                if f.len() != 3 {
                    return Err("--move L:I:dx,dy,dz".into());
                }
                a.moves.push((l, i, Vec3::new(f[0], f[1], f[2])));
            }
            "--layout" => a.layout = true,
            "--rail-info" => a.rail_info = true,
            "--bend-rail" => {
                // I:dx,dy,dz[:soft]
                let v = val()?;
                let mut it = v.split(':');
                let i = it.next().and_then(|x| x.parse().ok()).ok_or("--bend-rail I:dx,dy,dz[:soft]")?;
                let d = parse_vec3(it.next().ok_or("--bend-rail I:dx,dy,dz[:soft]")?)?;
                let soft = it.next().map(|x| x.parse().map_err(|_| "--bend-rail: bad soft count")).transpose()?.unwrap_or(edit::RAIL_SOFT);
                a.rail_bends.push((i, d, soft));
            }
            "--copy-spawn" => {
                let v = val()?;
                let (li, d) = v.rsplit_once(':').ok_or("--copy-spawn L:I:x,y,z")?;
                let (l, i) = parse_li(li)?;
                a.spawn_copies.push((l, i, parse_vec3(d)?));
            }
            "--add-model" => {
                let v = val()?;
                let (n, d) = v.split_once(':').ok_or("--add-model NAME:dx,dy,dz")?;
                a.model_adds.push((n.to_string(), parse_vec3(d)?));
            }
            "--rail-view" => a.rail_view = Some(val()?.parse().map_err(|_| "--rail-view: number expected")?),
            "--move-model" => {
                let v = val()?;
                let (n, d) = v.split_once(':').ok_or("--move-model NAME:dx,dy,dz")?;
                a.model_moves.push((n.to_string(), parse_vec3(d)?));
            }
            "--export" => a.export = Some(val()?.into()),
            "--pack" => a.pack = Some(val()?),
            "-h" | "--help" => {
                println!("level-editor [--data DIR] [--course LC_001_A] [--variant 1] [--select L:I] [--move L:I:dx,dy,dz] [--export DIR --pack NAME] [--shot out.png] [--size WxH] [--advanced] [--summary] [--selftest]");
                std::process::exit(0);
            }
            _ => return Err(format!("unknown argument {k}")),
        }
    }
    Ok(a)
}

fn print_summary(data: &mut course::DataRoot, name: &str, variant: u32) -> Result<(), String> {
    let c = data.courses().into_iter().find(|c| c.name.eq_ignore_ascii_case(name)).ok_or("course not found")?;
    let lc = data.load(&c, variant)?;
    println!("{} leg {} ({} legs, loaded in {} ms)", lc.course, lc.variant, lc.variants.len(), lc.load_ms);
    if let Some(v) = lc.variants.iter().find(|v| v.no == lc.variant) {
        for r in &v.levels {
            println!("  level ref {:08x} -> {}", r.hash, r.name.as_deref().unwrap_or("(unresolved)"));
        }
    }
    for (li, l) in lc.levels.iter().enumerate() {
        println!(
            "[{li}] {} [{}]: {} bytes, {} entities, {} nodes, {} models, {} paths, {} spawns, {} templates",
            l.name,
            l.kind(),
            l.size,
            l.entity_count,
            l.node_count,
            l.models.len(),
            l.paths.len(),
            l.spawns.len(),
            l.templates.len()
        );
        for (k, n) in &l.component_kinds {
            println!("    component {k}: {n}");
        }
        if !l.species.is_empty() {
            println!("    species: {:?}", l.species);
        }
        let own = l.spawns.iter().filter(|s| !s.pos_from_path && s.pos_off.is_some()).count();
        let route = l.spawns.iter().filter(|s| s.pos_from_path && s.path_idx.is_some()).count();
        if !l.spawns.is_empty() {
            println!("    spawns movable: {own} by own position, {route} by route, {} fixed", l.spawns.len() - own - route);
        }
        let mm = l.models.iter().filter(|m| m.pos_off.is_some()).count();
        if !l.models.is_empty() {
            println!("    models movable: {mm} of {}", l.models.len());
        }
    }
    for e in &lc.level_errors {
        println!("  error: {e}");
    }
    match lc.rail() {
        Some(r) => {
            for i in 0..r.segments.len() {
                println!("rail segment {i}: {} points, {:.1} units", r.segments[i].len(), r.length(i));
            }
        }
        None => println!("rail: not found"),
    }
    let resolved = lc.models.iter().filter(|m| m.mesh.is_some()).count();
    let tris: usize = lc.models.iter().filter_map(|m| m.mesh.as_ref()).map(|m| m.indices.len() / 3).sum();
    println!("model instances: {} ({} resolved, {} triangles)", lc.models.len(), resolved, tris);
    for m in lc.models.iter().filter(|m| m.error.is_some()) {
        println!("  unresolved {}: {}", m.inst.name, m.error.as_deref().unwrap_or(""));
    }
    let mut by_species: std::collections::BTreeMap<&str, usize> = Default::default();
    for l in &lc.levels {
        for s in &l.spawns {
            *by_species.entry(s.label.as_str()).or_default() += 1;
        }
    }
    println!("spawns by species: {by_species:?}");
    Ok(())
}

/// Export round trip: unedited = byte-identical, edited = only the edited floats change, undo = original again.
fn selftest(data: &mut course::DataRoot, name: &str, variant: u32) -> Result<bool, String> {
    let c = data.courses().into_iter().find(|c| c.name.eq_ignore_ascii_case(name)).ok_or("course not found")?;
    let mut lc = data.load(&c, variant)?;
    let out = std::env::current_dir().map_err(|e| e.to_string())?.join("target").join("selftest");
    let _ = std::fs::remove_dir_all(&out);
    let mut ok = true;
    let mut check = |name: &str, pass: bool, detail: String| {
        println!("{} {name}: {detail}", if pass { "PASS" } else { "FAIL" });
        ok &= pass;
    };

    // 1. unedited export of every level
    let files = edit::export(&lc, &out, "roundtrip", "selftest", true)?;
    for l in &lc.levels {
        let fname = std::path::Path::new(&l.file).file_name().unwrap();
        let f = files.iter().find(|p| p.file_name() == Some(fname)).ok_or("exported file missing")?;
        let a = std::fs::read(f).map_err(|e| e.to_string())?;
        let b = std::fs::read(&l.file).map_err(|e| e.to_string())?;
        check(&format!("unedited {}", l.name), a == b, format!("{} bytes, identical = {}", a.len(), a == b));
    }

    // 2. move one spawn with its own position and one that walks a route
    let li = lc.levels.iter().position(|l| !l.spawns.is_empty()).ok_or("no spawns")?;
    let own = lc.levels[li].spawns.iter().position(|s| !s.pos_from_path && s.pos_off.is_some()).ok_or("no own-position spawn")?;
    let route = lc.levels[li].spawns.iter().position(|s| s.pos_from_path && s.path_idx.is_some()).ok_or("no route spawn")?;
    let (own_before, route_before) = (lc.levels[li].spawns[own].pos, lc.levels[li].spawns[route].pos);
    let pidx = lc.levels[li].spawns[route].path_idx.unwrap();
    let route_pts: usize = lc.levels[li].paths[pidx].segments.iter().map(|s| s.len()).sum();
    let mut h = edit::History::default();
    let w = edit::move_by(&lc, Sel::Spawn(li, own), Vec3::new(5.0, 0.0, 0.0))?;
    h.apply(&mut lc, "move own", None, w);
    let w = edit::move_by(&lc, Sel::Spawn(li, route), Vec3::new(0.0, 0.0, 5.0))?;
    h.apply(&mut lc, "move route", None, w);
    let yaw_target = 90f32.to_radians();
    let has_rot = edit::set_yaw(&lc, Sel::Spawn(li, own), yaw_target);
    if let Some(w) = has_rot.clone() {
        h.apply(&mut lc, "turn", None, w);
    }
    let files = edit::export(&lc, &out, "edited", "selftest", false)?;
    let levelbs = files.iter().filter(|f| f.extension().is_some_and(|e| e == "levelb")).count();
    check("only changed levels exported", levelbs == 1, format!("{levelbs} levelb of {} files", files.len()));
    let mj = std::fs::read_to_string(out.join("mods.json")).unwrap_or_default();
    check("pack switched on in mods.json", mj.contains("\"edited\"") && mj.contains("\"enabled\": true"), mj.replace('\n', " "));
    let l = &lc.levels[li];
    let fname = std::path::Path::new(&l.file).file_name().unwrap();
    let f = files.iter().find(|p| p.file_name() == Some(fname)).ok_or("edited level not exported")?;
    let bytes = std::fs::read(f).map_err(|e| e.to_string())?;
    let orig = std::fs::read(&l.file).map_err(|e| e.to_string())?;
    let diff = bytes.iter().zip(&orig).filter(|(a, b)| a != b).count();
    let max_diff = 12 + 12 * route_pts + if has_rot.is_some() { 16 } else { 0 };
    check("same size", bytes.len() == orig.len(), format!("{} bytes", bytes.len()));
    check("only edited floats differ", diff > 0 && diff <= max_diff, format!("{diff} bytes differ (at most {max_diff})"));
    let re = level::parse(&bytes, &l.file)?;
    let a = re.spawns[own].pos;
    let want = [own_before[0] + 5.0, own_before[1], own_before[2]];
    check(
        &format!("spawn {li}:{own} {} moved +5 X", re.spawns[own].label),
        level::dist(a, want) < 1e-3,
        format!("{own_before:?} -> {a:?}"),
    );
    let b = re.spawns[route].pos;
    let want = [route_before[0], route_before[1], route_before[2] + 5.0];
    check(
        &format!("spawn {li}:{route} {} route moved +5 Z ({route_pts} points)", re.spawns[route].label),
        level::dist(b, want) < 1e-3,
        format!("{route_before:?} -> {b:?}"),
    );
    if has_rot.is_some() {
        let y = edit::yaw_of(re.spawns[own].rot).to_degrees();
        check("spawn turned to 90 degrees", (y - 90.0).abs() < 0.01, format!("{y:.3} degrees"));
    }
    let others_same = re.spawns.iter().enumerate().filter(|(i, _)| *i != own && *i != route).all(|(i, s)| {
        let shares = lc.levels[li].spawns[i].path_idx == Some(pidx);
        shares || s.pos == lc.levels[li].spawns[i].pos
    });
    check("other spawns unchanged", others_same, String::new());

    // 3. undo everything
    while h.undo(&mut lc).is_some() {}
    check("undo restores the original", lc.levels.iter().all(|l| l.data == l.orig && !l.dirty), String::new());
    let back = (lc.levels[li].spawns[own].pos, lc.levels[li].spawns[route].pos);
    check("undo restores positions", back == (own_before, route_before), format!("{back:?}"));

    // Adding: copy one model instance and one own-position spawn, export, re-parse.
    let mi = lc.models.iter().position(|m| m.inst.pos_off.is_some()).ok_or("no movable model")?;
    let (ml, n_models, n_spawns) = (lc.models[mi].level_idx, lc.levels[lc.models[mi].level_idx].models.len(), lc.levels[li].spawns.len());
    let mpos = Vec3::from(lc.models[mi].inst.pos) + Vec3::new(0.0, 7.0, 0.0);
    let (msel, w) = edit::add_copy(&mut lc, Sel::Model(mi), Some(mpos))?;
    h.apply(&mut lc, "copy model", None, w);
    let spos = Vec3::from(own_before) + Vec3::new(2.0, 0.0, -3.0);
    let (ssel, w) = edit::add_copy(&mut lc, Sel::Spawn(li, own), Some(spos))?;
    h.apply(&mut lc, "copy spawn", None, w);
    check("copies are present", edit::is_added(&lc, msel) && edit::is_added(&lc, ssel) && lc.levels[li].dirty && lc.levels[ml].dirty, String::new());
    let files = edit::export(&lc, &out, "added", "selftest adds", false)?;
    let mut grown_ok = true;
    for f in files.iter().filter(|f| f.extension().is_some_and(|e| e == "levelb")) {
        let b = std::fs::read(f).map_err(|e| e.to_string())?;
        let re = level::parse(&b, &f.display().to_string())?;
        if re.name == lc.levels[ml].name {
            let last = re.models.last().ok_or("no models")?;
            grown_ok &= re.models.len() == n_models + 1 && Vec3::from(last.pos).distance(mpos) < 1e-4 && last.name == lc.models[mi].inst.name;
            println!("  {}: {} models (was {n_models}), copy at {:?}", re.name, re.models.len(), last.pos);
        }
        if re.name == lc.levels[li].name {
            let last = re.spawns.last().ok_or("no spawns")?;
            grown_ok &= re.spawns.len() == n_spawns + 1 && Vec3::from(last.pos).distance(spos) < 1e-4 && last.label == lc.levels[li].spawns[own].label;
            println!("  {}: {} spawns (was {n_spawns}), copy {} at {:?}", re.name, re.spawns.len(), last.label, last.pos);
        }
    }
    check("grown files re-parse with the copies", grown_ok, format!("{} files", files.len()));
    let w = edit::remove(&lc, ssel)?;
    h.apply(&mut lc, "remove spawn copy", None, w);
    check("remove hides the copy", !lc.levels[li].spawns.last().unwrap().alive && lc.levels[li].adds().len() == 0, String::new());
    h.undo(&mut lc);
    h.undo(&mut lc);
    h.undo(&mut lc);
    check("undo of the adds leaves the course unchanged", lc.levels.iter().all(|l| l.data == l.orig && !l.dirty && l.adds().is_empty()), String::new());
    check("an edit-free export after undo is identical", lc.levels.iter().all(|l| l.export_bytes().map(|b| b == l.orig).unwrap_or(false)), String::new());
    // Rail: a bend moves the point fully and its neighbours partly, keeps shared joints together, and reset/undo restore it.
    let (rl, pi) = edit::rail_ref(&lc).ok_or("no rail")?;
    let before = lc.levels[rl].paths[pi].segments.clone();
    let w = edit::move_rail(&lc, rl, 0, 8, Vec3::new(0.0, 2.5, 0.0), 3)?;
    h.apply(&mut lc, "bend rail", None, w);
    let after = &lc.levels[rl].paths[pi].segments;
    let dy = |i: usize| after[0][i][1] - before[0][i][1];
    check(
        "rail bend: point moves fully, neighbours partly, far points not at all",
        (dy(8) - 2.5).abs() < 1e-4 && dy(7) > 0.5 && dy(7) < 2.5 && dy(11) > 0.0 && dy(12) == 0.0 && dy(4) == 0.0,
        format!("dy 4..12 = {:?}", (4..=12).map(dy).collect::<Vec<_>>()),
    );
    let st = edit::rail_stats(&lc).ok_or("no rail stats")?;
    check("rail stats see the longer ride", st.length > st.orig_length && st.sharp.is_empty(), format!("{} vs {}", st.length, st.orig_length));
    let last0 = before[0].len() - 1;
    let w = edit::move_rail(&lc, rl, 0, last0, Vec3::new(0.0, 1.0, 0.0), 0)?;
    h.apply(&mut lc, "bend rail end", None, w);
    let after = &lc.levels[rl].paths[pi].segments;
    check("a joint moves in both rail parts", after.len() < 2 || after[0][last0] == after[1][0], format!("{:?} vs {:?}", after[0][last0], after.get(1).map(|s| s[0])));
    let w = edit::reset_rail(&lc)?;
    h.apply(&mut lc, "reset rail", None, w);
    check("reset puts the rail back", lc.levels[rl].paths[pi].segments == before && lc.levels.iter().all(|l| l.data == l.orig), String::new());
    h.undo(&mut lc);
    h.undo(&mut lc);
    h.undo(&mut lc);
    check("undo of the bends leaves the course unchanged", lc.levels.iter().all(|l| l.data == l.orig && !l.dirty), String::new());

    // Ride time: the estimate uses the speed measured in game (Florio leg 1 part 0 took 141.0 s unedited).
    if before[0].len() == 50 {
        let secs = before[0].windows(2).map(|w| level::dist(w[0], w[1])).sum::<f32>() / edit::RAIL_SPEED;
        check("ride time estimate matches the measured 141 s", (secs - 141.0).abs() < 2.0, format!("{secs:.1} s"));
    }

    // A bend survives export: same size, only the moved floats differ, and the re-parsed rail equals the edited one.
    let w = edit::move_rail(&lc, rl, 0, 8, Vec3::new(0.0, 2.5, 0.0), 3)?;
    h.apply(&mut lc, "bend rail for export", None, w);
    let st = edit::rail_stats(&lc).ok_or("no rail stats")?;
    check(
        "ride time change follows the length change",
        st.seconds_change() > 0.0 && (st.seconds_change() * edit::RAIL_SPEED - (st.length - st.orig_length)).abs() < 1e-4,
        st.summary(),
    );
    let files = edit::export(&lc, &out, "railbend", "selftest rail bend", false)?;
    let rfile = std::path::Path::new(&lc.levels[rl].file).file_name().unwrap().to_owned();
    let f = files.iter().find(|p| p.file_name() == Some(rfile.as_os_str())).ok_or("rail level not exported")?;
    let bytes = std::fs::read(f).map_err(|e| e.to_string())?;
    let re = level::parse(&bytes, &f.display().to_string())?;
    let same = re.paths[pi].segments.len() == lc.levels[rl].paths[pi].segments.len()
        && re.paths[pi].segments.iter().zip(&lc.levels[rl].paths[pi].segments).all(|(a, b)| a.len() == b.len() && a.iter().zip(b).all(|(p, q)| level::dist(*p, *q) < 1e-6));
    let diff = bytes.iter().zip(&lc.levels[rl].orig).filter(|(a, b)| a != b).count();
    check(
        "exported rail bend re-parses to the edited rail",
        same && bytes.len() == lc.levels[rl].orig.len() && diff > 0 && diff <= 12 * 9,
        format!("{} bytes, {diff} bytes differ", bytes.len()),
    );
    h.undo(&mut lc);

    // The checks catch a sharp sideways kink and two points squeezed together.
    let ground = pick::Ground::default();
    let w = edit::move_rail(&lc, rl, 0, 20, Vec3::new(0.0, 0.0, 8.0), 0)?;
    h.apply(&mut lc, "kink rail", None, w);
    let issues = edit::checks(&lc, &ground);
    let sharp = issues.iter().any(|i| matches!(i.sel, Sel::Rail(_, 0, 20)) && i.text.starts_with("Sharp bend at rail point 20"));
    check("a sharp sideways kink is reported", sharp, issues.iter().map(|i| i.text.as_str()).collect::<Vec<_>>().join(" | "));
    h.undo(&mut lc);
    let (p30, p31) = (Vec3::from(before[0][30]), Vec3::from(before[0][31]));
    let w = edit::move_rail(&lc, rl, 0, 30, (p31 - p30) * 0.95, 0)?;
    h.apply(&mut lc, "squeeze rail", None, w);
    let st = edit::rail_stats(&lc).ok_or("no rail stats")?;
    let issues = edit::checks(&lc, &ground);
    check(
        "squeezed rail points are reported",
        p30.distance(p31) >= 0.25 && st.bunched.contains(&(0, 30)) && issues.iter().any(|i| i.text.starts_with("Rail points 30 and 31")),
        format!("bunched {:?}", st.bunched),
    );
    h.undo(&mut lc);
    check("undo after the rail checks leaves the course unchanged", lc.levels.iter().all(|l| l.data == l.orig && !l.dirty), String::new());
    println!("selftest output in {}", out.display());
    Ok(ok)
}

fn main() {
    let args = match parse_args() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(2);
        }
    };
    let data = course::DataRoot::locate(args.data.as_deref());
    let course_name = args.course.clone().unwrap_or("LC_001_A".into());
    if args.summary || args.selftest || args.rail_view.is_some() || args.layout || args.rail_info || !args.model_adds.is_empty() {
        let r = match data {
            Ok(mut d) => {
                if args.rail_info {
                    rail_info(&mut d, &course_name, args.variant).map(|_| true)
                } else if !args.model_adds.is_empty() {
                    let dir = args.export.clone().ok_or("--add-model needs --export DIR".to_string());
                    dir.and_then(|dir| add_models(&mut d, &course_name, args.variant, &args.model_adds, &dir, args.pack.as_deref().unwrap_or("added-objects")))
                } else if args.layout {
                    let c = d.courses().into_iter().find(|c| c.name.eq_ignore_ascii_case(&course_name));
                    match c.ok_or("course not found".to_string()).and_then(|c| d.load(&c, args.variant)) {
                        Ok(lc) => {
                            for l in &lc.levels {
                                layout::dump(&l.data, &l.name);
                            }
                            Ok(true)
                        }
                        Err(e) => Err(e),
                    }
                } else if let Some(dist) = args.rail_view {
                    rail_view(&mut d, &course_name, args.variant, dist).map(|_| true)
                } else if args.summary {
                    print_summary(&mut d, &course_name, args.variant).map(|_| true)
                } else {
                    selftest(&mut d, &course_name, args.variant)
                }
            }
            Err(e) => Err(e),
        };
        match r {
            Ok(true) => return,
            Ok(false) => std::process::exit(1),
            Err(e) => {
                eprintln!("{e}");
                std::process::exit(1);
            }
        }
    }
    let mut ed = app::EditorApp::new(data);
    ed.advanced = args.advanced;
    if args.course.is_some() {
        if let Err(e) = ed.open(&course_name, args.variant) {
            eprintln!("{e}");
        }
    }
    for (l, i, d) in &args.moves {
        match ed.move_by(Sel::Spawn(*l, *i), *d) {
            Ok(()) => println!("moved spawn {l}:{i} by {d}"),
            Err(e) => {
                eprintln!("move {l}:{i}: {e}");
                std::process::exit(1);
            }
        }
    }
    for (l, i, at) in &args.spawn_copies {
        match ed.copy_to(Sel::Spawn(*l, *i), *at) {
            Ok(s) => println!("copied spawn {l}:{i} to {at} as {s:?}"),
            Err(e) => {
                eprintln!("copy {l}:{i}: {e}");
                std::process::exit(1);
            }
        }
    }
    for (i, d, soft) in &args.rail_bends {
        match ed.bend_rail(*i, *d, *soft) {
            Ok(m) => println!("bent rail at point {i} by {d}: {m}"),
            Err(e) => {
                eprintln!("bend rail {i}: {e}");
                std::process::exit(1);
            }
        }
    }
    for (name, d) in &args.model_moves {
        let idx = ed.loaded.as_ref().and_then(|c| c.models.iter().position(|m| m.inst.name.eq_ignore_ascii_case(name)));
        match idx.map(|i| ed.move_model_by(i, *d)) {
            Some(Ok(())) => println!("moved model {name} by {d}"),
            Some(Err(e)) => {
                eprintln!("move {name}: {e}");
                std::process::exit(1);
            }
            None => {
                eprintln!("no model instance named {name}");
                std::process::exit(1);
            }
        }
    }
    if let Some(dir) = &args.export {
        let pack = args.pack.clone().unwrap_or(format!("{course_name}-edit"));
        match ed.export_to(dir.clone(), &pack) {
            Ok(m) => println!("{m}"),
            Err(e) => {
                eprintln!("export: {e}");
                std::process::exit(1);
            }
        }
    }
    if let Some((l, i)) = args.select {
        ed.select(Some(Sel::Spawn(l, i)), true);
    }
    if let Some(out) = &args.shot {
        ed.expand_all = true;
        match headless::shot(&mut ed, out, args.size.0, args.size.1, 4) {
            Ok(()) => println!("wrote {}", out.display()),
            Err(e) => {
                eprintln!("{e}");
                std::process::exit(1);
            }
        }
        return;
    }
    if args.export.is_some() || !args.moves.is_empty() || !args.model_moves.is_empty() || !args.spawn_copies.is_empty() || !args.rail_bends.is_empty() {
        return;
    }
    let opts = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([1600.0, 900.0]).with_title("Snap level editor"),
        renderer: eframe::Renderer::Wgpu,
        ..Default::default()
    };
    let r = eframe::run_native("Snap level editor", opts, Box::new(|_cc| Ok(Box::new(Window(ed)))));
    if let Err(e) = r {
        eprintln!("{e}");
    }
}

struct Window(app::EditorApp);

impl eframe::App for Window {
    fn ui(&mut self, ui: &mut egui::Ui, frame: &mut eframe::Frame) {
        let Some(rs) = frame.wgpu_render_state() else {
            ui.label("wgpu renderer not available");
            return;
        };
        let mut r = rs.renderer.write();
        let mut gpu = app::Gpu { device: &rs.device, queue: &rs.queue, egui: &mut r };
        self.0.ui(ui, &mut gpu);
    }
}
