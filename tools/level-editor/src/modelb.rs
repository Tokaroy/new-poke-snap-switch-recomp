//! Writes a `.modelb` (the game's per-model list of shape/material pairs; schema `analysis/registries/schemas/modelb.fbs`)
//! for an imported model. The root is a copy of a template model's root (every scalar kept); each entry is a copy of the
//! template's first entry with its own shape name (slot 0) and material label (slot 1).

use crate::fb::{Buf, Table};

struct W {
    out: Vec<u8>,
}

impl W {
    fn align(&mut self, a: usize) {
        while self.out.len() % a != 0 {
            self.out.push(0);
        }
    }
    fn put_u32(&mut self, at: usize, v: u32) {
        self.out[at..at + 4].copy_from_slice(&v.to_le_bytes());
    }
    /// Copies a table's vtable and inline bytes; offset fields still hold the source's values and must be patched.
    fn copy_table(&mut self, src: &Table) -> usize {
        let b = src.buf.0;
        let (vt, vs, ts) = (src.vtable_pos(), src.vtable_size(), src.inline_size());
        self.align(2);
        let vpos = self.out.len();
        self.out.extend_from_slice(&b[vt..vt + vs]);
        while self.out.len() % 16 != src.pos % 16 {
            self.out.push(0);
        }
        let pos = self.out.len();
        self.out.extend_from_slice(&b[src.pos..src.pos + ts]);
        self.put_u32(pos, (pos - vpos) as u32);
        pos
    }
    fn link(&mut self, table_pos: usize, src: &Table, slot: usize, target: usize) -> Result<(), String> {
        let f = table_pos + (src.field(slot).ok_or_else(|| format!("template has no slot {slot}"))? - src.pos);
        if target <= f {
            return Err("internal: backward offset".into());
        }
        self.put_u32(f, (target - f) as u32);
        Ok(())
    }
    fn string(&mut self, s: &str) -> usize {
        self.align(4);
        let p = self.out.len();
        self.out.extend_from_slice(&(s.len() as u32).to_le_bytes());
        self.out.extend_from_slice(s.as_bytes());
        self.out.push(0);
        p
    }
}

/// Builds a modelb from `template` (any scenery model's modelb) with one entry per (material label, shape name).
pub fn build(template: &[u8], entries: &[(String, String)]) -> Result<Vec<u8>, String> {
    let root = Buf(template).root().ok_or("template modelb: not a FlatBuffers file")?;
    let e0 = *root.tables(3).first().ok_or("template modelb has no entries")?;
    let s9 = e0.table(9).ok_or("template entry has no slot 9 table")?;
    if entries.is_empty() {
        return Err("no entries".into());
    }
    let mut w = W { out: vec![0; 4] };
    let rt = w.copy_table(&root);
    w.put_u32(0, rt as u32);
    // root slot 2 (a name string, often empty) and slot 3 (the entry vector)
    w.align(4);
    let vec_pos = w.out.len();
    w.out.resize(vec_pos + 4 + 4 * entries.len(), 0);
    w.put_u32(vec_pos, entries.len() as u32);
    w.link(rt, &root, 3, vec_pos)?;
    for (i, (mat, shape)) in entries.iter().enumerate() {
        let et = w.copy_table(&e0);
        let at = vec_pos + 4 + 4 * i;
        w.put_u32(at, (et - at) as u32);
        let st = w.copy_table(&s9);
        w.link(et, &e0, 9, st)?;
        let p0 = w.string(shape);
        w.link(et, &e0, 0, p0)?;
        let p1 = w.string(mat);
        w.link(et, &e0, 1, p1)?;
    }
    let name = root.str(2).unwrap_or("");
    let p2 = w.string(name);
    w.link(rt, &root, 2, p2)?;
    w.align(4);
    verify(template, &w.out, entries)?;
    Ok(w.out)
}

/// Re-reads a built modelb: the root's inline bytes match the template's (apart from offsets) and the entries are the
/// requested pairs, each a copy of the template's first entry.
pub fn verify(template: &[u8], built: &[u8], entries: &[(String, String)]) -> Result<(), String> {
    let (rt, rb) = (Buf(template).root().ok_or("template")?, Buf(built).root().ok_or("built: no root")?);
    for slot in [1usize, 5, 7, 8, 9, 10, 11, 12, 13, 14, 17, 18, 19, 20] {
        let (a, b) = (rt.field(slot), rb.field(slot));
        if a.is_some() != b.is_some() {
            return Err(format!("root slot {slot} presence differs"));
        }
        if let (Some(a), Some(b)) = (a, b) {
            if template[a..a + 4.min(template.len() - a)] != built[b..b + 4.min(built.len() - b)] {
                return Err(format!("root slot {slot} differs"));
            }
        }
    }
    if rt.str(2) != rb.str(2) {
        return Err("root name differs".into());
    }
    let got = rb.tables(3);
    if got.len() != entries.len() {
        return Err(format!("{} entries, expected {}", got.len(), entries.len()));
    }
    let e0 = rt.tables(3)[0];
    for (t, (m, s)) in got.iter().zip(entries) {
        if t.str(0) != Some(s.as_str()) || t.str(1) != Some(m.as_str()) {
            return Err(format!("entry {:?}/{:?} expected {s}/{m}", t.str(0), t.str(1)));
        }
        for slot in [2usize, 3, 5, 10, 11, 12, 13, 14, 15, 16] {
            if e0.u32(slot) != t.u32(slot) || e0.u8(slot) != t.u8(slot) {
                return Err(format!("entry slot {slot} differs from the template"));
            }
        }
        let (a, b) = (e0.table(9).ok_or("s9")?, t.table(9).ok_or("built s9")?);
        for slot in [1usize, 3, 4, 6] {
            if a.u32(slot) != b.u32(slot) {
                return Err(format!("entry s9 slot {slot} differs"));
            }
        }
    }
    Ok(())
}
