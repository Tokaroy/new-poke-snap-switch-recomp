//! New Pokémon Snap recomp level editor.
//!
//! level-editor [--data <extracted/drp-v262144>] [--course LC_001_A] [--variant 1]
//!              [--select L:I] [--move L:I:dx,dy,dz]... [--export DIR --pack NAME]
//!              [--shot out.png [--size 1600x900] [--advanced]] [--summary] [--selftest]
//!              [--brush radius,count,seed [--align]] [--scatter MODEL_NAME@x,z | --scatter L:I@x,z]... [--tool place|scatter|terrain]
//!              [--terrain raise|lower|smooth|flatten|paint|erase@x,z[,radius[,strength[,dabs]]]]...
//!
//! Without --shot, --summary or --selftest it opens a window. --shot renders the same UI offscreen and writes a PNG
//! (no window). --move edits spawn I of level L (indices as in the Pokémon list), --export writes the changed levels
//! as a mod pack under DIR. --selftest checks that export is byte-identical without edits and exact with edits.
//! Nothing is ever written to the data folder.

mod app;
mod course;
mod drp;
mod edit;
mod fb;
mod geom;
mod gizmo;
mod headless;
mod ground;
mod grow;
mod import;
mod import_pack;
mod reskin;
mod layout;
mod level;
mod modelb;
mod pick;
mod place;
mod procgen;
mod render;
mod scenery;
mod terrain;

use edit::Sel;
use glam::{Quat, Vec3};
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
    ground_info: bool,
    rail_bends: Vec<(usize, Vec3, usize)>,
    model_adds: Vec<(String, Vec3)>,
    model_moves: Vec<(String, Vec3)>,
    spawn_copies: Vec<(usize, usize, Vec3)>,
    /// (model name or L:I spawn, x, z)
    scatters: Vec<(String, f32, f32)>,
    brush: place::Brush,
    tool: Option<app::Tool>,
    /// (model name or L:I spawn, copies)
    fills: Vec<(String, usize)>,
    /// terrain strokes: (op, x, z, radius, strength, dabs)
    terrain: Vec<(ground::Op, f32, f32, f32, f32, usize)>,
    /// scenery import: model file and where it goes
    import: Option<(PathBuf, Vec3)>,
    import_name: Option<String>,
    template: Option<String>,
    materials: Vec<String>,
    import_scale: f32,
    import_yaw: f32,
    list_materials: bool,
    /// --reskin FILE: the user's model replaces a species' meshes
    reskin: Option<PathBuf>,
    /// --species CH_nnn (with --reskin or --reskin-info)
    species: Option<String>,
    reskin_model: String,
    reskin_keep: Vec<String>,
    reskin_map: Vec<(String, String)>,
    reskin_info: bool,
    /// --procgen FILE: lay a course made by snap-procgen (its JSON) over the leg's first rail part
    procgen: Option<PathBuf>,
    /// --procgen-opts band,swing,relief,props,spawns
    procgen_opts: Option<procgen::Opts>,
    /// RomFS root holding the original archives (default: `romfs-v262144` beside the data root)
    romfs: Option<PathBuf>,
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
    for (spec, d) in adds {
        // NAME or NAME=MESH: the copy shows model group MESH (e.g. an imported one) instead of NAME's
        let (model, mesh) = match spec.split_once('=') {
            Some((m, n)) => (m, Some(n)),
            None => (spec.as_str(), None),
        };
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
        println!("clone {model} (level {li}, entity {ei}) to {p}{}", mesh.map(|m| format!(" showing {m}")).unwrap_or_default());
        let names = mesh.map(|m| vec![(5, 1, m.to_string())]).unwrap_or_default();
        per_level.entry(li).or_default().push(grow::Add { entity: ei, set: vec![(5, 3, p.to_array().to_vec())], names, lists: vec![] });
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

/// Prints the game materials and template model groups of a course (for --material and --template).
fn list_materials(data: &mut course::DataRoot, name: &str, variant: u32, romfs: Option<&std::path::Path>) -> Result<(), String> {
    let c = data.courses().into_iter().find(|c| c.name.eq_ignore_ascii_case(name)).ok_or("course not found")?;
    let lc = data.load(&c, variant)?;
    let a = drp::Archive::open(&import_pack::romfs_archive(data, romfs, &lc.archive))?;
    let t = scenery::templates(&a);
    println!("templates ({}):", t.len());
    for x in &t {
        println!("  {x}");
    }
    let m = scenery::materials(&a);
    println!("materials ({}):", m.len());
    for x in &m {
        println!("  {x}");
    }
    Ok(())
}

/// `--reskin-info`: species packs, or one species' mesh objects (for --reskin-keep and --reskin-map).
fn reskin_info(data: &course::DataRoot, species: Option<&str>, model: &str) -> Result<(), String> {
    match species {
        None => println!("species: {}", reskin::species(&data.root).join(" ")),
        Some(sp) => {
            let (m, objs) = reskin::objects(&data.root, sp, model)?;
            println!("{sp} model {m}: {} mesh objects", objs.len());
            for o in objs {
                println!("  {}.{}  {}  {} vertices{}", o.name, o.subindex, o.material, o.vertices, if o.skinned { ", skinned" } else { "" });
            }
        }
    }
    Ok(())
}

/// `--reskin`: the user's model replaces a species' meshes in a mod pack (see `reskin`).
fn reskin_species(data: &course::DataRoot, spec: &reskin::Spec, dir: &std::path::Path, pack: &str) -> Result<bool, String> {
    if spec.species.is_empty() {
        return Err("--reskin needs --species CH_nnn (see --reskin-info)".into());
    }
    let r = reskin::build(spec, &data.root)?;
    for line in &r.report {
        println!("{line}");
    }
    let files = reskin::write_pack(&r, dir, pack, &format!("Reskin of {}", spec.species))?;
    println!("wrote {} file(s) to {}", files.len(), dir.join(edit::sanitize_pack(pack)).display());
    Ok(true)
}

/// `--import`: imports a model as a new scenery group into a mod pack (see `import_pack`).
#[allow(clippy::too_many_arguments)]
fn import_scenery(data: &mut course::DataRoot, name: &str, variant: u32, spec: &scenery::Spec, pos: Vec3, yaw: f32, romfs: Option<&std::path::Path>, dir: &std::path::Path, pack: &str) -> Result<bool, String> {
    for line in import_pack::import_to_pack(data, name, variant, spec, pos, yaw, romfs, dir, pack)? {
        println!("{line}");
    }
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
        ground_info: false,
        rail_bends: vec![],
        model_adds: vec![],
        spawn_copies: vec![],
        model_moves: vec![],
        scatters: vec![],
        brush: place::Brush::default(),
        tool: None,
        fills: vec![],
        terrain: vec![],
        import: None,
        import_name: None,
        template: None,
        materials: vec![],
        import_scale: 1.0,
        import_yaw: 0.0,
        list_materials: false,
        reskin: None,
        species: None,
        reskin_model: String::new(),
        reskin_keep: vec![],
        reskin_map: vec![],
        reskin_info: false,
        procgen: None,
        procgen_opts: None,
        romfs: None,
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
            "--ground-info" => a.ground_info = true,
            "--mesh-colors" => {
                // FILE.numshb@x,z: Color1 of the vertices within two units of (x, z), then exit
                let v = val()?;
                let (f, xz) = v.rsplit_once('@').ok_or("--mesh-colors FILE@x,z")?;
                let (x, z) = xz.split_once(',').ok_or("--mesh-colors FILE@x,z")?;
                let (x, z): (f32, f32) = (x.parse().map_err(|_| "bad x")?, z.parse().map_err(|_| "bad z")?);
                let mesh = ssbh_data::prelude::MeshData::from_file(f).map_err(|e| e.to_string())?;
                for o in &mesh.objects {
                    let (Some(p), Some(c)) = (o.positions.first(), o.color_sets.first()) else { continue };
                    if let (ssbh_data::mesh_data::VectorData::Vector3(ps), ssbh_data::mesh_data::VectorData::Vector4(cs)) = (&p.data, &c.data) {
                        for (q, col) in ps.iter().zip(cs) {
                            if (q[0] - x).abs() < 2.0 && (q[2] - z).abs() < 2.0 {
                                println!("{}#{} ({:.1},{:.2},{:.1}) rgba {:.2},{:.2},{:.2},{:.2}", o.name, o.subindex, q[0], q[1], q[2], col.x, col.y, col.z, col.w);
                            }
                        }
                    }
                }
                std::process::exit(0);
            }
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
                let (n, d) = v.split_once(':').ok_or("--add-model NAME[=MESH]:dx,dy,dz")?;
                a.model_adds.push((n.to_string(), parse_vec3(d)?));
            }
            "--rail-view" => a.rail_view = Some(val()?.parse().map_err(|_| "--rail-view: number expected")?),
            "--move-model" => {
                let v = val()?;
                let (n, d) = v.split_once(':').ok_or("--move-model NAME:dx,dy,dz")?;
                a.model_moves.push((n.to_string(), parse_vec3(d)?));
            }
            "--scatter" => {
                // SRC@x,z where SRC is a scenery model name or a spawn L:I
                let v = val()?;
                let (src, xz) = v.rsplit_once('@').ok_or("--scatter SRC@x,z")?;
                let (x, z) = xz.split_once(',').ok_or("--scatter SRC@x,z")?;
                a.scatters.push((src.to_string(), x.parse().map_err(|_| "--scatter: bad x")?, z.parse().map_err(|_| "--scatter: bad z")?));
            }
            "--brush" => {
                // radius,count,seed
                let v = val()?;
                let f: Vec<&str> = v.split(',').collect();
                if f.len() != 3 {
                    return Err("--brush radius,count,seed".into());
                }
                a.brush.radius = f[0].parse().map_err(|_| "--brush: bad radius")?;
                a.brush.count = f[1].parse().map_err(|_| "--brush: bad count")?;
                a.brush.seed = f[2].parse().map_err(|_| "--brush: bad seed")?;
            }
            "--align" => a.brush.align = true,
            "--rules" => {
                // slope=DEG,height=LO..HI,rail=LO..HI,ground
                let range = |v: &str| -> Result<(f32, f32), String> {
                    let (lo, hi) = v.split_once("..").ok_or("--rules: range is LO..HI")?;
                    Ok((lo.parse().map_err(|_| "--rules: bad number")?, hi.parse().map_err(|_| "--rules: bad number")?))
                };
                for kv in val()?.split(',') {
                    match kv.split_once('=') {
                        Some(("slope", v)) => a.brush.rules.max_slope = Some(v.parse().map_err(|_| "--rules: bad slope")?),
                        Some(("height", v)) => a.brush.rules.height = Some(range(v)?),
                        Some(("rail", v)) => a.brush.rules.rail_dist = Some(range(v)?),
                        None if kv == "ground" => a.brush.rules.ground_only = true,
                        _ => return Err(format!("--rules: unknown rule {kv}")),
                    }
                }
            }
            "--fill" => {
                // SRC@N
                let v = val()?;
                let (src, n) = v.rsplit_once('@').ok_or("--fill SRC@N")?;
                a.fills.push((src.to_string(), n.parse().map_err(|_| "--fill: bad count")?));
            }
            "--terrain" => {
                // OP@x,z[,radius[,strength[,dabs]]]
                let v = val()?;
                let usage = "--terrain raise|lower|smooth|flatten|paint|erase@x,z[,radius[,strength[,dabs]]]";
                let (op, rest) = v.split_once('@').ok_or(usage)?;
                let op = ground::Op::parse(op).ok_or(usage)?;
                let f: Vec<f32> = rest.split(',').map(|x| x.parse::<f32>()).collect::<Result<_, _>>().map_err(|_| usage)?;
                if f.len() < 2 {
                    return Err(usage.into());
                }
                let g = |i: usize, d: f32| f.get(i).copied().unwrap_or(d);
                a.terrain.push((op, f[0], f[1], g(2, 8.0), g(3, 0.5), g(4, 1.0).max(1.0) as usize));
            }
            "--tool" => {
                a.tool = Some(match val()?.as_str() {
                    "select" => app::Tool::Select,
                    "place" => app::Tool::Place,
                    "scatter" => app::Tool::Scatter,
                    "terrain" => app::Tool::Terrain,
                    _ => return Err("--tool select|place|scatter|terrain".into()),
                })
            }
            "--import" => {
                // FILE@x,y,z
                let v = val()?;
                let (f, p) = v.rsplit_once('@').ok_or("--import FILE@x,y,z")?;
                a.import = Some((f.into(), parse_vec3(p)?));
            }
            "--import-name" => a.import_name = Some(val()?),
            "--template" => a.template = Some(val()?),
            "--material" => a.materials = val()?.split(',').map(str::to_string).collect(),
            "--import-scale" => a.import_scale = val()?.parse().map_err(|_| "--import-scale: number expected")?,
            "--import-yaw" => a.import_yaw = val()?.parse().map_err(|_| "--import-yaw: degrees expected")?,
            "--list-materials" => a.list_materials = true,
            "--reskin" => a.reskin = Some(PathBuf::from(val()?)),
            "--species" => a.species = Some(val()?),
            "--reskin-model" => a.reskin_model = val()?,
            "--reskin-keep" => a.reskin_keep = val()?.split(',').map(str::to_string).collect(),
            "--reskin-map" => {
                a.reskin_map = val()?
                    .split(',')
                    .map(|kv| kv.split_once('=').map(|(k, v)| (k.to_string(), v.to_string())).ok_or("--reskin-map: source=object pairs expected"))
                    .collect::<Result<_, _>>()?
            }
            "--reskin-info" => a.reskin_info = true,
            "--procgen" => a.procgen = Some(PathBuf::from(val()?)),
            "--procgen-opts" => {
                let v = val()?;
                let f: Vec<f32> = v.split(',').map(|x| x.parse::<f32>()).collect::<Result<_, _>>().map_err(|_| "--procgen-opts: numbers expected")?;
                if f.len() != 5 {
                    return Err("--procgen-opts band,swing,relief,props,spawns".into());
                }
                let d = procgen::Opts::default();
                a.procgen_opts = Some(procgen::Opts { band: f[0], swing: f[1], relief: f[2], max_props: f[3] as usize, max_spawns: f[4] as usize, ..d });
            }
            "--romfs" => a.romfs = Some(val()?.into()),
            "--export" => a.export = Some(val()?.into()),
            "--pack" => a.pack = Some(val()?),
            "-h" | "--help" => {
                println!("level-editor [--data DIR] [--course LC_001_A] [--variant 1] [--select L:I] [--move L:I:dx,dy,dz] [--export DIR --pack NAME] [--procgen course.json [--procgen-opts band,swing,relief,props,spawns]] [--shot out.png] [--size WxH] [--advanced] [--summary] [--selftest]");
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

/// Ground files of the course: collision shapes, navmesh tiles, ground mesh attributes and materials, plus a height
/// comparison between the rendered ground, the collision and the navmesh at points along the rail.
fn ground_info(data: &mut course::DataRoot, name: &str, variant: u32) -> Result<(), String> {
    use ssbh_data::prelude::*;
    let c = data.courses().into_iter().find(|c| c.name.eq_ignore_ascii_case(name)).ok_or("course not found")?;
    let lc = data.load(&c, variant)?;
    let (_, ground) = render::build_geometry(&lc, render::Layers::default());
    for m in lc.models.iter().filter(|m| m.inst.name.to_lowercase().contains("ground")) {
        let stem = m.inst.name.clone();
        println!("== {} at {:?} rot {:?} scale {:?}", stem, m.inst.pos, m.inst.rot, m.inst.scale);
        if let Some(src) = &m.source {
            if let Ok(modl) = ModlData::from_file(src) {
                let dir = src.parent().unwrap();
                if let Ok(mesh) = MeshData::from_file(dir.join(&modl.mesh_file_name)) {
                    for o in &mesh.objects {
                        let n = |a: &[ssbh_data::mesh_data::AttributeData]| a.iter().map(|x| format!("{}({})", x.name, x.data.len())).collect::<Vec<_>>().join(" ");
                        println!(
                            "  mesh {}#{}: {} idx | pos {} | nrm {} | uv {} | color {} | tan {}",
                            o.name, o.subindex, o.vertex_indices.len(), n(&o.positions), n(&o.normals), n(&o.texture_coordinates), n(&o.color_sets), n(&o.tangents)
                        );
                        for cs in &o.color_sets {
                            if let ssbh_data::mesh_data::VectorData::Vector4(v) = &cs.data {
                                let mut lo = [f32::MAX; 4];
                                let mut hi = [f32::MIN; 4];
                                let mut distinct = std::collections::BTreeSet::new();
                                for c in v {
                                    for (k, x) in [c.x, c.y, c.z, c.w].iter().enumerate() {
                                        lo[k] = lo[k].min(*x);
                                        hi[k] = hi[k].max(*x);
                                    }
                                    distinct.insert([(c.x * 255.0) as u8, (c.y * 255.0) as u8, (c.z * 255.0) as u8, (c.w * 255.0) as u8]);
                                }
                                println!("    {} rgba min {:.2?} max {:.2?}, {} distinct, e.g. {:?}", cs.name, lo, hi, distinct.len(), distinct.iter().take(6).collect::<Vec<_>>());
                                // the color under rail points (the dirt path) and six units to each side (usually grass)
                                if let (Some(pos), Some(rail)) = (o.positions.first(), lc.rail()) {
                                    if let ssbh_data::mesh_data::VectorData::Vector3(ps) = &pos.data {
                                        let xf = ground::inst_xf(&m.inst);
                                        let near = |x: f32, z: f32| {
                                            ps.iter().enumerate().map(|(i, p)| (i, xf.transform_point3(Vec3::from(*p)))).min_by(|a, b| {
                                                let d = |q: Vec3| (q.x - x).powi(2) + (q.z - z).powi(2);
                                                d(a.1).total_cmp(&d(b.1))
                                            })
                                        };
                                        let mut line = vec![];
                                        for p in rail.segments.first().into_iter().flatten().step_by(8) {
                                            for dx in [-6.0f32, 0.0, 6.0] {
                                                if let Some((i, q)) = near(p[0] + dx, p[2]) {
                                                    if (q.x - p[0] - dx).abs() < 2.0 && (q.z - p[2]).abs() < 2.0 {
                                                        let c = v[i];
                                                        line.push(format!("({:.0},{:.0})={:.2},{:.2},{:.2},{:.2}", p[0] + dx, p[2], c.x, c.y, c.z, c.w));
                                                    }
                                                }
                                            }
                                        }
                                        println!("    {} along the rail (x-6, x, x+6): {}", cs.name, line.join(" "));
                                    }
                                }
                            }
                        }
                    }
                }
                for e in &modl.entries {
                    println!("  entry {}#{} -> {}", e.mesh_object_name, e.mesh_object_subindex, e.material_label);
                }
                for mf in &modl.material_file_names {
                    if let Ok(matl) = MatlData::from_file(dir.join(mf)) {
                        for e in &matl.entries {
                            let tex: Vec<String> = e.textures.iter().map(|t| format!("{:?}={}", t.param_id, t.data)).collect();
                            println!("  material {} [{}]: {}", e.material_label, e.shader_label, tex.join(" "));
                        }
                    }
                }
            }
        }
        let inst = {
            let q = Quat::from_array(m.inst.rot);
            let q = if q.length_squared() < 1e-6 { Quat::IDENTITY } else { q.normalize() };
            glam::Mat4::from_scale_rotation_translation(Vec3::from(m.inst.scale), q, Vec3::from(m.inst.pos))
        };
        let col = std::fs::read(c.dir.join(format!("{stem}.DC4A8177"))).ok();
        let shapes = col.as_deref().map(terrain::parse_collision);
        if let Some(r) = &shapes {
            match r {
                Ok(sh) => {
                    let ident = sh.iter().filter(|s| s.identity()).count();
                    let tris: usize = sh.iter().map(|s| s.tris.len()).sum();
                    let mut fl: std::collections::BTreeMap<u32, usize> = Default::default();
                    for s in sh {
                        for f in &s.flags {
                            *fl.entry(*f).or_default() += 1;
                        }
                    }
                    println!("  collision: {} shapes ({} identity), {} triangles, face flags {:x?}", sh.len(), ident, tris, fl);
                }
                Err(e) => println!("  collision: {e}"),
            }
        }
        let nav = std::fs::read(c.dir.join(format!("{stem}.navmshb"))).ok();
        let tiles = nav.as_deref().map(terrain::find_tiles).unwrap_or_default();
        for t in &tiles {
            println!(
                "  navmesh tile @{:#x}: {} polys, {} verts, {} detail verts, {} bv nodes, {} off-mesh, bounds {:?}..{:?}, quant {}, walkable {:?}, ends {:#x} of {:#x}",
                t.at, t.poly_count, t.vert_count, t.detail_verts, t.bv_nodes, t.off_mesh, t.bmin, t.bmax, t.quant, t.walkable, t.end, nav.as_ref().unwrap().len()
            );
        }
        // heights along the rail: render ground vs collision vs navmesh
        let Some(rail) = lc.rail() else { continue };
        let Ok(sh) = shapes.unwrap_or(Err(String::new())) else { continue };
        let mut n = 0;
        let (mut dcol, mut dnav, mut ncol, mut nnav) = (0f32, 0f32, 0, 0);
        for seg in &rail.segments {
            for p in seg.iter().step_by(3) {
                let p = Vec3::from(*p);
                let Some(h) = ground.raycast_where(p + Vec3::Y * 60.0, -Vec3::Y, 200.0, &|k| k == lc.models.iter().position(|x| std::ptr::eq(x, m)).unwrap_or(usize::MAX)) else { continue };
                n += 1;
                let mut best: Option<f32> = None;
                for s in &sh {
                    let xf = inst * s.xf();
                    for t in &s.tris {
                        let [a, b, d] = t.map(|i| xf.transform_point3(s.verts[i as usize]));
                        if let Some(y) = terrain::tri_height(a, b, d, h.point.x, h.point.z) {
                            if best.map_or(true, |bb| (y - h.point.y).abs() < (bb - h.point.y).abs()) {
                                best = Some(y);
                            }
                        }
                    }
                }
                if let Some(y) = best {
                    dcol = dcol.max((y - h.point.y).abs());
                    ncol += 1;
                }
                if let (Some(nb), Some(t)) = (nav.as_deref(), tiles.first()) {
                    if let Some(y) = t.height_at(nb, h.point.x, h.point.z, h.point.y) {
                        if (y - h.point.y).abs() > 0.3 {
                            println!("    navmesh off by {:.2} at {:.1?}", y - h.point.y, h.point);
                        }
                        dnav = dnav.max((y - h.point.y).abs());
                        nnav += 1;
                    }
                }
            }
        }
        println!("  rail samples on this ground: {n}; collision found {ncol} (max |dy| {dcol:.3}); navmesh found {nnav} (max |dy| {dnav:.3})");
        if let Some(seg) = rail.segments.first() {
            let pts: Vec<String> = seg.iter().enumerate().step_by(4).map(|(i, p)| format!("{i}:({:.1},{:.1},{:.1})", p[0], p[1], p[2])).collect();
            println!("  rail points: {}", pts.join(" "));
        }
    }
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

    // Placement: a scatter dab of a scenery model around a Pokémon's spot lands on the ground inside the circle, keeps
    // its gap, tilts to the slope, repeats from the same seed, exports as grown files and undoes in one step.
    let ground = render::build_geometry(&lc, render::Layers::default()).1;
    let mi = lc.models.iter().position(|m| m.inst.pos_off.is_some() && m.inst.rot_off.is_some() && m.inst.scale_off.is_some()).ok_or("no model with a stored rotation and size")?;
    let ml = lc.models[mi].level_idx;
    let orig_models = lc.levels[ml].models.iter().filter(|m| m.alive_off.is_none()).count();
    let site = place::Site::of(&lc);
    let c0 = place::surface_below(&ground, &site, Vec3::from(own_before), 50.0, false).ok_or("no ground under the test spawn")?.0;
    let brush = place::Brush { radius: 10.0, count: 8, min_gap: 2.0, scale: (0.5, 2.0), align: true, seed: 7, ..Default::default() };
    let spots = place::dab(&ground, &site, &brush, &mut place::Rng::new(brush.seed), c0, &mut vec![]);
    let again = place::dab(&ground, &site, &brush, &mut place::Rng::new(brush.seed), c0, &mut vec![]);
    check(
        "scatter repeats from the same seed",
        spots.len() == again.len() && spots.iter().zip(&again).all(|(a, b)| a.pos == b.pos && a.yaw == b.yaw && a.scale_mul == b.scale_mul),
        format!("{} spots", spots.len()),
    );
    let flat = |v: Vec3| Vec3::new(v.x, 0.0, v.z);
    let apart = spots.iter().enumerate().all(|(i, a)| spots[i + 1..].iter().all(|b| flat(a.pos - b.pos).length() >= 2.0));
    let inside = spots.iter().all(|s| flat(s.pos - c0).length() <= 10.0 + 1e-3);
    check("scatter keeps the gap and stays in the circle", spots.len() >= 4 && apart && inside, format!("{} of 8 placed", spots.len()));
    let (src_scale, src_rot) = (Vec3::from(lc.models[mi].inst.scale), lc.models[mi].inst.rot);
    let steps0 = h.steps();
    let mut placed = vec![];
    for sp in &spots {
        let rot = place::orient(src_rot, sp.yaw, sp.normal, true);
        let (ns, w) = edit::place_copy(&mut lc, Sel::Model(mi), sp.pos, Some(rot), sp.scale_mul)?;
        h.apply(&mut lc, "scatter", Some("scatter"), w);
        placed.push((ns, *sp));
    }
    h.end_interaction();
    let mut worst = 0.0f32;
    let mut sat = true;
    for (ns, sp) in &placed {
        let Sel::Model(m) = ns else { continue };
        let inst = &lc.models[*m].inst;
        sat &= Vec3::from(inst.pos).distance(sp.pos) < 1e-4 && (Vec3::from(inst.scale) - src_scale * sp.scale_mul).length() < 1e-3;
        worst = worst.max(1.0 - (Quat::from_array(inst.rot) * Vec3::Y).dot(sp.normal));
        sat &= place::surface_below(&ground, &site, sp.pos, 0.5, false).map(|g| (g.0.y - sp.pos.y).abs() < 0.05).unwrap_or(false);
    }
    check("scattered copies sit on the ground with their size and tilt", sat && worst < 1e-3, format!("worst tilt error {worst:.2e}"));
    check("a scatter stroke is one undo step", h.steps() == steps0 + 1, format!("{} steps", h.steps() - steps0));
    let (ss, w) = edit::place_copy(&mut lc, Sel::Spawn(li, own), Vec3::from(own_before) + Vec3::new(-2.0, 0.0, 2.0), Some(Quat::from_rotation_y(1.0)), 1.0)?;
    h.apply(&mut lc, "place spawn", None, w);
    let sy = edit::rot_of(&lc, ss).map(edit::yaw_of).unwrap_or(f32::NAN);
    check("a placed Pokémon gets its heading", (sy - 1.0).abs() < 1e-4, format!("{sy:.4} rad"));
    let files = edit::export(&lc, &out, "scatter", "selftest scatter", false)?;
    let mfile = std::path::Path::new(&lc.levels[ml].file).file_name().unwrap().to_owned();
    let f = files.iter().find(|p| p.file_name() == Some(mfile.as_os_str())).ok_or("scatter level not exported")?;
    let re = level::parse(&std::fs::read(f).map_err(|e| e.to_string())?, &f.display().to_string())?;
    let last = re.models.last().ok_or("no models")?;
    let want = placed.last().map(|p| p.1.pos).unwrap_or(Vec3::ZERO);
    check(
        "the scattered copies export as grown files",
        re.models.len() == orig_models + placed.len() && Vec3::from(last.pos).distance(want) < 1e-4,
        format!("{} models (was {orig_models})", re.models.len()),
    );
    h.undo(&mut lc);
    h.undo(&mut lc);
    check(
        "undo of the scatter leaves the course unchanged",
        lc.levels.iter().all(|l| l.data == l.orig && !l.dirty && l.adds().is_empty()),
        String::new(),
    );
    // Rule-based fill on the real course: every spot is on a ground model, no steeper than the limit, inside the
    // rail-distance band and apart from the others.
    let rules = place::Rules { max_slope: Some(25.0), rail_dist: Some((5.0, 20.0)), ground_only: true, ..Default::default() };
    let fb = place::Brush { min_gap: 2.0, seed: 11, rules, ..Default::default() };
    let (mn, mut mx) = site.rail_bounds(20.0).ok_or("no rail")?;
    mx.y += 200.0;
    let spots = place::fill(&ground, &site, &fb, &mut place::Rng::new(fb.seed), mn, mx, 60, &mut vec![]);
    let ok_rules = spots.iter().all(|s| {
        let d = site.rail_distance(s.pos);
        s.normal.y >= 25f32.to_radians().cos() - 1e-4
            && (5.0..=20.0).contains(&d)
            && place::surface_below(&ground, &site, s.pos, 0.5, true).map(|g| g.0.distance(s.pos) < 1e-3).unwrap_or(false)
    });
    let apart = spots.iter().enumerate().all(|(i, a)| spots[i + 1..].iter().all(|b| flat(a.pos - b.pos).length() >= 2.0));
    check("rule fill keeps to slope, rail band and ground", spots.len() >= 30 && ok_rules && apart, format!("{} of 60 placed", spots.len()));

    // Terrain: the unedited ground writes back byte-identical; a raise lifts the mesh, collision and navmesh together;
    // the navmesh bounds still hold every polygon; undo restores.
    let t0 = ground::Terrain::build(&lc, &ground).ok_or("no ground model to edit")?;
    let files_of = |lc: &course::LoadedCourse| -> Result<Vec<(PathBuf, Vec<u8>, glam::Mat4)>, String> {
        let t = lc.terrain.as_ref().ok_or("no terrain")?;
        let mut v = vec![];
        for &mi in &t.models {
            let m = &lc.models[mi];
            let Some(src) = &m.source else { continue };
            let inst = ground::inst_xf(&m.inst);
            for (p, b) in t.ground_files(src.parent().ok_or("no folder")?, &m.inst.name, src, inst)? {
                v.push((p, b, inst));
            }
        }
        Ok(v)
    };
    lc.terrain = Some(t0);
    let same = files_of(&lc)?;
    let ident = same.iter().filter(|(p, b, _)| std::fs::read(p).map(|o| o == *b).unwrap_or(false)).count();
    let kinds = |v: &[(PathBuf, Vec<u8>, glam::Mat4)]| {
        let has = |e: &str| v.iter().any(|(p, ..)| p.extension().is_some_and(|x| x.eq_ignore_ascii_case(e)));
        (has("numshb"), has("DC4A8177"), has("navmshb"))
    };
    let differ: Vec<String> = same
        .iter()
        .filter_map(|(p, b, _)| {
            let o = std::fs::read(p).ok()?;
            let n = o.iter().zip(b).filter(|(x, y)| x != y).count();
            let first = o.iter().zip(b).position(|(x, y)| x != y);
            (o != *b).then(|| format!("{} ({} vs {} bytes, {n} differ, first at {first:?})", p.file_name().unwrap_or_default().to_string_lossy(), b.len(), o.len()))
        })
        .collect();
    check(
        "unedited ground writes back byte-identical",
        !same.is_empty() && ident == same.len(),
        format!("{ident} of {} files, kinds {:?} {}", same.len(), kinds(&same), differ.join("; ")),
    );
    // a spot on the ground under the rail, a third of the way along, where collision and navmesh both exist
    let col_h = |v: &[(PathBuf, Vec<u8>, glam::Mat4)], x: f32, z: f32, near: f32| -> Option<f32> {
        let mut best: Option<f32> = None;
        for (p, b, inst) in v.iter().filter(|(p, ..)| p.extension().is_some_and(|e| e == "DC4A8177")) {
            let _ = p;
            for s in terrain::parse_collision(b).ok()? {
                let xf = *inst * s.xf();
                for t in &s.tris {
                    let [a, bb, d] = t.map(|i| xf.transform_point3(s.verts[i as usize]));
                    if let Some(y) = terrain::tri_height(a, bb, d, x, z) {
                        if best.map_or(true, |q| (y - near).abs() < (q - near).abs()) {
                            best = Some(y);
                        }
                    }
                }
            }
        }
        best
    };
    let nav_h = |v: &[(PathBuf, Vec<u8>, glam::Mat4)], x: f32, z: f32, near: f32| -> Option<f32> {
        v.iter().filter(|(p, ..)| p.extension().is_some_and(|e| e == "navmshb")).find_map(|(_, b, _)| terrain::find_tiles(b).iter().find_map(|t| t.height_at(b, x, z, near)))
    };
    let rail_pts: Vec<Vec3> = site.rail.iter().flatten().copied().collect();
    let spot = rail_pts.iter().skip(rail_pts.len() / 3).find_map(|r| {
        let t = lc.terrain.as_ref()?;
        let y = t.height_at(r.x, r.z);
        (!y.is_nan() && col_h(&same, r.x, r.z, y).is_some() && nav_h(&same, r.x, r.z, y).is_some()).then_some(Vec3::new(r.x, y, r.z))
    });
    if let Some(p) = spot {
        let mut h = edit::History::default();
        for _ in 0..5 {
            let w = lc.terrain.as_ref().unwrap().dab(ground::Op::Raise, p, 12.0, 1.0, p.y);
            h.apply(&mut lc, "raise", Some("ground"), w);
        }
        let w = lc.terrain.as_ref().unwrap().dab(ground::Op::Paint, p, 12.0, 1.0, p.y);
        h.apply(&mut lc, "paint", Some("ground"), w);
        h.end_interaction();
        let t = lc.terrain.as_ref().unwrap();
        let want = t.dy(p.x, p.z);
        check("raise builds up the height grid", (want - 1.5).abs() < 0.05 && t.layer_at(p.x, p.z) > 0.2, format!("+{want:.3} at the center, layer {:.2}", t.layer_at(p.x, p.z)));
        let edited = files_of(&lc)?;
        let (c0, c1) = (col_h(&same, p.x, p.z, p.y), col_h(&edited, p.x, p.z, p.y + want));
        let (n0, n1) = (nav_h(&same, p.x, p.z, p.y), nav_h(&edited, p.x, p.z, p.y + want));
        let cd = c1.zip(c0).map(|(a, b)| a - b).unwrap_or(f32::NAN);
        let nd = n1.zip(n0).map(|(a, b)| a - b).unwrap_or(f32::NAN);
        check("collision rises with the ground", (cd - want).abs() < 0.25 * want, format!("+{cd:.3} (grid +{want:.3})"));
        check("navmesh rises with the ground", nd > 0.5 * want && nd < 1.2 * want, format!("+{nd:.3} (grid +{want:.3})"));
        // every ground polygon's vertices sit inside its BV leaf and the tile bounds
        let mut bv_ok = true;
        let mut leaves = 0;
        for (path, b, _) in edited.iter().filter(|(p, ..)| p.extension().is_some_and(|e| e == "navmshb")) {
            let orig = same.iter().find(|(p, ..)| p == path).map(|x| x.1.clone()).unwrap_or_default();
            for tile in terrain::find_tiles(b) {
                let (lo, hi) = (terrain::f32_at(b, tile.at + 76).unwrap_or(0.0), terrain::f32_at(b, tile.at + 88).unwrap_or(0.0));
                for k in 0..tile.bv_nodes {
                    let at = tile.bv_at + 16 * k;
                    let i = i32::from_le_bytes(b[at + 12..at + 16].try_into().unwrap());
                    if i < 0 || i as usize >= tile.poly_count {
                        continue;
                    }
                    let (vs, db, dn) = tile.poly(b, i as usize);
                    if b[tile.polys_at + 32 * i as usize + 31] >> 6 != 0 {
                        continue;
                    }
                    // only raised polygons: the game's own leaves come from Recast data and are left as they were
                    let raised = vs.iter().any(|&v| tile.vert(b, v).y != tile.vert(&orig, v).y)
                        || (0..dn).any(|k| tile.dvert(b, db + k).y != tile.dvert(&orig, db + k).y);
                    if !raised {
                        continue;
                    }
                    leaves += 1;
                    let (q0, q1) = (u16::from_le_bytes([b[at + 2], b[at + 3]]) as f32, u16::from_le_bytes([b[at + 8], b[at + 9]]) as f32);
                    for y in vs.iter().map(|&v| tile.vert(b, v).y).chain((0..dn).map(|k| tile.dvert(b, db + k).y)) {
                        let qy = (y - lo) * tile.quant;
                        // leaves hold truncated quantized heights, as Detour writes them
                        bv_ok &= qy >= q0 - 1e-3 && qy < q1 + 1.0 && y >= lo - 1e-3 && y <= hi + 1e-3;
                    }
                }
            }
        }
        check("navmesh bounds hold every raised polygon", bv_ok && leaves > 0, format!("{leaves} leaves checked"));
        let files = edit::export(&lc, &out, "ground", "selftest", false)?;
        let gfiles = files.iter().filter(|f| !f.extension().is_some_and(|e| e == "levelb" || e == "json")).count();
        check("export writes the ground files", gfiles == same.len(), format!("{gfiles} ground files of {} written", files.len()));
        h.undo(&mut lc);
        let t = lc.terrain.as_ref().unwrap();
        check("undo flattens the ground again", !t.edited(), format!("edited = {}", t.edited()));
    } else {
        check("a ground spot under the rail", false, "none with collision and navmesh".into());
    }
    lc.terrain = None;
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
    if args.summary || args.selftest || args.rail_view.is_some() || args.layout || args.rail_info || args.ground_info || !args.model_adds.is_empty() || args.import.is_some() || args.list_materials || args.reskin.is_some() || args.reskin_info {
        let r = match data {
            Ok(mut d) => {
                if args.reskin_info {
                    reskin_info(&d, args.species.as_deref(), &args.reskin_model).map(|_| true)
                } else if let Some(file) = &args.reskin {
                    let dir = args.export.clone().ok_or("--reskin needs --export DIR".to_string());
                    let spec = reskin::Spec {
                        file: file.clone(),
                        species: args.species.clone().unwrap_or_default(),
                        model: args.reskin_model.clone(),
                        scale: args.import_scale,
                        keep: args.reskin_keep.clone(),
                        map: args.reskin_map.clone(),
                    };
                    dir.and_then(|dir| reskin_species(&d, &spec, &dir, args.pack.as_deref().unwrap_or("reskin")))
                } else if args.list_materials {
                    list_materials(&mut d, &course_name, args.variant, args.romfs.as_deref()).map(|_| true)
                } else if let Some((file, pos)) = &args.import {
                    let dir = args.export.clone().ok_or("--import needs --export DIR".to_string());
                    let spec = scenery::Spec {
                        file: file.clone(),
                        name: args.import_name.clone().unwrap_or_else(|| {
                            let stem = file.file_stem().and_then(|s| s.to_str()).unwrap_or("Model");
                            format!("SM_USR_{}", stem.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '_').collect::<String>())
                        }),
                        template: args.template.clone().unwrap_or_default(),
                        materials: args.materials.clone(),
                        scale: args.import_scale,
                        place: (Vec3::ZERO, 0.0),
                    };
                    dir.and_then(|dir| import_scenery(&mut d, &course_name, args.variant, &spec, *pos, args.import_yaw, args.romfs.as_deref(), &dir, args.pack.as_deref().unwrap_or("imported-scenery")))
                } else if args.ground_info {
                    ground_info(&mut d, &course_name, args.variant).map(|_| true)
                } else if args.rail_info {
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
    ed.brush = args.brush;
    // a brush source on the command line: a spawn L:I or a scenery model instance name
    let resolve = |ed: &app::EditorApp, src: &str| -> Result<Sel, String> {
        if let Ok((l, i)) = parse_li(src) {
            return Ok(Sel::Spawn(l, i));
        }
        let c = ed.loaded.as_ref().ok_or("no course loaded")?;
        c.models.iter().position(|m| m.inst.name.eq_ignore_ascii_case(src)).map(Sel::Model).ok_or_else(|| {
            let mut n: Vec<&str> = c.models.iter().map(|m| m.inst.name.as_str()).collect();
            n.sort();
            n.dedup();
            format!("no model instance named {src}; models: {}", n.join(", "))
        })
    };
    for (src, x, z) in &args.scatters {
        match resolve(&ed, src).and_then(|s| ed.scatter_at(s, *x, *z)) {
            Ok(n) => println!("scattered {n} copies of {src} around ({x}, {z})"),
            Err(e) => {
                eprintln!("scatter {src}: {e}");
                std::process::exit(1);
            }
        }
    }
    for (src, n) in &args.fills {
        let r = resolve(&ed, src).and_then(|s| {
            ed.brush_src = Some(s);
            ed.fill_course(*n)
        });
        match r {
            Ok(k) => println!("filled {k} of {n} copies of {src} by the rules {:?}", ed.brush.rules),
            Err(e) => {
                eprintln!("fill {src}: {e}");
                std::process::exit(1);
            }
        }
    }
    for &(op, x, z, r, s, n) in &args.terrain {
        ed.ground_radius = r;
        ed.ground_strength = s;
        match ed.ground_at(op, x, z, n) {
            Ok(()) => println!("{} ground at ({x}, {z}), radius {r}, strength {s}, {n} dab(s)", op.label()),
            Err(e) => {
                eprintln!("terrain: {e}");
                std::process::exit(1);
            }
        }
    }
    if let Some(f) = &args.procgen {
        match ed.generate_course(f, args.procgen_opts.unwrap_or_default()) {
            Ok(m) => println!("{m}"),
            Err(e) => {
                eprintln!("random course: {e}");
                std::process::exit(1);
            }
        }
    }
    if let Some(t) = args.tool {
        ed.tool = t;
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
    if args.export.is_some() || args.procgen.is_some() || !args.moves.is_empty() || !args.model_moves.is_empty() || !args.spawn_copies.is_empty() || !args.rail_bends.is_empty() {
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
