//! A course's lightmapped environment as world-space mesh objects, read from the player's own unpacked data.
use glam::{Mat3, Mat4, Quat, Vec3};
use ssbh_data::mesh_data::VectorData;
use ssbh_data::prelude::*;
use std::path::Path;

pub struct Obj {
    pub model: String,
    pub name: String,
    pub subindex: u64,
    pub material: String,
    pub pos: Vec<[f32; 3]>,
    pub nrm: Vec<[f32; 3]>,
    /// All UV sets of the object: (attribute name, values)
    pub uvs: Vec<(String, Vec<[f32; 2]>)>,
    pub idx: Vec<u32>,
    pub instance: usize,
}

fn v3(d: &VectorData) -> Vec<Vec3> {
    match d {
        VectorData::Vector2(v) => v.iter().map(|a| Vec3::new(a.x, a.y, 0.0)).collect(),
        VectorData::Vector3(v) => v.iter().map(|a| Vec3::new(a.x, a.y, a.z)).collect(),
        VectorData::Vector4(v) => v.iter().map(|a| Vec3::new(a.x, a.y, a.z)).collect(),
    }
}

fn v2(d: &VectorData) -> Vec<[f32; 2]> {
    match d {
        VectorData::Vector2(v) => v.iter().map(|a| [a.x, a.y]).collect(),
        VectorData::Vector3(v) => v.iter().map(|a| [a.x, a.y]).collect(),
        VectorData::Vector4(v) => v.iter().map(|a| [a.x, a.y]).collect(),
    }
}

/// Reads one model file and returns its mesh objects placed by `xf` (instance transform).
pub fn load_objects(modl_path: &Path, model: &str, xf: Mat4, instance: usize) -> Result<Vec<Obj>, String> {
    let dir = modl_path.parent().ok_or("no parent dir")?;
    let modl = ModlData::from_file(modl_path).map_err(|e| format!("modl {}: {e}", modl_path.display()))?;
    let mesh = MeshData::from_file(dir.join(&modl.mesh_file_name)).map_err(|e| format!("mesh {}: {e}", modl.mesh_file_name))?;
    let skel = SkelData::from_file(dir.join(&modl.skeleton_file_name)).ok();
    let bone_world = |name: &str| -> Mat4 {
        skel.as_ref()
            .and_then(|s| s.bones.iter().find(|b| b.name == name).map(|b| (s, b)))
            .and_then(|(s, b)| s.calculate_world_transform(b).ok())
            .unwrap_or(Mat4::IDENTITY)
    };
    let mut out = vec![];
    for o in &mesh.objects {
        let Some(pos) = o.positions.first() else { continue };
        let pos = v3(&pos.data);
        let bone = if o.bone_influences.is_empty() { bone_world(&o.parent_bone_name) } else { Mat4::IDENTITY };
        let m = xf * bone;
        let nm = Mat3::from_mat4(m).inverse().transpose();
        let nrm_in = o.normals.first().map(|n| v3(&n.data));
        let mut p = Vec::with_capacity(pos.len());
        let mut n = Vec::with_capacity(pos.len());
        for (i, q) in pos.iter().enumerate() {
            p.push(m.transform_point3(*q).to_array());
            let nn = nrm_in.as_ref().and_then(|a| a.get(i)).map(|a| (nm * *a).normalize_or_zero()).unwrap_or(Vec3::Y);
            n.push(nn.to_array());
        }
        let count = pos.len() as u32;
        let mut idx: Vec<u32> = o.vertex_indices.iter().copied().filter(|&i| i < count).collect();
        let extra = idx.len() % 3;
        idx.truncate(idx.len() - extra);
        let uvs = o
            .texture_coordinates
            .iter()
            .map(|a| (a.name.clone(), v2(&a.data)))
            .filter(|(_, v)| v.len() == pos.len())
            .collect();
        let material = modl
            .entries
            .iter()
            .find(|e| e.mesh_object_name == o.name && e.mesh_object_subindex == o.subindex)
            .map(|e| e.material_label.clone())
            .unwrap_or_default();
        out.push(Obj { model: model.to_string(), name: o.name.clone(), subindex: o.subindex, material, pos: p, nrm: n, uvs, idx, instance });
    }
    Ok(out)
}

pub fn instance_matrix(pos: [f32; 3], rot: [f32; 4], scale: [f32; 3]) -> Mat4 {
    Mat4::from_scale_rotation_translation(Vec3::from(scale), Quat::from_array(rot), Vec3::from(pos))
}
