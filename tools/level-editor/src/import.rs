//! Loads user scenery from glTF 2.0 (.gltf/.glb) and Wavefront OBJ into triangle parts, one part per source material.
//! Node transforms are applied (glTF), normals are computed when the file has none, and missing UVs become zeros.
//! The files are the user's own and stay local; nothing here writes them anywhere.

use glam::{Mat3, Mat4, Vec3};
use std::collections::BTreeMap;
use std::path::Path;

#[derive(Clone, Debug, Default)]
pub struct Part {
    /// the source material's name (used to pick a game material for it)
    pub material: String,
    pub positions: Vec<[f32; 3]>,
    pub normals: Vec<[f32; 3]>,
    pub uvs: Vec<[f32; 2]>,
    pub indices: Vec<u32>,
}

impl Part {
    fn append(&mut self, p: &[[f32; 3]], n: Option<Vec<[f32; 3]>>, uv: Option<Vec<[f32; 2]>>, idx: &[u32]) {
        let base = self.positions.len() as u32;
        let count = p.len();
        self.positions.extend_from_slice(p);
        let n = n.filter(|n| n.len() == count).unwrap_or_else(|| smooth_normals(p, idx));
        self.normals.extend(n);
        self.uvs.extend(uv.filter(|u| u.len() == count).unwrap_or_else(|| vec![[0.0, 0.0]; count]));
        self.indices.extend(idx.iter().map(|i| i + base));
    }
    pub fn bounds(&self) -> (Vec3, Vec3) {
        let mut lo = Vec3::splat(f32::MAX);
        let mut hi = Vec3::splat(f32::MIN);
        for p in &self.positions {
            lo = lo.min(Vec3::from(*p));
            hi = hi.max(Vec3::from(*p));
        }
        (lo, hi)
    }
}

fn smooth_normals(p: &[[f32; 3]], idx: &[u32]) -> Vec<[f32; 3]> {
    let mut n = vec![Vec3::ZERO; p.len()];
    for t in idx.chunks_exact(3) {
        let (a, b, c) = (Vec3::from(p[t[0] as usize]), Vec3::from(p[t[1] as usize]), Vec3::from(p[t[2] as usize]));
        let f = (b - a).cross(c - a);
        for &i in t {
            n[i as usize] += f;
        }
    }
    n.into_iter().map(|v| v.try_normalize().unwrap_or(Vec3::Y).to_array()).collect()
}

/// Loads a model file. Parts with no triangles are dropped; an empty result is an error.
pub fn load(path: &Path) -> Result<Vec<Part>, String> {
    let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase();
    let parts = match ext.as_str() {
        "gltf" | "glb" => load_gltf(path)?,
        "obj" => load_obj(path)?,
        "fbx" => return Err("FBX is not supported yet: export the model as glTF (.glb) or OBJ".into()),
        _ => return Err(format!("{}: unsupported model type (use .glb, .gltf or .obj)", path.display())),
    };
    let parts: Vec<Part> = parts.into_values().filter(|p| !p.indices.is_empty()).collect();
    if parts.is_empty() {
        return Err(format!("{}: no triangles", path.display()));
    }
    for p in &parts {
        if p.positions.len() > 65535 * 16 {
            return Err(format!("{}: part {} has {} vertices; split it", path.display(), p.material, p.positions.len()));
        }
        if p.positions.iter().flatten().any(|v| !v.is_finite()) {
            return Err(format!("{}: part {} has invalid positions", path.display(), p.material));
        }
    }
    Ok(parts)
}

fn load_obj(path: &Path) -> Result<BTreeMap<String, Part>, String> {
    let opts = tobj::LoadOptions { single_index: true, triangulate: true, ignore_points: true, ignore_lines: true };
    let (models, mats) = tobj::load_obj(path, &opts).map_err(|e| format!("{}: {e}", path.display()))?;
    let mats = mats.unwrap_or_default();
    let mut out: BTreeMap<String, Part> = BTreeMap::new();
    for m in models {
        let mesh = &m.mesh;
        let name = mesh.material_id.and_then(|i| mats.get(i)).map(|x| x.name.clone()).unwrap_or_else(|| "default".into());
        let p: Vec<[f32; 3]> = mesh.positions.chunks_exact(3).map(|c| [c[0], c[1], c[2]]).collect();
        let n = (!mesh.normals.is_empty()).then(|| mesh.normals.chunks_exact(3).map(|c| [c[0], c[1], c[2]]).collect());
        // OBJ v runs bottom to top; the game's textures are top-down like glTF
        let uv = (!mesh.texcoords.is_empty()).then(|| mesh.texcoords.chunks_exact(2).map(|c| [c[0], 1.0 - c[1]]).collect());
        let part = out.entry(name.clone()).or_insert_with(|| Part { material: name, ..Default::default() });
        part.append(&p, n, uv, &mesh.indices);
    }
    Ok(out)
}

fn base64(s: &str) -> Result<Vec<u8>, String> {
    let val = |c: u8| -> Option<u32> {
        Some(match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' | b'-' => 62,
            b'/' | b'_' => 63,
            _ => return None,
        } as u32)
    };
    let mut out = vec![];
    let (mut acc, mut bits) = (0u32, 0);
    for &c in s.as_bytes() {
        if c == b'=' || c.is_ascii_whitespace() {
            continue;
        }
        acc = (acc << 6) | val(c).ok_or("bad base64 in a data URI")?;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
        }
    }
    Ok(out)
}

fn load_gltf(path: &Path) -> Result<BTreeMap<String, Part>, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let g = gltf::Gltf::from_slice(&bytes).map_err(|e| format!("{}: {e}", path.display()))?;
    let dir = path.parent().unwrap_or(Path::new("."));
    let mut buffers: Vec<Vec<u8>> = vec![];
    for b in g.buffers() {
        let data = match b.source() {
            gltf::buffer::Source::Bin => g.blob.clone().ok_or("glb has no binary chunk")?,
            gltf::buffer::Source::Uri(u) if u.starts_with("data:") => base64(u.split_once(',').ok_or("bad data URI")?.1)?,
            gltf::buffer::Source::Uri(u) => {
                let rel = u.replace("%20", " ");
                if rel.contains("..") || Path::new(&rel).is_absolute() {
                    return Err(format!("buffer {u}: only files next to the model are read"));
                }
                std::fs::read(dir.join(&rel)).map_err(|e| format!("buffer {u}: {e}"))?
            }
        };
        if data.len() < b.length() {
            return Err(format!("buffer {} is shorter than declared", b.index()));
        }
        buffers.push(data);
    }
    let scene = g.default_scene().or_else(|| g.scenes().next()).ok_or("no scene")?;
    let mut out: BTreeMap<String, Part> = BTreeMap::new();
    let mut stack: Vec<(gltf::Node, Mat4)> = scene.nodes().map(|n| (n, Mat4::IDENTITY)).collect();
    while let Some((node, parent)) = stack.pop() {
        let m = parent * Mat4::from_cols_array_2d(&node.transform().matrix());
        let nm = Mat3::from_mat4(m).inverse().transpose();
        if let Some(mesh) = node.mesh() {
            for prim in mesh.primitives() {
                if prim.mode() != gltf::mesh::Mode::Triangles {
                    continue;
                }
                let r = prim.reader(|b| buffers.get(b.index()).map(|v| v.as_slice()));
                let Some(pos) = r.read_positions() else { continue };
                let p: Vec<[f32; 3]> = pos.map(|v| m.transform_point3(Vec3::from(v)).to_array()).collect();
                let n = r.read_normals().map(|it| it.map(|v| (nm * Vec3::from(v)).normalize_or_zero().to_array()).collect());
                let uv = r.read_tex_coords(0).map(|t| t.into_f32().collect());
                let idx: Vec<u32> = match r.read_indices() {
                    Some(i) => i.into_u32().collect(),
                    None => (0..p.len() as u32).collect(),
                };
                if idx.iter().any(|&i| i as usize >= p.len()) {
                    return Err(format!("mesh {:?}: index out of range", mesh.name()));
                }
                // mirrored transforms flip the winding
                let idx = if m.determinant() < 0.0 { idx.chunks_exact(3).flat_map(|t| [t[0], t[2], t[1]]).collect() } else { idx };
                let mat = prim.material();
                let name = mat.name().map(str::to_string).unwrap_or_else(|| match mat.index() {
                    Some(i) => format!("material{i}"),
                    None => "default".into(),
                });
                let part = out.entry(name.clone()).or_insert_with(|| Part { material: name, ..Default::default() });
                part.append(&p, n, uv, &idx);
            }
        }
        for c in node.children() {
            stack.push((c, m));
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn obj_cube_loads() {
        let dir = std::env::temp_dir().join(format!("le_import_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let f = dir.join("q.obj");
        std::fs::write(&f, "v 0 0 0\nv 1 0 0\nv 1 0 1\nv 0 0 1\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nf 1/1 3/3 2/2\nf 1/1 4/4 3/3\n").unwrap();
        let parts = load(&f).unwrap();
        assert_eq!(parts.len(), 1);
        assert_eq!(parts[0].indices.len(), 6);
        assert_eq!(parts[0].positions.len(), parts[0].normals.len());
        assert!(parts[0].normals.iter().all(|n| (n[1].abs() - 1.0).abs() < 1e-5));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn base64_decodes() {
        assert_eq!(base64("aGVsbG8=").unwrap(), b"hello");
    }
}
