//! Byte layout survey of a levelb file (`--layout`): where the root table, its vtable, its children and the entity tables
//! sit, so the writer knows which offsets cross an insertion point.

use crate::fb::{Buf, Table};
use std::collections::{BTreeMap, HashSet};

/// Field positions and inline sizes of a table, sorted by position. The size of the last field runs to the table end
/// (`vtable[1]` = inline size).
pub fn fields(t: &Table) -> Vec<(usize, usize, usize)> {
    let mut f: Vec<(usize, usize)> = (0..t.slot_count()).filter_map(|s| t.field(s).map(|p| (s, p))).collect();
    f.sort_by_key(|x| x.1);
    let end = t.pos + t.inline_size();
    (0..f.len())
        .map(|i| {
            let next = f.get(i + 1).map(|x| x.1).unwrap_or(end);
            (f[i].0, f[i].1, next - f[i].1)
        })
        .collect()
}

pub fn dump(bytes: &[u8], name: &str) {
    let buf = Buf(bytes);
    let Some(root) = buf.root() else { return };
    println!("== {name}: {} bytes, root table at {:#x}, vtable at {:#x} (size {}), inline size {}", bytes.len(), root.pos, root.vtable_pos(), root.vtable_size(), root.inline_size());
    println!("   file identifier bytes 4..8: {:02x?}", &bytes[4..8]);
    for (s, p, sz) in fields(&root) {
        let v = buf.u32_at(p).unwrap_or(0);
        let tgt = if sz == 4 && (p + v as usize) < bytes.len() { format!(" -> {:#x}", p + v as usize) } else { String::new() };
        println!("   slot {s:>2} at {p:#x} size {sz} value {v:#x}{tgt}");
    }
    let ents = root.tables(2);
    let nodes = root.tables(3);
    let (emin, emax) = ents.iter().fold((usize::MAX, 0), |a, t| (a.0.min(t.pos), a.1.max(t.pos)));
    let vtmin = ents.iter().map(|t| t.vtable_pos()).min().unwrap_or(0);
    println!("   entities {} at {:#x}: tables {:#x}..{:#x}, lowest vtable {:#x}", ents.len(), root.vec(2).map(|v| v.0).unwrap_or(0), emin, emax, vtmin);
    let node_ents: HashSet<u32> = nodes.iter().filter_map(|n| n.u32(2)).collect();
    let with_node = ents.iter().filter(|e| e.u32(1).is_some_and(|h| node_ents.contains(&h))).count();
    println!("   nodes {}: entities with a node {with_node} of {}", nodes.len(), ents.len());
    // Lowest address of anything reachable one level down from the root's offset fields.
    let low = fields(&root).iter().filter(|f| f.2 == 4 && f.0 != 0).map(|f| f.1 + buf.u32_at(f.1).unwrap_or(0) as usize).min();
    println!("   lowest root child at {:#x}; root table ends at {:#x}", low.unwrap_or(0), root.pos + root.inline_size());
    // Component layouts per kind: slots present in the sub-table and their sizes.
    let mut kinds: BTreeMap<usize, BTreeMap<String, usize>> = BTreeMap::new();
    let mut comp_fields: BTreeMap<String, usize> = BTreeMap::new();
    let mut ent_fields: BTreeMap<String, usize> = BTreeMap::new();
    for e in &ents {
        let ef: Vec<String> = fields(e).iter().map(|f| format!("s{}:{}", f.0, f.2)).collect();
        *ent_fields.entry(ef.join(" ")).or_default() += 1;
        for c in e.tables(0) {
            let cf: Vec<String> = fields(&c).iter().map(|f| format!("s{}:{}", f.0, f.2)).collect();
            *comp_fields.entry(cf.join(" ")).or_default() += 1;
            let Some((slot, t)) = (1..c.slot_count()).find_map(|s| c.table(s).map(|t| (s, t))) else { continue };
            let sf: Vec<String> = fields(&t).iter().map(|f| format!("s{}:{}", f.0, f.2)).collect();
            *kinds.entry(slot).or_default().entry(sf.join(" ")).or_default() += 1;
        }
    }
    println!("   entity layouts: {ent_fields:?}");
    println!("   component wrapper layouts: {comp_fields:?}");
    for (k, v) in kinds {
        println!("   kind {k}: {v:?}");
    }
    // Ids: entity hash, its node, and the uint inside each component sub-table's s0 child table.
    let ent_hashes: HashSet<u32> = ents.iter().filter_map(|e| e.u32(1)).collect();
    let mut comp_ids: Vec<u32> = vec![];
    for (i, e) in ents.iter().enumerate() {
        let h = e.u32(1).unwrap_or(0);
        let node = nodes.iter().find(|n| n.u32(2) == Some(h));
        let mut cs = vec![];
        for c in e.tables(0) {
            let Some((slot, t)) = (1..c.slot_count()).find_map(|s| c.table(s).map(|t| (s, t))) else { continue };
            let id = t.table(0).and_then(|x| x.u32(0));
            if let Some(id) = id {
                comp_ids.push(id);
            }
            cs.push(format!("k{slot}:tag{}:id{:08x}", c.u8(0).unwrap_or(255), id.unwrap_or(0)));
        }
        if i < 4 {
            let nd = node.map(|n| format!("node id {:08x} s4 {:08x} s3 {:?} comps {}", n.u32(1).unwrap_or(0), n.u32(4).unwrap_or(0), n.u8(3), n.tables(0).len()));
            println!("   entity {i} hash {h:08x} [{}] {}", cs.join(" "), nd.unwrap_or_default());
        }
    }
    let uniq: HashSet<u32> = comp_ids.iter().copied().collect();
    println!("   component ids: {} of which unique {}; equal to an entity hash: {}", comp_ids.len(), uniq.len(), comp_ids.iter().filter(|c| ent_hashes.contains(c)).count());
    let node_ids: HashSet<u32> = nodes.iter().filter_map(|n| n.u32(1)).collect();
    let parents: HashSet<u32> = nodes.iter().filter_map(|n| n.u32(4)).collect();
    println!("   node s4 values: {} distinct; s4 that are node ids: {}", parents.len(), parents.iter().filter(|p| node_ids.contains(p)).count());
    for slot in [4usize, 5, 10, 12, 14] {
        let v = root.tables(slot);
        let s0: Vec<u32> = v.iter().filter_map(|t| t.u32(0)).collect();
        println!(
            "   root s{slot}: {} entries; s0 matching entity hash {}, node id {}, component id {}",
            v.len(),
            s0.iter().filter(|x| ent_hashes.contains(x)).count(),
            s0.iter().filter(|x| node_ids.contains(x)).count(),
            s0.iter().filter(|x| uniq.contains(x)).count()
        );
    }
    if let Some(n) = nodes.first() {
        let nf: Vec<String> = fields(n).iter().map(|f| format!("s{}:{}", f.0, f.2)).collect();
        println!("   first node layout: {}", nf.join(" "));
    }
}
