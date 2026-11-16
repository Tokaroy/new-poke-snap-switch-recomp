//! Scenery import into a mod pack, shared by the command line (`--import`) and the editor's Import panel.
//!
//! One import writes three things into `<dir>/<pack>/`:
//! - `romfs/<archive>`: the course archive with the new model group (`scenery::build`, `drp.rs`);
//! - `<archive>/<level>`: the level with a clone of the template's instance naming the new group (`grow.rs`);
//! - `<archive>/<ground>.DC4A8177`: the ground collision with the model's triangles as one more shape (`terrain.rs`).
//!
//! When the pack already holds these files (an earlier import), the next import builds on them, so one pack can carry
//! several imported models. Imported user files never enter the repository; packs are written where the user says.

use crate::course::{DataRoot, LoadedCourse};
use crate::{drp, edit, ground, grow, level, render, scenery, terrain};
use glam::Vec3;
use std::path::{Path, PathBuf};

/// The game's archive for a course: under `romfs` when given, else the `romfs-v262144` folder next to the data root.
pub fn romfs_archive(data: &DataRoot, romfs: Option<&Path>, archive: &str) -> PathBuf {
    let root = romfs.map(|p| p.to_path_buf()).unwrap_or_else(|| data.root.parent().unwrap_or(&data.root).join("romfs-v262144"));
    root.join(archive)
}

fn native(archive: &str) -> String {
    archive.replace('/', std::path::MAIN_SEPARATOR_STR)
}

/// Imports a model as a new scenery group of course `name` (leg `variant`) at `pos` (y = height above the surface
/// under x, z) turned by `yaw` degrees, into the pack `<dir>/<pack>`. Returns report lines.
#[allow(clippy::too_many_arguments)]
pub fn import_to_pack(data: &mut DataRoot, name: &str, variant: u32, spec: &scenery::Spec, pos: Vec3, yaw: f32, romfs: Option<&Path>, dir: &Path, pack: &str) -> Result<Vec<String>, String> {
    let c = data.courses().into_iter().find(|c| c.name.eq_ignore_ascii_case(name)).ok_or("course not found")?;
    let mut lc = data.load(&c, variant)?;
    import_into(data, &mut lc, spec, pos, yaw, romfs, dir, pack)
}

/// The import itself, on a loaded course. A level the caller has not changed (`dirty` false) is first replaced by the
/// pack's copy when the pack has one, so an earlier import's instance survives; a changed level (the editor's unsaved
/// edits) is used as it is. On success the course's levels hold the new instance and are marked changed, so a later
/// export from the editor keeps it.
#[allow(clippy::too_many_arguments)]
pub fn import_into(data: &DataRoot, lc: &mut LoadedCourse, spec: &scenery::Spec, pos: Vec3, yaw: f32, romfs: Option<&Path>, dir: &Path, pack: &str) -> Result<Vec<String>, String> {
    let mut rep = vec![];
    if spec.template.is_empty() {
        return Err("choose a template model group (see --list-materials)".into());
    }
    if spec.name.is_empty() || !spec.name.chars().all(|ch| ch.is_ascii_alphanumeric() || ch == '_') {
        return Err("the new model's name must be letters, digits and _ only".into());
    }
    let pack_dir = dir.join(edit::sanitize_pack(pack));
    let arch_dir = pack_dir.join(native(&lc.archive));
    let rom_out = pack_dir.join("romfs").join(native(&lc.archive));
    // build on an earlier import in the same pack
    let src = if rom_out.exists() { rom_out.clone() } else { romfs_archive(data, romfs, &lc.archive) };
    let a = drp::Archive::open(&src)?;
    if a.find_group(&spec.name).is_some() {
        return Err(format!("the course already has a model named {}; pick another name", spec.name));
    }
    for l in lc.levels.iter_mut().filter(|l| !l.dirty) {
        let f = arch_dir.join(Path::new(&l.file).file_name().ok_or("bad level file name")?);
        if let Ok(b) = std::fs::read(&f) {
            let file = l.file.clone();
            *l = level::parse(&b, &file).map_err(|e| format!("{}: {e}", f.display()))?;
            l.dirty = true;
            rep.push(format!("building on the pack's {}", l.name));
        }
    }
    // y is the height above the surface under (x, z) (ground or any other scenery), like click-to-place
    let (_, surf) = render::build_geometry(lc, render::Layers::default());
    let pos = match surf.raycast(Vec3::new(pos.x, 1000.0, pos.z), Vec3::NEG_Y, 2000.0, None).map(|h| h.point.y) {
        Some(h) => {
            rep.push(format!("ground at ({:.1}, {:.1}) is y {h:.2}; placing {:.2} above it", pos.x, pos.z, pos.y));
            Vec3::new(pos.x, h + pos.y, pos.z)
        }
        None => {
            rep.push(format!("no ground under ({:.1}, {:.1}); y {:.2} is absolute", pos.x, pos.z, pos.y));
            pos
        }
    };
    // the placement is baked into the vertices; the instance stays at the origin like the course's own scenery
    let spec = &scenery::Spec { place: (pos, yaw), ..spec.clone() };
    let built = scenery::build(spec, &a, &data.root.join(&lc.archive))?;
    rep.push(format!("{}: shapes {}, world bounds {:.1?}..{:.1?}", spec.name, built.shapes.join(", "), built.bounds.0, built.bounds.1));
    for (src_m, game_m, v, t) in &built.parts {
        rep.push(format!("  part {src_m}: {v} vertices, {t} triangles -> {game_m}"));
    }
    // debugging aid: SNAP_IMPORT_DUMP=DIR writes the new files there as loose files
    if let Some(d) = std::env::var_os("SNAP_IMPORT_DUMP") {
        let d = PathBuf::from(d);
        std::fs::create_dir_all(&d).map_err(|e| e.to_string())?;
        for (n, b) in &built.chunks {
            std::fs::write(d.join(n), b).map_err(|e| format!("{n}: {e}"))?;
        }
    }
    let (li, ei) = scenery::template_instance(lc, &spec.template).ok_or_else(|| format!("no instance of {} to copy", spec.template))?;
    let add = grow::Add {
        entity: ei,
        set: vec![(5, 3, vec![0.0; 3]), (5, 4, vec![0.0, 0.0, 0.0, 1.0]), (5, 5, vec![1.0, 1.0, 1.0])],
        names: vec![(5, 1, spec.name.clone())],
        // the instance lists one lightmap (s17) and one s23 entry per model entry, in model order
        lists: vec![(5, 17, built.shapes.clone()), (5, 23, built.shapes.clone())],
    };
    let l = &mut lc.levels[li];
    let (out, ids) = grow::grow(&l.data, std::slice::from_ref(&add))?;
    rep.push(format!("{}: {}", l.name, grow::verify(&l.data, &out, std::slice::from_ref(&add))?));
    rep.push(format!("  new entity {:08x}: instance at the origin, geometry around {pos:.1}", ids[0].hash));
    let file = l.file.clone();
    *l = level::parse(&out, &file)?;
    l.dirty = true;
    let files = edit::export(lc, dir, pack, &format!("Imported scenery: {}", spec.name), false)?;
    // collision: the model's triangles as one more shape in the collision file of the ground under it (the course's
    // rocks and props live there too); served as a loose file next to the level
    let center = (built.bounds.0 + built.bounds.1) * 0.5;
    let mut collided = false;
    for m in lc.models.iter().filter(|m| ground::is_terrain_name(&m.inst.name)) {
        let Some(msrc) = &m.source else { continue };
        let fname = format!("{}.DC4A8177", m.inst.name);
        let packed = arch_dir.join(&fname);
        let path = if packed.exists() { packed.clone() } else { msrc.parent().ok_or("model without folder")?.join(&fname) };
        let Ok(b) = std::fs::read(&path) else { continue };
        let xf = ground::inst_xf(&m.inst);
        let shapes = terrain::parse_collision(&b).map_err(|e| format!("{}: {e}", path.display()))?;
        let covers = shapes.iter().filter(|s| s.identity() && !s.verts.is_empty()).any(|s| {
            let w: Vec<Vec3> = s.verts.iter().map(|v| xf.transform_point3(*v)).collect();
            let lo = w.iter().fold(Vec3::splat(f32::MAX), |a, v| a.min(*v));
            let hi = w.iter().fold(Vec3::splat(f32::MIN), |a, v| a.max(*v));
            (lo.x..=hi.x).contains(&center.x) && (lo.z..=hi.z).contains(&center.z)
        });
        if !covers {
            continue;
        }
        let inv = xf.inverse();
        let verts: Vec<Vec3> = built.collision.0.iter().map(|v| inv.transform_point3(*v)).collect();
        let flags = terrain::prop_face_flags(&b).ok_or("the ground collision has no faces")?;
        let nb = terrain::append_shape(&b, &verts, &built.collision.1, flags, crc32fast::hash(spec.name.as_bytes()))?;
        std::fs::create_dir_all(&arch_dir).map_err(|e| e.to_string())?;
        std::fs::write(&packed, &nb).map_err(|e| format!("{}: {e}", packed.display()))?;
        rep.push(format!("  collision: {} triangles added to {} (shapes {} -> {}, surface flags {:x?})", built.collision.1.len(), m.inst.name, shapes.len(), shapes.len() + 1, flags));
        collided = true;
        break;
    }
    if !collided {
        rep.push("  collision: no ground collision under the model; it has none".into());
    }
    // write to a temporary name first: the source may be this very file (an earlier import)
    let tmp = rom_out.with_extension("drp.new");
    a.write(&built.chunks, &built.groups, &tmp)?;
    let b = drp::Archive::open(&tmp)?;
    if b.entries.len() != a.entries.len() + built.chunks.len() || b.groups != built.groups {
        return Err("written archive differs from the plan".into());
    }
    for (k, (n, d)) in built.chunks.iter().enumerate() {
        let i = a.entries.len() + k;
        if b.entries[i].name != *n || b.raw_payload(i)?.as_deref() != Some(&d[..]) {
            return Err(format!("written archive: {n} differs"));
        }
    }
    drop((a, b));
    std::fs::rename(&tmp, &rom_out).map_err(|e| format!("{}: {e}", rom_out.display()))?;
    rep.push(format!("exported {} file(s) and {} ({} new entries) to {}", files.len(), rom_out.display(), built.chunks.len(), pack_dir.display()));
    Ok(rep)
}
