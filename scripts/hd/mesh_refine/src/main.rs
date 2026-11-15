// SPDX-License-Identifier: MIT
// mesh_refine: inspect and refine SSBH meshes (numshb) for Snap Recomp model mod packs.
// usage:
//   mesh_refine info <file.numshb>             print layout (version, objects, attributes, strides, rigging)
//   mesh_refine roundtrip <in.numshb> <out>    read + write with ssbh_lib, report byte equality
use ssbh_lib::formats::mesh::*;
use std::path::Path;
mod refine;

fn inner(p: &Path) -> MeshInner<AttributeV10, ssbh_lib::SsbhByteBuffer> {
    match Mesh::from_file(p).expect("read mesh") {
        Mesh::V10(m) => m,
        _ => panic!("not a 1.10 mesh"),
    }
}

// Split a refined object's index range into (original, refined); None when the object carries no marker.
fn split(ix: &[u32]) -> Option<(Vec<u32>, Vec<u32>)> {
    if ix.len() < 27 || ix[..9] != refine::HDR || ix[ix.len() - 9..] != refine::TRL {
        return None;
    }
    let mut k = 9;
    while k + 9 <= ix.len() - 9 {
        if ix[k..k + 9] == refine::SEP {
            return Some((ix[9..k].to_vec(), ix[k + 9..ix.len() - 9].to_vec()));
        }
        k += 3;
    }
    None
}

// Proves the invariants the runtime and photo scoring rely on: per object, the original index range equals the original
// indices, the original vertices are byte-identical, and the original vertices keep exactly their skin weights.
fn verify(orig: &Path, refd: &Path) {
    let a = inner(orig);
    let b = inner(refd);
    assert_eq!(a.objects.elements.len(), b.objects.elements.len(), "object count");
    let mut fails = 0;
    for (oa, ob) in a.objects.elements.iter().zip(b.objects.elements.iter()) {
        let label = format!("{}#{}", oa.name.to_str().unwrap_or(""), oa.subindex);
        let ia = refine::read_indices(&a, oa);
        let ib = refine::read_indices(&b, ob);
        let nv = oa.vertex_count as usize;
        let mut msg = vec![];
        let refined = match split(&ib) {
            Some((o, r)) => {
                if o != ia {
                    msg.push("original range differs".to_string());
                }
                if r.iter().any(|&x| x as usize >= ob.vertex_count as usize) {
                    msg.push("refined index out of range".into());
                }
                Some(r.len() / 3)
            }
            None => {
                if ib != ia {
                    msg.push("unmarked object with changed indices".into());
                }
                None
            }
        };
        for buf in 0..2 {
            let (st, oa_off, ob_off) = match buf {
                0 => (oa.stride0 as usize, oa.vertex_buffer0_offset as usize, ob.vertex_buffer0_offset as usize),
                _ => (oa.stride1 as usize, oa.vertex_buffer1_offset as usize, ob.vertex_buffer1_offset as usize),
            };
            if st == 0 {
                continue;
            }
            if a.vertex_buffers.elements[buf].elements[oa_off..oa_off + st * nv] != b.vertex_buffers.elements[buf].elements[ob_off..ob_off + st * nv] {
                msg.push(format!("buffer {buf} original vertices differ"));
            }
        }
        let ra = a.rigging_buffers.elements.iter().find(|r| r.mesh_object_name.to_str() == oa.name.to_str() && r.mesh_object_subindex == oa.subindex);
        let rb = b.rigging_buffers.elements.iter().find(|r| r.mesh_object_name.to_str() == oa.name.to_str() && r.mesh_object_subindex == oa.subindex);
        if let (Some(ra), Some(rb)) = (ra, rb) {
            let wa = refine::read_weights(ra, nv);
            let wb = refine::read_weights(rb, nv);
            if wa != wb {
                msg.push("original skin weights differ".into());
            }
            let wall = refine::read_weights(rb, ob.vertex_count as usize);
            // A static object (no original vertex weighted) keeps unweighted new vertices; a skinned one must weight them all.
            let skinned = wa.iter().any(|w| !w.is_empty());
            let unweighted = if skinned { wall[nv..].iter().filter(|w| w.is_empty()).count() } else { 0 };
            let over = wall.iter().filter(|w| w.len() > rb.flags.max_influences as usize).count();
            if unweighted > 0 || over > 0 {
                msg.push(format!("new vertices: {unweighted} without weights, {over} over max influences"));
            }
        }
        if oa.bounding_info != ob.bounding_info {
            msg.push("bounding info changed".into());
        }
        fails += msg.len();
        println!("{label}: v {} -> {} tris {} -> {} {}", nv, ob.vertex_count, ia.len() / 3, refined.map(|r| r.to_string()).unwrap_or("(unrefined)".into()), if msg.is_empty() { "OK".into() } else { msg.join("; ") });
    }
    println!("{}", if fails == 0 { "VERIFY OK" } else { "VERIFY FAILED" });
    if fails > 0 {
        std::process::exit(1);
    }
}

// Writes a plain mesh with only one range of each refined object (for offline viewers): refined or original.
fn strip(refd: &Path, out: &Path, original: bool) {
    let mut m = inner(refd);
    let mut ibuf = vec![];
    let objs = m.objects.elements.clone();
    for (i, o) in objs.iter().enumerate() {
        let ix = refine::read_indices(&m, o);
        let keep = match split(&ix) {
            Some((a, b)) => if original { a } else { b },
            None => ix,
        };
        let om = &mut m.objects.elements[i];
        om.index_buffer_offset = ibuf.len() as u32;
        om.vertex_index_count = keep.len() as u32;
        let u16i = om.draw_element_type == DrawElementType::UnsignedShort;
        for x in keep {
            if u16i { ibuf.extend_from_slice(&(x as u16).to_le_bytes()) } else { ibuf.extend_from_slice(&x.to_le_bytes()) }
        }
    }
    m.polygon_index_size = ibuf.len() as u64;
    m.index_buffer.elements = ibuf;
    Mesh::V10(m).write_to_file(out).expect("write");
}

fn info(p: &Path) {
    let m = Mesh::from_file(p).expect("read mesh");
    let (maj, min) = ssbh_lib::Version::major_minor_version(&m);
    println!("version {maj}.{min}");
    match &m {
        Mesh::V10(inner) => {
            println!("model_name {:?}", inner.model_name.to_str());
            println!("buffer_sizes {:?} polygon_index_size {}", inner.buffer_sizes.elements, inner.polygon_index_size);
            println!("vertex_buffers {:?} index_buffer {}", inner.vertex_buffers.elements.iter().map(|b| b.elements.len()).collect::<Vec<_>>(), inner.index_buffer.elements.len());
            for o in &inner.objects.elements {
                println!(
                    "obj {:?}#{} parent={:?} v={} i={} unk2={} vb_off=[{},{},{},{}] stride=[{},{},{},{}] ib_off={} unk8={} idx={:?} skin={} sort={} depth=({},{})",
                    o.name.to_str(), o.subindex, o.parent_bone_name.to_str(), o.vertex_count, o.vertex_index_count, o.unk2,
                    o.vertex_buffer0_offset, o.vertex_buffer1_offset, o.vertex_buffer2_offset, o.vertex_buffer3_offset,
                    o.stride0, o.stride1, o.stride2, o.stride3, o.index_buffer_offset, o.unk8, o.draw_element_type,
                    o.use_vertex_skinning, o.sort_bias, o.depth_flags.disable_depth_write, o.depth_flags.disable_depth_test
                );
                let bs = &o.bounding_info.bounding_sphere;
                println!("   sphere c=({:.3},{:.3},{:.3}) r={:.3}", bs.center.x, bs.center.y, bs.center.z, bs.radius);
                for a in &o.attributes.elements {
                    println!(
                        "   attr {:?} {:?} {:?} buf={} off={} sub={} names={:?}",
                        a.name.to_str(), a.usage, a.data_type, a.buffer_index, a.buffer_offset, a.subindex,
                        a.attribute_names.elements.iter().map(|s| s.to_str().unwrap_or("").to_string()).collect::<Vec<_>>()
                    );
                }
            }
            for r in &inner.rigging_buffers.elements {
                println!(
                    "rig {:?}#{} max_infl={} bones={} bytes={}",
                    r.mesh_object_name.to_str(), r.mesh_object_subindex, r.flags.max_influences, r.buffers.elements.len(),
                    r.buffers.elements.iter().map(|b| b.data.elements.len()).sum::<usize>()
                );
            }
        }
        _ => println!("(non-1.10 mesh; detail printing not implemented)"),
    }
}

// Topology and faceting stats per object: unique/degenerate triangles, welded edges, and the dihedral angle
// across smooth (non-crease) edges, which is what shows as a faceted silhouette.
fn stats(p: &Path) {
    use ssbh_data::mesh_data::MeshData;
    let md = MeshData::from_file(p).expect("mesh");
    for o in &md.objects {
        let pos = vec3s(&o.positions[0].data);
        let tris: Vec<[u32; 3]> = o.vertex_indices.chunks(3).map(|c| [c[0], c[1], c[2]]).collect();
        let mut uniq: std::collections::HashMap<[u32; 3], [u32; 3]> = Default::default();
        let mut degen = 0;
        let mut dup = 0;
        let (mut d_same, mut d_rot, mut d_flip) = (0, 0, 0);
        for t in &tris {
            if t[0] == t[1] || t[1] == t[2] || t[0] == t[2] {
                degen += 1;
                continue;
            }
            let mut s = *t;
            s.sort();
            if let Some(f) = uniq.get(&s) {
                dup += 1;
                let rots = [[f[0], f[1], f[2]], [f[1], f[2], f[0]], [f[2], f[0], f[1]]];
                if *t == *f {
                    d_same += 1;
                } else if rots.contains(t) {
                    d_rot += 1;
                } else {
                    d_flip += 1;
                }
            } else {
                uniq.insert(s, *t);
            }
        }
        if dup > 0 {
            println!("  duplicates: {d_same} same order, {d_rot} rotated, {d_flip} flipped; first indices {:?}", &o.vertex_indices[..o.vertex_indices.len().min(18)]);
        }
        let weld = weld_ids(&pos);
        let mut edges: std::collections::HashMap<(u32, u32), Vec<usize>> = Default::default();
        let fnrm: Vec<glam::Vec3> = tris.iter().map(|t| (pos[t[1] as usize] - pos[t[0] as usize]).cross(pos[t[2] as usize] - pos[t[0] as usize]).normalize_or_zero()).collect();
        for (fi, t) in tris.iter().enumerate() {
            for k in 0..3 {
                let (a, b) = (weld[t[k] as usize], weld[t[(k + 1) % 3] as usize]);
                if a == b {
                    continue;
                }
                edges.entry((a.min(b), a.max(b))).or_default().push(fi);
            }
        }
        let (mut bnd, mut nonman, mut angs) = (0, 0, vec![]);
        for f in edges.values() {
            match f.len() {
                1 => bnd += 1,
                2 => {
                    let d = fnrm[f[0]].dot(fnrm[f[1]]).clamp(-1.0, 1.0).acos().to_degrees();
                    angs.push(d);
                }
                _ => nonman += 1,
            }
        }
        angs.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let pct = |q: f32| if angs.is_empty() { 0.0 } else { angs[((angs.len() - 1) as f32 * q) as usize] };
        let ext = bbox(&pos);
        println!(
            "{}#{} v={} weld={} tris={} uniq={} degen={} dup={} edges={} boundary={} nonmanifold={} dihedral med={:.1} p90={:.1} p99={:.1} size={:.3}",
            o.name, o.subindex, pos.len(), weld.iter().max().map(|m| m + 1).unwrap_or(0), tris.len(), uniq.len(), degen, dup, edges.len(), bnd, nonman,
            pct(0.5), pct(0.9), pct(0.99), ext
        );
    }
}

fn vec3s(d: &ssbh_data::mesh_data::VectorData) -> Vec<glam::Vec3> {
    use ssbh_data::mesh_data::VectorData;
    match d {
        VectorData::Vector2(v) => v.iter().map(|a| glam::vec3(a.x, a.y, 0.0)).collect(),
        VectorData::Vector3(v) => v.clone(),
        VectorData::Vector4(v) => v.iter().map(|a| a.truncate()).collect(),
    }
}
fn bbox(p: &[glam::Vec3]) -> f32 {
    let mut mn = glam::Vec3::splat(f32::MAX);
    let mut mx = glam::Vec3::splat(f32::MIN);
    for q in p {
        mn = mn.min(*q);
        mx = mx.max(*q);
    }
    (mx - mn).length()
}
// Position weld: vertices at the same position (UV seams, hard-edge splits) share an id.
fn weld_ids(pos: &[glam::Vec3]) -> Vec<u32> {
    let mut map: std::collections::HashMap<[u32; 3], u32> = Default::default();
    pos.iter()
        .map(|p| {
            let k = [p.x.to_bits(), p.y.to_bits(), p.z.to_bits()];
            let n = map.len() as u32;
            *map.entry(k).or_insert(n)
        })
        .collect()
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    match a.get(1).map(|s| s.as_str()) {
        Some("info") => info(Path::new(&a[2])),
        Some("stats") => {
            for f in &a[2..] {
                println!("== {f}");
                stats(Path::new(f));
            }
        }
        Some("check") => {
            let (mut ok, mut bad, mut other) = (0, 0, 0);
            for f in &a[2..] {
                match Mesh::from_file(f) {
                    Ok(Mesh::V10(m)) => match refine::check_layout(&m) {
                        Ok(()) => ok += 1,
                        Err(e) => {
                            bad += 1;
                            println!("{f}: {e}");
                        }
                    },
                    Ok(_) => other += 1,
                    Err(e) => println!("{f}: read error {e}"),
                }
            }
            println!("layout ok {ok} bad {bad} other-version {other}");
        }
        Some("refine") => {
            // refine <in.numshb> <out.numshb> [--levels N] [--clamp F] [--only name#sub]... [--min-tris N]
            let mut opt = refine::Options { levels: 1, clamp: 0.25, only: vec![], min_tris: 0 };
            let mut i = 4;
            while i < a.len() {
                match a[i].as_str() {
                    "--levels" => opt.levels = a[i + 1].parse().unwrap(),
                    "--clamp" => opt.clamp = a[i + 1].parse().unwrap(),
                    "--only" => opt.only.push(a[i + 1].clone()),
                    "--min-tris" => opt.min_tris = a[i + 1].parse().unwrap(),
                    x => panic!("unknown option {x}"),
                }
                i += 2;
            }
            let mut m = Mesh::from_file(&a[2]).expect("read");
            match refine::refine_mesh(&mut m, &opt) {
                Ok(lines) => {
                    for l in lines {
                        println!("{l}");
                    }
                    m.write_to_file(&a[3]).expect("write");
                }
                Err(e) => {
                    eprintln!("refine failed: {e}");
                    std::process::exit(1);
                }
            }
        }
        Some("verify") => verify(Path::new(&a[2]), Path::new(&a[3])),
        Some("strip") => strip(Path::new(&a[2]), Path::new(&a[3]), a.get(4).map(|s| s.as_str()) == Some("original")),
        Some("roundtrip") => {
            let m = Mesh::from_file(&a[2]).expect("read");
            m.write_to_file(&a[3]).expect("write");
            let x = std::fs::read(&a[2]).unwrap();
            let y = std::fs::read(&a[3]).unwrap();
            let first = x.iter().zip(y.iter()).position(|(p, q)| p != q);
            println!("in {} out {} identical {} first_diff {:?}", x.len(), y.len(), x == y, first);
        }
        _ => {
            eprintln!("usage: mesh_refine info|roundtrip ...");
            std::process::exit(2);
        }
    }
}
