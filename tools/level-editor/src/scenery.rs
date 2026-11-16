//! Scenery import: turns a user's glTF/OBJ model into a new model group of a course archive, drawn with the game's own
//! materials.
//!
//! A course draws scenery from model groups in its archive (`SM_ENV_*`: skeleton, animation, modelb, mesh and modl files
//! for each LOD, the materials' numatb files, and one child group per material with its textures and matinstb). An
//! import copies a template group's structure under a new name:
//! - numshb: one mesh object per source material, named after the template's first shape (`groupShape468`,
//!   `groupShape468_1`, ...; the level instance's s17 and s23 lists name the model's entries one to one), in
//!   the template object's exact attribute layout (`match_layout`: the shaders bind UVs by the names `map1` and
//!   `lightMap1`); tangents are computed; color sets get the template's average.
//! - nusrcmdlb: the template's modl renamed, with one entry per object and the chosen materials' numatb files.
//! - modelb: one (material, shape) entry per object (`modelb.rs`).
//! - skeleton and animation: renamed copies of the template's (an empty skeleton; an idle animation whose FlatBuffer
//!   names `<model>.nuanmx`, rewritten by `rename_fb_strings`). LODs reuse the full mesh.
//! - materials: the chosen game materials' numatb chunks and their material groups (shared, not copied).
//! The new chunks and group go into a rewritten copy of the archive (`drp.rs`) that the mod loader serves as a whole file.
//! The levelb gets a model instance naming the new group (a clone of a template instance, see `grow.rs`).

use crate::drp::{Archive, Group};
use crate::import::Part;
use glam::{Quat, Vec3};
use ssbh_data::mesh_data::{AttributeData, BoneInfluence, MeshObjectData, VectorData, VertexWeight};
use ssbh_data::modl_data::ModlEntryData;
use ssbh_data::prelude::*;
use std::io::Cursor;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug)]
pub struct Spec {
    /// the user's model file (.glb, .gltf, .obj)
    pub file: PathBuf,
    /// new model group name, e.g. "SM_USR_Crate"
    pub name: String,
    /// template model group in the course, e.g. "SM_ENV_001_1_Rock2"
    pub template: String,
    /// game material label per source material, in the order `import::load` returns the parts; one label applies to all
    pub materials: Vec<String>,
    /// uniform scale applied to the source (glTF is in meters)
    pub scale: f32,
    /// world placement (position, yaw in degrees) baked into the vertices. The course's own scenery is world-baked
    /// geometry under an instance at the origin, and the game culls a model by its mesh bounds as stored: a model whose
    /// vertices sit around the origin and that is moved only by its instance never draws.
    pub place: (Vec3, f32),
}

pub struct Built {
    /// new chunks: (stored name with extension, payload)
    pub chunks: Vec<(String, Vec<u8>)>,
    /// the archive's dependency table with the new group inserted
    pub groups: Vec<Group>,
    /// mesh object (shape) name per part, in model entry order
    pub shapes: Vec<String>,
    pub parts: Vec<(String, String, usize, usize)>, // (source material, game material, vertices, triangles)
    pub bounds: (Vec3, Vec3),
}

/// Game materials of a course archive: labels of the `MI_*.drl` material groups that have a numatb chunk.
pub fn materials(a: &Archive) -> Vec<String> {
    let mut v: Vec<String> = a
        .groups
        .iter()
        .filter_map(|g| g.name.strip_suffix(".drl"))
        .filter(|n| n.starts_with("MI_") && a.find_entry(&format!("{n}.numatb")).is_some())
        .map(str::to_string)
        .collect();
    v.sort();
    v.dedup();
    v
}

/// Model groups usable as templates: groups whose files include a modelb and a modl.
pub fn templates(a: &Archive) -> Vec<String> {
    a.groups
        .iter()
        .filter(|g| {
            let has = |ext: &str| g.idx.iter().any(|&i| a.entries.get(i as usize).is_some_and(|e| e.name.starts_with(&g.name) && e.name.ends_with(ext)));
            !g.name.ends_with(".drl") && !g.name.is_empty() && has(".modelb") && has(".nusrcmdlb")
        })
        .map(|g| g.name.clone())
        .collect()
}

fn rename(s: &str, from: &str, to: &str) -> String {
    match s.strip_prefix(from) {
        Some(rest) => format!("{to}{rest}"),
        None => s.to_string(),
    }
}

fn valid_name(n: &str) -> bool {
    !n.is_empty() && n.len() <= 64 && n.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_')
}

fn mean(v: &VectorData) -> [f32; 4] {
    let mut s = [0f32; 4];
    let n = v.len().max(1) as f32;
    match v {
        VectorData::Vector2(x) => x.iter().for_each(|a| (0..2).for_each(|i| s[i] += a[i])),
        VectorData::Vector3(x) => x.iter().for_each(|a| (0..3).for_each(|i| s[i] += a[i])),
        VectorData::Vector4(x) => x.iter().for_each(|a| (0..4).for_each(|i| s[i] += a[i])),
    }
    s.map(|x| x / n)
}

fn element(v: &VectorData, i: usize) -> [f32; 4] {
    match v {
        VectorData::Vector2(x) => [x[i].x, x[i].y, 0.0, 0.0],
        VectorData::Vector3(x) => [x[i].x, x[i].y, x[i].z, 0.0],
        VectorData::Vector4(x) => x[i].to_array(),
    }
}

/// A lightmap UV set holds atlas coordinates of the template's baked light. The new model has no baked light of its
/// own, so every vertex samples one texel the template itself uses: the one under its most upward-facing vertex
/// (open to the sky, so lit). Spreading the model's own 0..1 UVs over the atlas samples unlit gaps and draws it black.
fn lit_texel(t: &MeshObjectData, uvs: &VectorData) -> [f32; 4] {
    let Some(n) = t.normals.first() else { return mean(uvs) };
    let best = (0..n.data.len().min(uvs.len())).max_by(|&a, &b| element(&n.data, a)[1].total_cmp(&element(&n.data, b)[1]));
    best.map(|i| element(uvs, i)).unwrap_or_else(|| mean(uvs))
}

fn is_lightmap(name: &str) -> bool {
    name.to_ascii_lowercase().contains("lightmap")
}

fn same_shape(like: &VectorData, n: usize, f: impl Fn(usize) -> [f32; 4]) -> VectorData {
    match like {
        VectorData::Vector2(_) => VectorData::Vector2((0..n).map(|i| { let v = f(i); [v[0], v[1]].into() }).collect()),
        VectorData::Vector3(_) => VectorData::Vector3((0..n).map(|i| { let v = f(i); [v[0], v[1], v[2]].into() }).collect()),
        VectorData::Vector4(_) => VectorData::Vector4((0..n).map(|i| f(i).into()).collect()),
    }
}

/// Renames `old` to `new` in the strings of a small FlatBuffer (or returns it unchanged when `old` is absent). Each
/// string that starts with `old` is appended in its new form at the end of the buffer and the one offset field that
/// pointed to the old string is repointed; the old string is zeroed. Errors when a string is not referenced
/// exactly once or `old` appears outside a string.
pub fn rename_fb_strings(b: &[u8], old: &str, new: &str) -> Result<Vec<u8>, String> {
    let rd = |v: &[u8], p: usize| u32::from_le_bytes(v[p..p + 4].try_into().unwrap()) as usize;
    let hits: Vec<usize> = b.windows(old.len()).enumerate().filter(|(_, w)| *w == old.as_bytes()).map(|(i, _)| i).collect();
    if hits.is_empty() {
        return Ok(b.to_vec());
    }
    if b.starts_with(b"HBSS") {
        return Err(format!("SSBH file names {old} inside; not supported"));
    }
    let mut out = b.to_vec();
    for s in hits {
        // a FlatBuffers string: u32 length right before the text, NUL after it, 4-aligned
        if s < 4 || (s - 4) % 4 != 0 {
            return Err(format!("{old} at {s:#x} is not at the start of a string"));
        }
        let (lp, len) = (s - 4, rd(b, s - 4));
        if s + len >= b.len() || b[s + len] != 0 || b[s..s + len].contains(&0) {
            return Err(format!("{old} at {s:#x} is not a whole string"));
        }
        let refs: Vec<usize> = (0..lp).step_by(4).filter(|&p| p + rd(b, p) == lp).collect();
        if refs.len() != 1 {
            return Err(format!("string at {lp:#x} has {} references", refs.len()));
        }
        let text = format!("{new}{}", std::str::from_utf8(&b[s + old.len()..s + len]).map_err(|_| "string is not UTF-8")?);
        while out.len() % 4 != 0 {
            out.push(0);
        }
        let np = out.len();
        out.extend_from_slice(&(text.len() as u32).to_le_bytes());
        out.extend_from_slice(text.as_bytes());
        out.push(0);
        let r = refs[0];
        out[r..r + 4].copy_from_slice(&((np - r) as u32).to_le_bytes());
        out[lp..s + len].fill(0);
    }
    while out.len() % 4 != 0 {
        out.push(0);
    }
    Ok(out)
}

fn decode(t: ssbh_lib::formats::mesh::AttributeDataTypeV10, b: &[u8]) -> [f32; 4] {
    use ssbh_lib::formats::mesh::AttributeDataTypeV10 as T;
    let f = |i: usize| f32::from_le_bytes(b[4 * i..4 * i + 4].try_into().unwrap());
    let h = |i: usize| half::f16::from_le_bytes([b[2 * i], b[2 * i + 1]]).to_f32();
    match t {
        T::Float2 => [f(0), f(1), 0.0, 0.0],
        T::Float3 => [f(0), f(1), f(2), 0.0],
        T::Float4 => [f(0), f(1), f(2), f(3)],
        T::HalfFloat2 => [h(0), h(1), 0.0, 0.0],
        T::HalfFloat4 => [h(0), h(1), h(2), h(3)],
        T::Byte4 => [b[0] as f32 / 255.0, b[1] as f32 / 255.0, b[2] as f32 / 255.0, b[3] as f32 / 255.0],
    }
}

fn encode(t: ssbh_lib::formats::mesh::AttributeDataTypeV10, v: [f32; 4], out: &mut [u8]) {
    use ssbh_lib::formats::mesh::AttributeDataTypeV10 as T;
    let (nf, nh) = match t {
        T::Float2 => (2, 0),
        T::Float3 => (3, 0),
        T::Float4 => (4, 0),
        T::HalfFloat2 => (0, 2),
        T::HalfFloat4 => (0, 4),
        T::Byte4 => {
            for i in 0..4 {
                out[i] = (v[i].clamp(0.0, 1.0) * 255.0).round() as u8;
            }
            return;
        }
    };
    for i in 0..nf {
        out[4 * i..4 * i + 4].copy_from_slice(&v[i].to_le_bytes());
    }
    for i in 0..nh {
        out[2 * i..2 * i + 2].copy_from_slice(&half::f16::from_f32(v[i]).to_le_bytes());
    }
}

fn type_size(t: ssbh_lib::formats::mesh::AttributeDataTypeV10) -> usize {
    use ssbh_lib::formats::mesh::AttributeDataTypeV10 as T;
    match t {
        T::Float2 => 8,
        T::Float3 => 12,
        T::Float4 => 16,
        T::HalfFloat2 => 4,
        T::HalfFloat4 => 8,
        T::Byte4 => 4,
    }
}

/// ssbh_data writes 2-component attributes as HalfFloat2 and names every attribute after its `attribute_names` entry,
/// while the game's shaders bind the template's names (`map1`, `lightMap1`) and layout (Float2 UVs, 16-byte stride).
/// Rewrites every object of `new` to the attribute layout of the template object `shape` (data types, buffers, offsets,
/// strides, names), converting the values. Bounds, indices and rigging stay as ssbh_data wrote them.
fn match_layout(new: &[u8], template: &Path, shape: &str) -> Result<Vec<u8>, String> {
    use ssbh_lib::formats::mesh::*;
    let Mesh::V10(t) = Mesh::from_file(template).map_err(|e| format!("{}: {e}", template.display()))? else {
        return Err("template mesh is not version 1.10".into());
    };
    let Mesh::V10(mut m) = Mesh::read(&mut Cursor::new(new)).map_err(|e| format!("new mesh: {e}"))? else {
        return Err("new mesh is not version 1.10".into());
    };
    let to = t.objects.elements.iter().find(|o| o.name.to_str() == Some(shape)).or(t.objects.elements.first()).ok_or("template mesh is empty")?;
    if to.attributes.elements.iter().any(|a| a.buffer_index > 1) || to.stride2 != 0 || to.stride3 != 0 {
        return Err("template mesh uses vertex buffers 2/3; not supported yet".into());
    }
    let tstride = [to.stride0 as usize, to.stride1 as usize];
    let src: Vec<Vec<u8>> = m.vertex_buffers.elements.iter().map(|b| b.elements.clone()).collect();
    let mut bufs: [Vec<u8>; 2] = [vec![], vec![]];
    for o in m.objects.elements.iter_mut() {
        let n = o.vertex_count as usize;
        let sstride = [o.stride0 as usize, o.stride1 as usize];
        let soff = [o.vertex_buffer0_offset as usize, o.vertex_buffer1_offset as usize];
        let base = [bufs[0].len(), bufs[1].len()];
        for b in 0..2 {
            bufs[b].resize(base[b] + n * tstride[b], 0);
        }
        for ta in &to.attributes.elements {
            let sa = o
                .attributes
                .elements
                .iter()
                .find(|a| a.usage == ta.usage && a.subindex == ta.subindex)
                .ok_or_else(|| format!("new mesh lacks the template's {:?} {}", ta.usage, ta.subindex))?;
            let (sb, tb) = (sa.buffer_index as usize, ta.buffer_index as usize);
            if sb > 1 {
                return Err("new mesh uses vertex buffers 2/3".into());
            }
            let (ss, ts) = (type_size(sa.data_type), type_size(ta.data_type));
            for v in 0..n {
                let s = soff[sb] + sstride[sb] * v + sa.buffer_offset as usize;
                let val = decode(sa.data_type, src[sb].get(s..s + ss).ok_or("new mesh buffer too short")?);
                let d = base[tb] + tstride[tb] * v + ta.buffer_offset as usize;
                encode(ta.data_type, val, &mut bufs[tb][d..d + ts]);
            }
        }
        o.attributes = to.attributes.clone();
        o.stride0 = to.stride0;
        o.stride1 = to.stride1;
        o.vertex_buffer0_offset = base[0] as u32;
        o.vertex_buffer1_offset = base[1] as u32;
    }
    m.buffer_sizes.elements = vec![bufs[0].len() as u32, bufs[1].len() as u32, 0, 0];
    let [b0, b1] = bufs;
    m.vertex_buffers.elements = vec![ssbh_lib::SsbhByteBuffer { elements: b0 }, ssbh_lib::SsbhByteBuffer { elements: b1 }, ssbh_lib::SsbhByteBuffer::new(), ssbh_lib::SsbhByteBuffer::new()];
    let mut out = Cursor::new(vec![]);
    Mesh::V10(m).write(&mut out).map_err(|e| format!("numshb rewrite: {e}"))?;
    Ok(out.into_inner())
}

fn mesh_object(t: &MeshObjectData, shape: &str, sub: u64, p: &Part) -> Result<MeshObjectData, String> {
    let n = p.positions.len();
    let pos = VectorData::Vector3(p.positions.iter().map(|v| (*v).into()).collect());
    let normals: Vec<[f32; 4]> = p.normals.iter().map(|v| [v[0], v[1], v[2], 0.0]).collect();
    let uv = VectorData::Vector2(p.uvs.iter().map(|v| (*v).into()).collect());
    let nrm = VectorData::Vector4(normals.iter().map(|v| (*v).into()).collect());
    let tang: Vec<[f32; 4]> = ssbh_data::mesh_data::calculate_tangents_vec4(&pos, &nrm, &uv, &p.indices)
        .map_err(|e| format!("tangents: {e}"))?
        .iter()
        .map(|v| v.to_array())
        .collect();
    let mut o = MeshObjectData {
        name: shape.to_string(),
        subindex: sub,
        parent_bone_name: t.parent_bone_name.clone(),
        sort_bias: t.sort_bias,
        disable_depth_write: t.disable_depth_write,
        disable_depth_test: t.disable_depth_test,
        vertex_indices: p.indices.clone(),
        ..Default::default()
    };
    o.positions = t.positions.iter().map(|a| AttributeData { name: a.name.clone(), data: same_shape(&a.data, n, |i| { let v = p.positions[i]; [v[0], v[1], v[2], 1.0] }) }).collect();
    o.normals = t.normals.iter().map(|a| AttributeData { name: a.name.clone(), data: same_shape(&a.data, n, |i| normals[i]) }).collect();
    o.tangents = t.tangents.iter().map(|a| AttributeData { name: a.name.clone(), data: same_shape(&a.data, n, |i| tang[i]) }).collect();
    o.binormals = t
        .binormals
        .iter()
        .map(|a| {
            let data = same_shape(&a.data, n, |i| {
                let (nv, tv) = (Vec3::from_slice(&normals[i][..3]), Vec3::from_slice(&tang[i][..3]));
                let b = nv.cross(tv) * tang[i][3];
                [b.x, b.y, b.z, 0.0]
            });
            AttributeData { name: a.name.clone(), data }
        })
        .collect();
    o.texture_coordinates = t
        .texture_coordinates
        .iter()
        .map(|a| {
            let data = if is_lightmap(&a.name) {
                let lit = lit_texel(t, &a.data);
                same_shape(&a.data, n, |_| lit)
            } else {
                same_shape(&a.data, n, |i| { let v = p.uvs[i]; [v[0], v[1], 0.0, 0.0] })
            };
            AttributeData { name: a.name.clone(), data }
        })
        .collect();
    o.color_sets = t
        .color_sets
        .iter()
        .map(|a| {
            let m = mean(&a.data);
            AttributeData { name: a.name.clone(), data: same_shape(&a.data, n, |_| m) }
        })
        .collect();
    if let Some(b) = t.bone_influences.first() {
        o.bone_influences = vec![BoneInfluence {
            bone_name: b.bone_name.clone(),
            vertex_weights: (0..n as u32).map(|i| VertexWeight { vertex_index: i, vertex_weight: 1.0 }).collect(),
        }];
    }
    if o.positions.is_empty() {
        return Err("template mesh object has no positions".into());
    }
    Ok(o)
}

/// Builds the new group's files and the archive's new dependency table. `unpacked` is the course's unpacked archive
/// folder (`extracted/drp-v262144/<archive>/`), where the template's files are read.
pub fn build(spec: &Spec, a: &Archive, unpacked: &Path) -> Result<Built, String> {
    if !valid_name(&spec.name) {
        return Err(format!("model name {:?}: use letters, digits and _ (up to 64)", spec.name));
    }
    if a.groups.iter().any(|g| g.name.eq_ignore_ascii_case(&spec.name)) || a.entries.iter().any(|e| e.name.to_ascii_lowercase().starts_with(&format!("{}.", spec.name.to_ascii_lowercase())) || e.name.to_ascii_lowercase().starts_with(&format!("{}_", spec.name.to_ascii_lowercase()))) {
        return Err(format!("{} is already used in this course", spec.name));
    }
    if !(spec.scale.is_finite() && spec.scale > 0.0) {
        return Err("scale must be positive".into());
    }
    let mut parts = crate::import::load(&spec.file)?;
    let (at, yaw) = spec.place;
    let rot = Quat::from_rotation_y(yaw.to_radians());
    for p in &mut parts {
        for v in &mut p.positions {
            *v = (rot * (Vec3::from(*v) * spec.scale) + at).to_array();
        }
        for n in &mut p.normals {
            *n = (rot * Vec3::from(*n)).to_array();
        }
    }
    let gi = a.find_group(&spec.template).ok_or_else(|| format!("no model group {} in this course", spec.template))?;
    let parent = a.parents()[gi].ok_or("template group has no parent")?;
    let tg = &a.groups[gi];
    let tname = |i: &u32| a.entries[*i as usize].name.clone();
    let files: Vec<String> = tg.idx.iter().map(tname).collect();
    let modl_name = files
        .iter()
        .filter(|f| f.starts_with(&spec.template) && f.ends_with(".nusrcmdlb"))
        .min_by_key(|f| f.len())
        .ok_or("template has no nusrcmdlb")?
        .clone();
    let modl = ModlData::from_file(unpacked.join(&modl_name)).map_err(|e| format!("{modl_name}: {e}"))?;
    let tmesh = MeshData::from_file(unpacked.join(&modl.mesh_file_name)).map_err(|e| format!("{}: {e}", modl.mesh_file_name))?;
    let tmodelb = std::fs::read(unpacked.join(format!("{}.modelb", spec.template))).map_err(|e| format!("template modelb: {e}"))?;
    let shape = crate::fb::Buf(&tmodelb)
        .root()
        .and_then(|r| r.tables(3).first().and_then(|e| e.str(0)).map(str::to_string))
        .ok_or("template modelb has no shape")?;
    let tobj = tmesh.objects.iter().find(|o| o.name == shape).or(tmesh.objects.first()).ok_or("template mesh is empty")?;

    // materials
    if spec.materials.is_empty() {
        return Err("choose a game material".into());
    }
    // "source=MI_x" pairs map by source material name; plain labels go in source-name order, one label covers all
    let names = parts.iter().map(|p| p.material.as_str()).collect::<Vec<_>>().join(", ");
    let pairs: Vec<(&str, &str)> = spec.materials.iter().filter_map(|m| m.split_once('=')).collect();
    let mapped: Vec<String> = if !pairs.is_empty() {
        if pairs.len() != spec.materials.len() {
            return Err("mix of source=material pairs and plain materials".into());
        }
        if let Some((s, _)) = pairs.iter().find(|(s, _)| !parts.iter().any(|p| p.material == *s)) {
            return Err(format!("the model has no material {s} (it has {names})"));
        }
        parts
            .iter()
            .map(|p| pairs.iter().find(|(s, _)| *s == p.material).map(|(_, m)| m.to_string()).ok_or_else(|| format!("no game material given for {}", p.material)))
            .collect::<Result<_, _>>()?
    } else if spec.materials.len() == 1 {
        vec![spec.materials[0].clone(); parts.len()]
    } else if spec.materials.len() == parts.len() {
        spec.materials.clone()
    } else {
        return Err(format!("the model has {} materials ({names}); give 1 material, {} in that order, or source=material pairs", parts.len(), parts.len()));
    };
    let mat_of = |i: usize| mapped[i].clone();
    let mut mats: Vec<String> = vec![];
    for i in 0..parts.len() {
        let m = mat_of(i);
        if !mats.contains(&m) {
            mats.push(m);
        }
    }
    let mut mat_chunks = vec![];
    let mut mat_groups = vec![];
    for m in &mats {
        let c = a.find_entry(&format!("{m}.numatb")).ok_or_else(|| format!("no game material {m} in this course"))?;
        let g = a.find_group(&format!("{m}.drl")).ok_or_else(|| format!("material {m} has no material group"))?;
        mat_chunks.push(c as u32);
        mat_groups.push(a.groups[g].clone());
    }

    // mesh
    // one shape name per part: the template's first, then numbered copies of it
    let shapes: Vec<String> = (0..parts.len()).map(|i| if i == 0 { shape.clone() } else { format!("{shape}_{i}") }).collect();
    let mut mesh = MeshData { major_version: tmesh.major_version, minor_version: tmesh.minor_version, objects: vec![] };
    for (p, s) in parts.iter().zip(&shapes) {
        mesh.objects.push(mesh_object(tobj, s, 0, p)?);
    }
    let mut cur = Cursor::new(vec![]);
    mesh.write(&mut cur).map_err(|e| format!("numshb write: {e}"))?;
    let mesh_bytes = match_layout(&cur.into_inner(), &unpacked.join(&modl.mesh_file_name), &shape)?;
    let back = MeshData::read(&mut Cursor::new(&mesh_bytes)).map_err(|e| format!("numshb re-read: {e}"))?;
    if back.objects.len() != parts.len() || back.objects.iter().zip(&parts).any(|(o, p)| o.vertex_count().ok() != Some(p.positions.len()) || o.vertex_indices != p.indices) {
        return Err("numshb re-read differs".into());
    }
    // positions and the first UV set survive the layout pass
    let v2 = |d: &VectorData| -> Vec<[f32; 4]> {
        match d {
            VectorData::Vector2(v) => v.iter().map(|x| [x.x, x.y, 0.0, 0.0]).collect(),
            VectorData::Vector3(v) => v.iter().map(|x| [x.x, x.y, x.z, 0.0]).collect(),
            VectorData::Vector4(v) => v.iter().map(|x| x.to_array()).collect(),
        }
    };
    for (o, p) in back.objects.iter().zip(&parts) {
        let pos = o.positions.first().map(|a| v2(&a.data)).unwrap_or_default();
        let uv = o.texture_coordinates.first().map(|a| v2(&a.data)).unwrap_or_default();
        let ok = pos.len() == p.positions.len()
            && pos.iter().zip(&p.positions).all(|(a, b)| (0..3).all(|k| (a[k] - b[k]).abs() < 1e-4))
            && uv.len() == p.uvs.len()
            && uv.iter().zip(&p.uvs).all(|(a, b)| (0..2).all(|k| (a[k] - b[k]).abs() < 2e-3));
        if !ok {
            return Err("numshb re-read: positions or UVs differ".into());
        }
    }

    let modelb_bytes = crate::modelb::build(&tmodelb, &(0..parts.len()).map(|i| (mat_of(i), shapes[i].clone())).collect::<Vec<_>>())?;
    let rn = |s: &str| rename(s, &spec.template, &spec.name);

    let mut chunks: Vec<(String, Vec<u8>)> = vec![];
    let mut idx: Vec<u32> = vec![];
    let mut mats_placed = false;
    let first_new = a.entries.len() as u32;
    for (k, f) in files.iter().enumerate() {
        if !f.starts_with(&spec.template) {
            // the template's materials: replaced by the chosen ones, at the first material's place
            if f.ends_with(".numatb") {
                if !mats_placed {
                    idx.extend(&mat_chunks);
                    mats_placed = true;
                }
            } else {
                idx.push(tg.idx[k]);
            }
            continue;
        }
        let data = if f.ends_with(".nusktb") || f.ends_with(".nuanmb") {
            // static scenery: an empty skeleton and an idle animation that names itself (`<model>.nuanmx`); the model gets
            // its own renamed copies, as the game's own groups do
            let b = std::fs::read(unpacked.join(f)).map_err(|e| format!("{f}: {e}"))?;
            rename_fb_strings(&b, &spec.template, &spec.name).map_err(|e| format!("{f}: {e}"))?
        } else if f.ends_with(".numshb") {
            mesh_bytes.clone()
        } else if f.ends_with(".nusrcmdlb") {
            let src = ModlData::from_file(unpacked.join(f)).map_err(|e| format!("{f}: {e}"))?;
            let m = ModlData {
                major_version: src.major_version,
                minor_version: src.minor_version,
                model_name: rn(&src.model_name),
                skeleton_file_name: rn(&src.skeleton_file_name),
                material_file_names: mats.iter().map(|m| format!("{m}.numatb")).collect(),
                animation_file_name: src.animation_file_name.as_deref().map(|s| rn(s)),
                mesh_file_name: rn(&src.mesh_file_name),
                entries: (0..parts.len()).map(|i| ModlEntryData { mesh_object_name: shapes[i].clone(), mesh_object_subindex: 0, material_label: mat_of(i) }).collect(),
            };
            let mut c = Cursor::new(vec![]);
            m.write(&mut c).map_err(|e| format!("modl write: {e}"))?;
            c.into_inner()
        } else if f.ends_with(".modelb") {
            modelb_bytes.clone()
        } else {
            let b = std::fs::read(unpacked.join(f)).map_err(|e| format!("{f}: {e}"))?;
            if b.windows(spec.template.len()).any(|w| w == spec.template.as_bytes()) {
                return Err(format!("{f} names its model inside; this template cannot be renamed yet"));
            }
            b
        };
        idx.push(first_new + chunks.len() as u32);
        chunks.push((rn(f), data));
    }
    if !mats_placed {
        idx.extend(&mat_chunks);
    }
    let group = Group { magic: tg.magic, kids: mat_groups.len() as u32, name: spec.name.clone(), idx };
    let mut groups = a.groups.clone();
    let at = a.subtree_end(gi);
    let mut sub = vec![group];
    sub.extend(mat_groups);
    groups.splice(at..at, sub);
    groups[parent].kids += 1;

    let mut lo = Vec3::splat(f32::MAX);
    let mut hi = Vec3::splat(f32::MIN);
    for p in &parts {
        let (l, h) = p.bounds();
        lo = lo.min(l);
        hi = hi.max(h);
    }
    let parts_info = parts.iter().enumerate().map(|(i, p)| (p.material.clone(), mat_of(i), p.positions.len(), p.indices.len() / 3)).collect();
    Ok(Built { chunks, groups, shapes, parts: parts_info, bounds: (lo, hi) })
}

/// The template's model instance to clone for the new model: the first instance whose mesh is the template group.
pub fn template_instance(lc: &crate::course::LoadedCourse, template: &str) -> Option<(usize, usize)> {
    for (li, l) in lc.levels.iter().enumerate() {
        let root = crate::fb::Buf(&l.data).root()?;
        for (ei, e) in root.tables(2).iter().enumerate() {
            if e.tables(0).iter().filter_map(|c| c.table(5)).any(|t| t.str(1).is_some_and(|n| n.eq_ignore_ascii_case(template))) {
                return Some((li, ei));
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A buffer with one offset field at 4 pointing at the string "SM_A_tail" (length word at 8).
    fn one_string() -> Vec<u8> {
        let mut b = vec![0u8; 8];
        b[4..8].copy_from_slice(&4u32.to_le_bytes());
        b.extend_from_slice(&9u32.to_le_bytes());
        b.extend_from_slice(b"SM_A_tail\0");
        while b.len() % 4 != 0 {
            b.push(0);
        }
        b
    }

    fn read_ref(b: &[u8], field: usize) -> String {
        let lp = field + u32::from_le_bytes(b[field..field + 4].try_into().unwrap()) as usize;
        let len = u32::from_le_bytes(b[lp..lp + 4].try_into().unwrap()) as usize;
        String::from_utf8(b[lp + 4..lp + 4 + len].to_vec()).unwrap()
    }

    #[test]
    fn rename_repoints_the_one_reference() {
        let out = rename_fb_strings(&one_string(), "SM_A", "SM_Longer").unwrap();
        assert_eq!(read_ref(&out, 4), "SM_Longer_tail");
        assert!(!out.windows(4).any(|w| w == b"SM_A"), "old text zeroed");
        assert_eq!(out.len() % 4, 0);
    }

    #[test]
    fn rename_leaves_unrelated_buffers_alone() {
        let b = one_string();
        assert_eq!(rename_fb_strings(&b, "SM_Z", "SM_Y").unwrap(), b);
    }

    #[test]
    fn rename_refuses_ambiguous_references() {
        let mut b = one_string();
        b[0..4].copy_from_slice(&8u32.to_le_bytes()); // a second field pointing at the same string
        assert!(rename_fb_strings(&b, "SM_A", "SM_B").is_err());
    }

    #[test]
    fn lightmap_uvs_take_the_most_upward_texel() {
        let t = MeshObjectData {
            normals: vec![AttributeData { name: "Normal0".into(), data: VectorData::Vector3(vec![[1.0, 0.0, 0.0].into(), [0.0, 1.0, 0.0].into(), [0.0, -1.0, 0.0].into()]) }],
            ..Default::default()
        };
        let uvs = VectorData::Vector2(vec![[0.1, 0.1].into(), [0.7, 0.3].into(), [0.9, 0.9].into()]);
        assert_eq!(lit_texel(&t, &uvs), [0.7, 0.3, 0.0, 0.0]);
        assert!(is_lightmap("LightMapTextureCoordinate1") && is_lightmap("lightMap1") && !is_lightmap("map1"));
    }
}
