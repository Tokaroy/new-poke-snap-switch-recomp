//! Adds entities to a levelb by cloning existing ones.
//!
//! FlatBuffers offsets only point forward, so a table cannot simply be appended and referenced from older data. In every
//! levelb checked (`--layout`) the file starts with the root vtable, then the root table, and every other object comes after
//! the root table. The writer inserts one block right after the root table and shifts the rest of the file by the block size
//! (a multiple of 16, so all alignment holds). Only the root table's own offsets cross the insertion point; they are
//! corrected. The block holds new entity and scene node vectors (old entries re-pointed past the block, new entries
//! appended) and the cloned tables. Field types come from the inferred schema `analysis/registries/schemas/levelb.fbs`:
//! a clone keeps every scalar and struct byte for byte and re-points every offset at the original child, except the
//! children it replaces (component tables and their id tables, which get fresh ids).

use crate::fb::{Buf, Table};
use std::collections::{HashMap, HashSet};

const SCHEMA: &str = include_str!("../../../analysis/registries/schemas/levelb.fbs");

pub struct Schema {
    /// table name -> slot -> field type
    tables: HashMap<String, HashMap<usize, String>>,
}

const SCALARS: &[&str] = &["bool", "byte", "ubyte", "short", "ushort", "int", "uint", "long", "ulong", "float", "double"];
const STRUCTS: &[&str] = &["Raw12", "Raw16", "Vec2", "Vec3", "Vec4"];

impl Schema {
    pub fn levelb() -> Self {
        let mut tables: HashMap<String, HashMap<usize, String>> = HashMap::new();
        let mut cur: Option<String> = None;
        for line in SCHEMA.lines() {
            let l = line.trim();
            if let Some(rest) = l.strip_prefix("table ") {
                cur = rest.split_whitespace().next().map(str::to_string);
                tables.entry(cur.clone().unwrap()).or_default();
            } else if l.starts_with('}') {
                cur = None;
            } else if let (Some(t), Some((name_ty, rest))) = (&cur, l.split_once(" (id: ")) {
                let Some((_, ty)) = name_ty.split_once(':') else { continue };
                let Ok(id) = rest.split(')').next().unwrap_or("").trim().parse::<usize>() else { continue };
                tables.get_mut(t).unwrap().insert(id, ty.trim().to_string());
            }
        }
        Self { tables }
    }
    pub fn field_type(&self, table: &str, slot: usize) -> Option<&str> {
        self.tables.get(table)?.get(&slot).map(String::as_str)
    }
    /// True when the field is a 4-byte offset (string, vector or table).
    pub fn is_offset(&self, table: &str, slot: usize) -> Result<bool, String> {
        let ty = self.field_type(table, slot).ok_or_else(|| format!("schema has no {table}.s{slot}"))?;
        Ok(ty.starts_with('[') || ty == "string" || !(SCALARS.contains(&ty) || STRUCTS.contains(&ty)))
    }
}

/// One clone request: copy entity `entity` (index in the root entity list) and set floats in its components.
#[derive(Clone, Debug)]
pub struct Add {
    pub entity: usize,
    /// (component kind slot, field slot, values) e.g. (5, 3, pos) for a model instance position.
    pub set: Vec<(usize, usize, Vec<f32>)>,
    /// (component kind slot, string field slot, text): gives the clone its own string, e.g. (5, 1, mesh name) for a model
    /// instance that shows an imported model group instead of the source's.
    pub names: Vec<(usize, usize, String)>,
    /// (component kind slot, vector-of-tables slot, names): replaces the clone's list with one copy of the source list's
    /// first element per name, each with its string field 0 set to the name. A model instance keeps one entry per model
    /// entry in its s17 (lightmap) and s23 lists; a new model needs lists that match its own shapes.
    pub lists: Vec<(usize, usize, Vec<String>)>,
}

/// Appends a FlatBuffers string (u32 length, bytes, NUL) and returns its position.
fn put_string(blk: &mut Block, text: &str) -> usize {
    blk.align(4);
    let sp = blk.reserve(4 + text.len() + 1);
    blk.put_u32(sp, text.len() as u32);
    let i = sp + 4 - blk.base;
    blk.bytes[i..i + text.len()].copy_from_slice(text.as_bytes());
    sp
}

/// Fresh ids of one added entity, so the caller can find it again after export.
#[derive(Clone, Debug)]
pub struct Added {
    pub hash: u32,
    pub node_id: Option<u32>,
}

struct Block {
    base: usize,
    bytes: Vec<u8>,
}

impl Block {
    fn here(&self) -> usize {
        self.base + self.bytes.len()
    }
    fn align(&mut self, a: usize) {
        while self.here() % a != 0 {
            self.bytes.push(0);
        }
    }
    fn reserve(&mut self, n: usize) -> usize {
        let p = self.here();
        self.bytes.resize(self.bytes.len() + n, 0);
        p
    }
    fn put_u32(&mut self, at: usize, v: u32) {
        let i = at - self.base;
        self.bytes[i..i + 4].copy_from_slice(&v.to_le_bytes());
    }
    fn put_i32(&mut self, at: usize, v: i32) {
        self.put_u32(at, v as u32);
    }
}

/// Copies a table (its vtable and inline bytes) into the block. Offset fields keep pointing at the original children
/// (shifted by `shift`, the block size, since they sit after the insertion point); the caller re-points replaced ones.
/// Returns the new table position.
fn copy_table(blk: &mut Block, src: &Table, ty: &str, schema: &Schema, shift: usize) -> Result<usize, String> {
    let buf = src.buf;
    let vt = src.vtable_pos();
    let vsize = src.vtable_size();
    let tsize = src.inline_size();
    blk.align(2);
    let vpos = blk.here();
    blk.bytes.extend_from_slice(&buf.0[vt..vt + vsize]);
    // keep the table's alignment within 16 bytes, so inline structs stay aligned as in the original
    while blk.here() % 16 != src.pos % 16 {
        blk.bytes.push(0);
    }
    let pos = blk.here();
    blk.bytes.extend_from_slice(&buf.0[src.pos..src.pos + tsize]);
    blk.put_i32(pos, (pos - vpos) as i32);
    for slot in 0..src.slot_count() {
        let Some(f) = src.field(slot) else { continue };
        if schema.is_offset(ty, slot)? {
            let target = f + buf.u32_at(f).ok_or("bad offset")? as usize + shift;
            let nf = pos + (f - src.pos);
            blk.put_u32(nf, target.wrapping_sub(nf) as u32);
        }
    }
    Ok(pos)
}

fn set_offset(blk: &mut Block, table_pos: usize, src: &Table, slot: usize, target: usize) -> Result<(), String> {
    let f = src.field(slot).ok_or("slot missing")?;
    let nf = table_pos + (f - src.pos);
    if target <= nf {
        return Err("internal: backward offset".into());
    }
    blk.put_u32(nf, (target - nf) as u32);
    Ok(())
}

fn fresh(used: &mut HashSet<u32>, seed: &mut u32) -> u32 {
    loop {
        *seed ^= *seed << 13;
        *seed ^= *seed >> 17;
        *seed ^= *seed << 5;
        if *seed != 0 && used.insert(*seed) {
            return *seed;
        }
    }
}

/// Builds a new levelb with the requested clones. The input is never changed.
pub fn grow(bytes: &[u8], adds: &[Add]) -> Result<(Vec<u8>, Vec<Added>), String> {
    let schema = Schema::levelb();
    let buf = Buf(bytes);
    let root = buf.root().ok_or("not a FlatBuffers file")?;
    let root_ty = "LevelData";
    let p_ins = root.pos + root.inline_size();
    if root.vtable_pos() >= root.pos || p_ins % 4 != 0 {
        return Err("unexpected root layout".into());
    }
    // every root child must lie after the insertion point
    for slot in 0..root.slot_count() {
        if let Some(f) = root.field(slot) {
            if schema.is_offset(root_ty, slot)? && f + (buf.u32_at(f).unwrap_or(0) as usize) < p_ins {
                return Err(format!("root slot {slot} points before the insertion point"));
            }
        }
    }
    let ents = root.tables(2);
    let nodes = root.tables(3);
    let ent_ty = schema.field_type(root_ty, 2).ok_or("schema")?.trim_matches(|c| c == '[' || c == ']').to_string();
    let node_ty = schema.field_type(root_ty, 3).ok_or("schema")?.trim_matches(|c| c == '[' || c == ']').to_string();
    let comp_ty = schema.field_type(&ent_ty, 0).ok_or("schema")?.trim_matches(|c| c == '[' || c == ']').to_string();

    // ids in use
    let mut used: HashSet<u32> = HashSet::new();
    for e in &ents {
        used.extend(e.u32(1));
        for c in e.tables(0) {
            for s in 1..c.slot_count() {
                if let Some(id) = c.table(s).and_then(|t| t.table(0)).and_then(|x| x.u32(0)) {
                    used.insert(id);
                }
            }
        }
    }
    for n in &nodes {
        used.extend(n.u32(1));
    }
    let seed = crc32fast::hash(&bytes[..bytes.len().min(4096)]) | 1;

    // Two passes: the block size must be known to compute shifted offsets, and the layout does not depend on the
    // shift value, so pass 1 measures with shift 0 and pass 2 writes with the real shift.
    let mut size = 0usize;
    let mut out_ids = vec![];
    for pass in 0..2 {
        let shift = size;
        let mut s2 = seed;
        let mut used2 = used.clone();
        let mut blk = Block { base: p_ins, bytes: vec![] };
        out_ids.clear();
        // vectors first (they reference everything else in the block)
        blk.align(4);
        let ev = blk.reserve(4 + 4 * (ents.len() + adds.len()));
        blk.put_u32(ev, (ents.len() + adds.len()) as u32);
        root.vec(2).ok_or("no entity vector")?;
        for i in 0..ents.len() {
            let target = ents[i].pos + shift;
            let at = ev + 4 + 4 * i;
            blk.put_u32(at, target.wrapping_sub(at) as u32);
        }
        let new_nodes: usize = adds.iter().filter(|a| ents.get(a.entity).and_then(|e| e.u32(1)).is_some_and(|h| nodes.iter().any(|n| n.u32(2) == Some(h)))).count();
        let nv = blk.reserve(4 + 4 * (nodes.len() + new_nodes));
        blk.put_u32(nv, (nodes.len() + new_nodes) as u32);
        for i in 0..nodes.len() {
            let target = nodes[i].pos + shift;
            let at = nv + 4 + 4 * i;
            blk.put_u32(at, target.wrapping_sub(at) as u32);
        }
        let mut node_slot = nodes.len();
        for (k, a) in adds.iter().enumerate() {
            let src = *ents.get(a.entity).ok_or_else(|| format!("no entity {}", a.entity))?;
            let src_hash = src.u32(1).ok_or("entity without hash")?;
            let hash = fresh(&mut used2, &mut s2);
            // entity table
            let et = copy_table(&mut blk, &src, &ent_ty, &schema, shift)?;
            let hf = et + (src.field(1).ok_or("entity hash field")? - src.pos);
            blk.put_u32(hf, hash);
            let at = ev + 4 + 4 * (ents.len() + k);
            blk.put_u32(at, (et - at) as u32);
            // node
            let mut node_id = None;
            if let Some(n) = nodes.iter().find(|n| n.u32(2) == Some(src_hash)) {
                let nt = copy_table(&mut blk, n, &node_ty, &schema, shift)?;
                let id = fresh(&mut used2, &mut s2);
                blk.put_u32(nt + (n.field(1).ok_or("node id field")? - n.pos), id);
                blk.put_u32(nt + (n.field(2).ok_or("node entity field")? - n.pos), hash);
                let at = nv + 4 + 4 * node_slot;
                blk.put_u32(at, (nt - at) as u32);
                node_slot += 1;
                node_id = Some(id);
            }
            // components vector
            let comps = src.tables(0);
            blk.align(4);
            let cv = blk.reserve(4 + 4 * comps.len());
            blk.put_u32(cv, comps.len() as u32);
            set_offset(&mut blk, et, &src, 0, cv)?;
            for (ci, c) in comps.iter().enumerate() {
                let wt = copy_table(&mut blk, c, &comp_ty, &schema, shift)?;
                let at = cv + 4 + 4 * ci;
                blk.put_u32(at, (wt - at) as u32);
                let Some((kind, sub)) = (1..c.slot_count()).find_map(|s| c.table(s).map(|t| (s, t))) else { continue };
                let sub_ty = schema.field_type(&comp_ty, kind).ok_or("schema: component kind")?.to_string();
                let st = copy_table(&mut blk, &sub, &sub_ty, &schema, shift)?;
                set_offset(&mut blk, wt, c, kind, st)?;
                // fresh component id in the s0 child table
                if let Some(idt) = sub.table(0) {
                    let id_ty = schema.field_type(&sub_ty, 0).ok_or("schema: id table")?.to_string();
                    let it = copy_table(&mut blk, &idt, &id_ty, &schema, shift)?;
                    let id = fresh(&mut used2, &mut s2);
                    blk.put_u32(it + (idt.field(0).ok_or("id field")? - idt.pos), id);
                    set_offset(&mut blk, st, &sub, 0, it)?;
                }
                for (k2, f2, vals) in &a.set {
                    if *k2 != kind {
                        continue;
                    }
                    let f = sub.field(*f2).ok_or_else(|| format!("component {kind} has no field {f2}"))?;
                    for (i, v) in vals.iter().enumerate() {
                        blk.put_u32(st + (f - sub.pos) + 4 * i, v.to_bits());
                    }
                }
                for (k2, f2, text) in &a.names {
                    if *k2 != kind {
                        continue;
                    }
                    if !schema.field_type(&sub_ty, *f2).is_some_and(|t| t == "string") {
                        return Err(format!("component {kind} field {f2} is not a string"));
                    }
                    // the string lies after the table, so the offset is forward
                    let sp = put_string(&mut blk, text);
                    set_offset(&mut blk, st, &sub, *f2, sp)?;
                }
                for (k2, f2, items) in &a.lists {
                    if *k2 != kind {
                        continue;
                    }
                    let el_ty = schema.field_type(&sub_ty, *f2).ok_or("schema: list")?;
                    let el_ty = el_ty.strip_prefix('[').and_then(|s| s.strip_suffix(']')).ok_or_else(|| format!("component {kind} field {f2} is not a vector"))?.to_string();
                    let first = *sub.tables(*f2).first().ok_or_else(|| format!("component {kind} list {f2} is empty"))?;
                    if schema.field_type(&el_ty, 0) != Some("string") || first.field(0).is_none() {
                        return Err(format!("component {kind} list {f2}: elements have no name"));
                    }
                    blk.align(4);
                    let vp = blk.reserve(4 + 4 * items.len());
                    blk.put_u32(vp, items.len() as u32);
                    set_offset(&mut blk, st, &sub, *f2, vp)?;
                    for (i, text) in items.iter().enumerate() {
                        let et = copy_table(&mut blk, &first, &el_ty, &schema, shift)?;
                        let at = vp + 4 + 4 * i;
                        blk.put_u32(at, (et - at) as u32);
                        let sp = put_string(&mut blk, text);
                        set_offset(&mut blk, et, &first, 0, sp)?;
                    }
                }
            }
            out_ids.push(Added { hash, node_id });
        }
        blk.align(16);
        if pass == 0 {
            size = blk.bytes.len();
            continue;
        }
        if blk.bytes.len() != size {
            return Err("internal: block size changed between passes".into());
        }
        // assemble: head (with corrected root offsets) + block + rest
        let mut out = Vec::with_capacity(bytes.len() + size);
        out.extend_from_slice(&bytes[..p_ins]);
        for slot in 0..root.slot_count() {
            let Some(f) = root.field(slot) else { continue };
            if !schema.is_offset(root_ty, slot)? {
                continue;
            }
            let v = u32::from_le_bytes(out[f..f + 4].try_into().unwrap());
            let nv_ = match slot {
                2 => (ev - f) as u32,
                3 => (nv - f) as u32,
                _ => v + size as u32,
            };
            out[f..f + 4].copy_from_slice(&nv_.to_le_bytes());
        }
        out.extend_from_slice(&blk.bytes);
        out.extend_from_slice(&bytes[p_ins..]);
        return Ok((out, out_ids));
    }
    unreachable!()
}

fn scalar_size(ty: &str) -> Option<usize> {
    Some(match ty {
        "bool" | "byte" | "ubyte" => 1,
        "short" | "ushort" => 2,
        "int" | "uint" | "float" => 4,
        "long" | "ulong" | "double" => 8,
        "Raw12" | "Vec3" => 12,
        "Raw16" | "Vec4" => 16,
        "Vec2" => 8,
        _ => return None,
    })
}

/// Canonical text of everything reachable from a table, by schema (positions excluded). `limit` caps the length of
/// the root's vectors in the listed slots, to compare a grown file with its original.
pub fn canon(schema: &Schema, t: &Table, ty: &str, limit: &[(usize, usize)], out: &mut String) -> Result<(), String> {
    use std::fmt::Write;
    let buf = t.buf;
    out.push('{');
    for slot in 0..t.slot_count() {
        let Some(f) = t.field(slot) else { continue };
        let fty = schema.field_type(ty, slot).ok_or_else(|| format!("schema has no {ty}.s{slot}"))?;
        let _ = write!(out, "{slot}:");
        if let Some(n) = scalar_size(fty) {
            let _ = write!(out, "{:02x?}", &buf.0[f..f + n]);
        } else if fty == "string" {
            let _ = write!(out, "{:?}", t.str(slot));
        } else if let Some(el) = fty.strip_prefix('[').and_then(|s| s.strip_suffix(']')) {
            let (p, mut n) = t.vec(slot).ok_or("bad vector")?;
            if let Some(l) = limit.iter().find(|l| l.0 == slot) {
                n = n.min(l.1);
            }
            out.push('[');
            if let Some(sz) = scalar_size(el) {
                let _ = write!(out, "{:02x?}", buf.0.get(p..p + sz * n).ok_or("vector past end")?);
            } else if el == "string" {
                for i in 0..n {
                    let e = p + 4 * i;
                    let s = e + buf.u32_at(e).ok_or("bad")? as usize;
                    let len = buf.u32_at(s).ok_or("bad")? as usize;
                    let _ = write!(out, "{:?},", String::from_utf8_lossy(buf.0.get(s + 4..s + 4 + len).ok_or("bad")?));
                }
            } else {
                for i in 0..n {
                    let e = p + 4 * i;
                    let c = Table::at(buf, e + buf.u32_at(e).ok_or("bad")? as usize).ok_or("bad table")?;
                    canon(schema, &c, el, &[], out)?;
                    out.push(',');
                }
            }
            out.push(']');
        } else {
            let c = t.table(slot).ok_or("bad child table")?;
            canon(schema, &c, fty, &[], out)?;
        }
        out.push(';');
    }
    out.push('}');
    Ok(())
}

/// Checks a grown file: everything the original holds is still there unchanged, and the new entities are clones of
/// their sources apart from ids and the set floats. Returns a short report.
/// The node id of entity `entity` (index in the root entity list): the number the course pack's baked Shadow and
/// Lightmap textures of that instance carry in their names (`<level>^<model>^<node id>^<shape>^<n>^...`).
pub fn node_id(bytes: &[u8], entity: usize) -> Option<u32> {
    let root = Buf(bytes).root()?;
    let h = root.tables(2).get(entity)?.u32(1)?;
    root.tables(3).iter().find(|n| n.u32(2) == Some(h))?.u32(1)
}

pub fn verify(orig: &[u8], grown: &[u8], adds: &[Add]) -> Result<String, String> {
    let schema = Schema::levelb();
    let (ro, rg) = (Buf(orig).root().ok_or("orig")?, Buf(grown).root().ok_or("grown")?);
    let (ne, nn) = (ro.tables(2).len(), ro.tables(3).len());
    let mut a = String::new();
    let mut b = String::new();
    canon(&schema, &ro, "LevelData", &[], &mut a)?;
    canon(&schema, &rg, "LevelData", &[(2, ne), (3, nn)], &mut b)?;
    if a != b {
        let i = a.bytes().zip(b.bytes()).position(|(x, y)| x != y).unwrap_or(0);
        return Err(format!("original content changed near char {i}: {:?} vs {:?}", &a[i.saturating_sub(40)..(i + 40).min(a.len())], &b[i.saturating_sub(40)..(i + 40).min(b.len())]));
    }
    let ge = rg.tables(2);
    if ge.len() != ne + adds.len() {
        return Err(format!("entity count {} expected {}", ge.len(), ne + adds.len()));
    }
    let gn = rg.tables(3);
    let hashes: HashSet<u32> = ge.iter().filter_map(|e| e.u32(1)).collect();
    if hashes.len() != ge.len() {
        return Err("duplicate entity hash".into());
    }
    let ids: HashSet<u32> = gn.iter().filter_map(|n| n.u32(1)).collect();
    if ids.len() != gn.len() {
        return Err("duplicate node id".into());
    }
    for (k, a) in adds.iter().enumerate() {
        for (kind, slot, text) in &a.names {
            let ok = ge[ne + k].tables(0).iter().filter_map(|c| c.table(*kind)).any(|t| t.str(*slot) == Some(text.as_str()));
            if !ok {
                return Err(format!("added entity {k}: component {kind} field {slot} is not {text:?}"));
            }
        }
        for (kind, slot, items) in &a.lists {
            let ok = ge[ne + k].tables(0).iter().filter_map(|c| c.table(*kind)).any(|t| {
                let v = t.tables(*slot);
                v.len() == items.len() && v.iter().zip(items).all(|(e, s)| e.str(0) == Some(s.as_str()))
            });
            if !ok {
                return Err(format!("added entity {k}: component {kind} list {slot} is not {items:?}"));
            }
        }
    }
    Ok(format!("{} bytes -> {} bytes; original content identical; entities {} -> {}; nodes {} -> {}", orig.len(), grown.len(), ne, ge.len(), nn, gn.len()))
}
