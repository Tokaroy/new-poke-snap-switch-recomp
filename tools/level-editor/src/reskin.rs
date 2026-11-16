//! Pokémon reskin: a user's model replaces the meshes of an existing species, while the species' skeleton, animations,
//! materials, sounds and behavior stay as they are.
//!
//! A species pack (`nx/Pack/Characters/master/CH_nnn.drp`) holds a model `SK_CH_nnn_00_0_A00` (and LODs): a skeleton
//! (`nusktb`), meshes (`numshb`) and model files (`nusrcmdlb`) that give each mesh object, by name and subindex, its
//! material. The reskin writes new `numshb` files with the same object names and subindices, so the model files,
//! skeleton and every animation keep working; only replaced entries go into the pack, as loose files.
//!
//! Each part of the user's model (one per source material) goes to one mesh object:
//! - `map` names it (`source material=object`);
//! - else the object whose material has the part's source material name (a model exported from the species itself);
//! - else the species' biggest object.
//!
//! Objects listed in `keep` stay as they are (eyes, for example); every other object with no part becomes one
//! zero-area triangle, so it draws nothing.
//!
//! Skinning: a glTF skin's joints map to the species' bones by name. A vertex whose joints match no bone, and every
//! vertex of an unskinned model, follows the nearest bone the species' own mesh is skinned to (rigid parts). The user's
//! texture coordinates sample the species' own textures; new textures go through a texture pack.

use crate::import::{self, Part};
use crate::scenery;
use glam::Vec3;
use ssbh_data::mesh_data::{BoneInfluence, MeshObjectData, VertexWeight};
use ssbh_data::modl_data::ModlData;
use ssbh_data::prelude::*;
use std::collections::BTreeMap;
use std::io::Cursor;
use std::path::{Path, PathBuf};

pub const CHAR_DIR: &str = "nx/Pack/Characters/master";

#[derive(Clone, Debug, Default)]
pub struct Spec {
    /// the user's model file (.glb, .gltf, .obj)
    pub file: PathBuf,
    /// species pack, e.g. "CH_007"
    pub species: String,
    /// model in the pack, e.g. "SK_CH_007_00_0_A00"; the first `SK_*` with a mesh when empty
    pub model: String,
    /// uniform scale applied to the source
    pub scale: f32,
    /// mesh object names to leave as they are
    pub keep: Vec<String>,
    /// (source material, mesh object name)
    pub map: Vec<(String, String)>,
}

pub struct Reskin {
    /// archive path of the species pack, e.g. `nx/Pack/Characters/master/CH_007.drp`
    pub archive: String,
    /// (entry file name, bytes)
    pub files: Vec<(String, Vec<u8>)>,
    pub report: Vec<String>,
}

/// One mesh object of a model file: name, subindex, material label, vertex count, skinned.
pub struct ObjectInfo {
    pub name: String,
    pub subindex: u64,
    pub material: String,
    pub vertices: usize,
    pub skinned: bool,
}

/// Species packs in the unpacked data (`CH_nnn`).
pub fn species(data_root: &Path) -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(data_root.join(CHAR_DIR))
        .map(|d| d.filter_map(|e| e.ok()).filter_map(|e| e.file_name().to_str().and_then(|n| n.strip_suffix(".drp")).map(str::to_string)).filter(|n| n.starts_with("CH_")).collect())
        .unwrap_or_default();
    v.sort();
    v
}

/// Models of a species pack (`SK_*` with a model file that names an existing mesh).
pub fn models(unpacked: &Path) -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(unpacked)
        .map(|d| {
            d.filter_map(|e| e.ok())
                .filter_map(|e| e.file_name().to_str().and_then(|n| n.strip_suffix(".nusktb")).map(str::to_string))
                .filter(|n| n.starts_with("SK_") && unpacked.join(format!("{n}.numshb")).is_file())
                .collect()
        })
        .unwrap_or_default();
    v.sort();
    v
}

/// The model files of `model` (main, then LODs) with their meshes: (modl file name, mesh file name, modl).
fn model_files(dir: &Path, model: &str) -> Result<Vec<(String, String, ModlData)>, String> {
    let mut out = vec![];
    for stem in [model.to_string(), format!("{model}_LOD1"), format!("{model}_LOD2"), format!("{model}_LOD3")] {
        let f = format!("{stem}.nusrcmdlb");
        if !dir.join(&f).is_file() {
            continue;
        }
        let m = ModlData::from_file(dir.join(&f)).map_err(|e| format!("{f}: {e}"))?;
        if dir.join(&m.mesh_file_name).is_file() {
            out.push((f, m.mesh_file_name.clone(), m));
        }
    }
    if out.is_empty() {
        return Err(format!("{model}: no model file with a mesh"));
    }
    Ok(out)
}

fn material_of(m: &ModlData, name: &str, sub: u64) -> String {
    m.entries.iter().find(|e| e.mesh_object_name == name && e.mesh_object_subindex == sub).map(|e| e.material_label.clone()).unwrap_or_default()
}

/// The mesh objects of a species' main model, for choosing what to keep or map.
pub fn objects(data_root: &Path, species: &str, model: &str) -> Result<(String, Vec<ObjectInfo>), String> {
    let dir = data_root.join(CHAR_DIR).join(format!("{species}.drp"));
    let model = if model.is_empty() { models(&dir).into_iter().next().ok_or_else(|| format!("{species}: no SK_ model"))? } else { model.to_string() };
    let (_, mesh_file, modl) = model_files(&dir, &model)?.remove(0);
    let mesh = MeshData::from_file(dir.join(&mesh_file)).map_err(|e| format!("{mesh_file}: {e}"))?;
    let v = mesh
        .objects
        .iter()
        .map(|o| ObjectInfo {
            name: o.name.clone(),
            subindex: o.subindex,
            material: material_of(&modl, &o.name, o.subindex),
            vertices: o.positions.first().map(|a| a.data.len()).unwrap_or(0),
            skinned: !o.bone_influences.is_empty(),
        })
        .collect();
    Ok((model, v))
}

/// Joins parts into one (joint lists merged by name).
fn merge(parts: &[&Part]) -> Part {
    let mut out = Part { material: parts.iter().map(|p| p.material.as_str()).collect::<Vec<_>>().join("+"), ..Default::default() };
    let skinned = parts.iter().any(|p| !p.joints.is_empty());
    for p in parts {
        let base = out.positions.len() as u32;
        out.positions.extend_from_slice(&p.positions);
        out.normals.extend_from_slice(&p.normals);
        out.uvs.extend_from_slice(&p.uvs);
        out.indices.extend(p.indices.iter().map(|i| i + base));
        if skinned {
            let remap: Vec<u16> = p
                .joints
                .iter()
                .map(|n| match out.joints.iter().position(|x| x == n) {
                    Some(i) => i as u16,
                    None => {
                        out.joints.push(n.clone());
                        (out.joints.len() - 1) as u16
                    }
                })
                .collect();
            if p.weights.len() == p.positions.len() {
                out.weights.extend(p.weights.iter().map(|w| w.map(|(j, x)| (remap.get(j as usize).copied().unwrap_or(0), x))));
            } else {
                out.weights.extend(std::iter::repeat([(0u16, 0f32); 4]).take(p.positions.len()));
            }
        }
    }
    out
}

/// Bone weights for every vertex of `p`: the glTF skin's joints by bone name, else the nearest of `deform`. Returns the
/// influences and how many vertices fell back to the nearest bone.
fn influences(p: &Part, bones: &[String], deform: &[(String, Vec3)]) -> (Vec<BoneInfluence>, usize) {
    let bone_of: Vec<Option<usize>> = p.joints.iter().map(|j| bones.iter().position(|b| b.eq_ignore_ascii_case(j))).collect();
    let mut per: BTreeMap<usize, Vec<VertexWeight>> = BTreeMap::new();
    let mut fallback = 0;
    for (v, pos) in p.positions.iter().enumerate() {
        let mut ws: Vec<(usize, f32)> = vec![];
        if let Some(w) = p.weights.get(v) {
            for &(j, x) in w {
                if x > 0.0 {
                    if let Some(Some(b)) = bone_of.get(j as usize) {
                        match ws.iter_mut().find(|e| e.0 == *b) {
                            Some(e) => e.1 += x,
                            None => ws.push((*b, x)),
                        }
                    }
                }
            }
        }
        let total: f32 = ws.iter().map(|e| e.1).sum();
        if total <= 1e-6 {
            fallback += 1;
            let at = Vec3::from(*pos);
            let near = deform.iter().min_by(|a, b| a.1.distance_squared(at).total_cmp(&b.1.distance_squared(at)));
            ws = near.and_then(|(n, _)| bones.iter().position(|b| b == n)).map(|b| vec![(b, 1.0)]).unwrap_or_default();
        } else {
            ws.iter_mut().for_each(|e| e.1 /= total);
        }
        for (b, x) in ws {
            per.entry(b).or_default().push(VertexWeight { vertex_index: v as u32, vertex_weight: x });
        }
    }
    (per.into_iter().map(|(b, vertex_weights)| BoneInfluence { bone_name: bones[b].clone(), vertex_weights }).collect(), fallback)
}

/// A part with one zero-area triangle at `at`: the object stays (its material and model entries still match) but
/// draws nothing.
fn hidden_part(at: Vec3) -> Part {
    Part {
        material: "hidden".into(),
        positions: vec![at.to_array(); 3],
        normals: vec![[0.0, 1.0, 0.0]; 3],
        uvs: vec![[0.0, 0.0], [1.0, 0.0], [0.0, 1.0]],
        indices: vec![0, 1, 2],
        ..Default::default()
    }
}

pub fn build(spec: &Spec, data_root: &Path) -> Result<Reskin, String> {
    let archive = format!("{CHAR_DIR}/{}.drp", spec.species);
    let dir = data_root.join(&archive);
    if !dir.is_dir() {
        return Err(format!("species pack {} not found under {}", spec.species, data_root.join(CHAR_DIR).display()));
    }
    let model = if spec.model.is_empty() { models(&dir).into_iter().next().ok_or_else(|| format!("{}: no SK_ model", spec.species))? } else { spec.model.clone() };
    let skel = SkelData::from_file(dir.join(format!("{model}.nusktb"))).map_err(|e| format!("{model}.nusktb: {e}"))?;
    let bones: Vec<String> = skel.bones.iter().map(|b| b.name.clone()).collect();
    let bone_pos: BTreeMap<String, Vec3> =
        skel.bones.iter().filter_map(|b| skel.calculate_world_transform(b).ok().map(|m| (b.name.clone(), m.w_axis.truncate()))).collect();
    let mut parts = import::load(&spec.file)?;
    if spec.scale != 1.0 {
        for p in &mut parts {
            p.positions.iter_mut().for_each(|v| *v = (Vec3::from(*v) * spec.scale).to_array());
        }
    }
    let mut rep = vec![format!("{} ({} bones): model {model}", spec.species, bones.len())];
    let skinned = parts.iter().any(|p| !p.joints.is_empty());
    if skinned {
        let names: std::collections::BTreeSet<&String> = parts.iter().flat_map(|p| p.joints.iter()).collect();
        let unmatched: Vec<&&String> = names.iter().filter(|j| !bones.iter().any(|b| b.eq_ignore_ascii_case(j))).collect();
        rep.push(format!("skin: {} joints, {} match the species' bones{}", names.len(), names.len() - unmatched.len(), if unmatched.is_empty() { String::new() } else { format!(" (not matched: {})", unmatched.iter().take(8).map(|s| s.as_str()).collect::<Vec<_>>().join(", ")) }));
    } else {
        rep.push("no skin: every vertex follows the nearest bone of the species' mesh".into());
    }
    let keep = |n: &str| spec.keep.iter().any(|k| k.eq_ignore_ascii_case(n));
    let mut files = vec![];
    for (_, mesh_file, modl) in model_files(&dir, &model)? {
        let path = dir.join(&mesh_file);
        let orig = MeshData::from_file(&path).map_err(|e| format!("{mesh_file}: {e}"))?;
        // bones the species' own mesh deforms with: rigid parts follow the nearest of these
        let mut used: Vec<String> = orig.objects.iter().flat_map(|o| o.bone_influences.iter().map(|b| b.bone_name.clone())).collect();
        used.sort();
        used.dedup();
        let deform: Vec<(String, Vec3)> = used.iter().filter_map(|n| bone_pos.get(n).map(|p| (n.clone(), *p))).collect();
        if deform.is_empty() {
            return Err(format!("{mesh_file}: the species' mesh is not skinned"));
        }
        // the biggest replaceable object takes parts nothing else claims
        let main = orig
            .objects
            .iter()
            .enumerate()
            .filter(|(_, o)| !keep(&o.name))
            .max_by_key(|(_, o)| o.positions.first().map(|a| a.data.len()).unwrap_or(0))
            .map(|(i, _)| i)
            .ok_or("every mesh object is kept; nothing to replace")?;
        let mut assign: Vec<Vec<&Part>> = vec![vec![]; orig.objects.len()];
        for p in &parts {
            let by_map = spec.map.iter().find(|(m, _)| m.eq_ignore_ascii_case(&p.material)).and_then(|(_, o)| orig.objects.iter().position(|x| x.name.eq_ignore_ascii_case(o) && !keep(&x.name)));
            let by_material = || {
                orig.objects
                    .iter()
                    .enumerate()
                    .filter(|(_, o)| !keep(&o.name) && material_of(&modl, &o.name, o.subindex).eq_ignore_ascii_case(&p.material))
                    .max_by_key(|(_, o)| o.positions.first().map(|a| a.data.len()).unwrap_or(0))
                    .map(|(i, _)| i)
            };
            let to = by_map.or_else(by_material).unwrap_or(main);
            assign[to].push(p);
        }
        let mut objects = vec![];
        let mut fell = 0;
        for (i, o) in orig.objects.iter().enumerate() {
            let label = format!("{}.{}", o.name, o.subindex);
            if keep(&o.name) {
                objects.push(o.clone());
                rep.push(format!("  {mesh_file} {label}: kept"));
                continue;
            }
            let (part, what) = if assign[i].is_empty() {
                let at = o.bone_influences.first().and_then(|b| bone_pos.get(&b.bone_name)).copied().unwrap_or(deform[0].1);
                (hidden_part(at), "hidden".to_string())
            } else {
                let m = merge(&assign[i]);
                let w = format!("{} ({} vertices, {} triangles)", m.material, m.positions.len(), m.indices.len() / 3);
                (m, w)
            };
            let mut n: MeshObjectData = scenery::mesh_object(o, &o.name, o.subindex, &part)?;
            if o.bone_influences.is_empty() {
                // a single-bound object follows its parent bone; keep it that way
                n.bone_influences.clear();
            } else {
                let (inf, f) = influences(&part, &bones, &deform);
                if what != "hidden" {
                    fell += f;
                }
                n.bone_influences = inf;
            }
            rep.push(format!("  {mesh_file} {label}: {what}"));
            objects.push(n);
        }
        if fell > 0 {
            rep.push(format!("  {mesh_file}: {fell} vertices follow the nearest bone"));
        }
        let mut cur = Cursor::new(vec![]);
        MeshData { major_version: orig.major_version, minor_version: orig.minor_version, objects }.write(&mut cur).map_err(|e| format!("numshb write: {e}"))?;
        let bytes = scenery::match_layout(&cur.into_inner(), &path, None)?;
        let back = MeshData::read(&mut Cursor::new(&bytes)).map_err(|e| format!("numshb re-read: {e}"))?;
        let same = back.objects.len() == orig.objects.len() && back.objects.iter().zip(&orig.objects).all(|(a, b)| a.name == b.name && a.subindex == b.subindex);
        if !same {
            return Err(format!("{mesh_file}: written mesh objects differ from the original's"));
        }
        files.push((mesh_file, bytes));
    }
    Ok(Reskin { archive, files, report: rep })
}

/// Writes a reskin into the pack `<dir>/<pack>` as loose files, adds the pack's `pack.json` and switches it on.
pub fn write_pack(r: &Reskin, dir: &Path, pack: &str, description: &str) -> Result<Vec<PathBuf>, String> {
    let out = dir.join(crate::edit::sanitize_pack(pack)).join(r.archive.replace('/', std::path::MAIN_SEPARATOR_STR));
    std::fs::create_dir_all(&out).map_err(|e| format!("{}: {e}", out.display()))?;
    let mut written = vec![];
    for (n, b) in &r.files {
        let f = out.join(n);
        std::fs::write(&f, b).map_err(|e| format!("{}: {e}", f.display()))?;
        written.push(f);
    }
    written.extend(crate::edit::write_pack_meta(dir, pack, description)?);
    Ok(written)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn skin_maps_by_name_and_falls_back_to_nearest() {
        let p = Part {
            positions: vec![[0.0, 0.0, 0.0], [0.0, 5.0, 0.0], [0.0, 9.0, 0.0]],
            normals: vec![[0.0, 1.0, 0.0]; 3],
            uvs: vec![[0.0, 0.0]; 3],
            indices: vec![0, 1, 2],
            joints: vec!["Hip".into(), "Unknown".into(), "Head".into()],
            weights: vec![[(0, 0.5), (2, 0.5), (0, 0.0), (0, 0.0)], [(1, 1.0), (0, 0.0), (0, 0.0), (0, 0.0)], [(2, 1.0), (0, 0.0), (0, 0.0), (0, 0.0)]],
            ..Default::default()
        };
        let bones: Vec<String> = ["Trans", "Hip", "Head"].map(String::from).to_vec();
        let deform = vec![("Hip".to_string(), Vec3::ZERO), ("Head".to_string(), Vec3::new(0.0, 10.0, 0.0))];
        let (inf, fell) = influences(&p, &bones, &deform);
        assert_eq!(fell, 1);
        let w = |b: &str, v: u32| inf.iter().find(|i| i.bone_name == b).and_then(|i| i.vertex_weights.iter().find(|x| x.vertex_index == v)).map(|x| x.vertex_weight);
        assert_eq!(w("Hip", 0), Some(0.5));
        assert_eq!(w("Head", 0), Some(0.5));
        // vertex 1's only joint is unknown: nearest deform bone (Hip at 0 is 5 away, Head at 10 is 5 away; first wins)
        assert_eq!(w("Hip", 1).or(w("Head", 1)), Some(1.0));
        assert_eq!(w("Head", 2), Some(1.0));
        assert!(inf.iter().all(|i| i.bone_name != "Trans"));
    }

    #[test]
    fn merge_keeps_joint_names() {
        let a = Part { positions: vec![[0.0; 3]], normals: vec![[0.0; 3]], uvs: vec![[0.0; 2]], joints: vec!["A".into()], weights: vec![[(0, 1.0), (0, 0.0), (0, 0.0), (0, 0.0)]], ..Default::default() };
        let b = Part { positions: vec![[1.0; 3]], normals: vec![[0.0; 3]], uvs: vec![[0.0; 2]], joints: vec!["B".into(), "A".into()], weights: vec![[(1, 1.0), (0, 0.0), (0, 0.0), (0, 0.0)]], ..Default::default() };
        let m = merge(&[&a, &b]);
        assert_eq!(m.joints, vec!["A".to_string(), "B".to_string()]);
        assert_eq!(m.weights[1][0], (0, 1.0));
    }
}
