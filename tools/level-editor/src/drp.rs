//! DRP archive rewrite: add new entries and dependency groups to a course archive, written as a whole-file mod
//! (`<pack>/romfs/<archive>`, served by the loader's rom layer). Port of `scripts/hd/drp_add_entries.py`.
//!
//! Layout: a 0x94-byte header (magic, seed, three encrypted words: marker, ?, entry count; then a 128-byte table that
//! seeds the per-chunk xorshift streams), the chunks ("resd" blocks: crc, magic, total, ext id, csize, dsize, name, data),
//! then the dependency table ("footer"): groups of [magic, n, child count, name, n chunk indices] in preorder. A course
//! loads its models through these groups, so a new model is new chunks plus a new group under the level's group.
//! Proven in game 2026-11-15 (run 3-20261115_234909: a copied tree group under a new name rendered on Florio).
//! The stream cipher here is the archive's own scrambling, keyed by the table in its header; no outside key is involved.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

const MAGIC: u32 = 0x7F0E_5359;
const FHDR: u32 = 0xC657_53E8;
const RESD: u32 = 0xE0A3_31B4;

struct XorShift([u32; 4]);

impl XorShift {
    fn new(seed: u32) -> Self {
        let w = 0x41C6_4E6Du32.wrapping_mul(seed).wrapping_add(12345);
        let z = 0x41C6_4E6Du32.wrapping_mul(w).wrapping_add(12345);
        let y = 0x41C6_4E6Du32.wrapping_mul(z).wrapping_add(12345);
        let x = 0x41C6_4E6Du32.wrapping_mul(y).wrapping_add(12345);
        Self([x, y, z, w])
    }
    fn next(&mut self) -> u32 {
        let [x, y, z, w] = self.0;
        let t = x ^ (x << 11);
        let nw = w ^ ((w as i32 >> 19) as u32) ^ (t ^ ((t as i32 >> 8) as u32));
        self.0 = [y, z, w, nw];
        nw & 0x7FFF_FFFF
    }
    fn word(&mut self) -> u32 {
        let a = self.next();
        let b = self.next() >> 30;
        a | (b << 31)
    }
}

fn rng_for(seed: u32, table: &[u8]) -> XorShift {
    let mut key = 0u32;
    for i in 0..4 {
        key |= (table[((seed >> (i * 8)) & 0x7F) as usize] as u32) << (i * 8);
    }
    XorShift::new(key)
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

fn xor_words(buf: &mut [u8], mut r: XorShift, start: usize) {
    for i in start..buf.len() / 4 {
        let v = u32_at(buf, 4 * i) ^ r.word();
        buf[4 * i..4 * i + 4].copy_from_slice(&v.to_le_bytes());
    }
}

#[derive(Clone, Debug)]
pub struct Entry {
    /// stored name, with its extension (e.g. "SM_ENV_001_1_Obj.modelb")
    pub name: String,
    pub ext: u32,
    pub off: u64,
    pub total: u32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Group {
    pub magic: u32,
    pub kids: u32,
    pub name: String,
    pub idx: Vec<u32>,
}

pub struct Archive {
    path: std::path::PathBuf,
    /// decrypted header
    hd: Vec<u8>,
    encrypted: bool,
    pub entries: Vec<Entry>,
    /// end of the last chunk
    end: u64,
    pub groups: Vec<Group>,
}

/// DRP extension id: crc32 of the lowercase extension, except nutexb.
pub fn ext_id(ext: &str) -> u32 {
    let e = ext.to_ascii_lowercase();
    if e == "nutexb" {
        0x5C15_6DBC
    } else {
        crc32fast::hash(e.as_bytes())
    }
}

impl Archive {
    pub fn open(path: &Path) -> Result<Self, String> {
        let err = |e: std::io::Error| format!("{}: {e}", path.display());
        let mut f = File::open(path).map_err(err)?;
        let size = f.metadata().map_err(err)?.len();
        let mut hd = vec![0u8; 0x94];
        f.read_exact(&mut hd).map_err(err)?;
        if u32_at(&hd, 0) != MAGIC {
            return Err(format!("{}: not a DRP archive", path.display()));
        }
        let seed = u32_at(&hd, 4);
        let encrypted = u32_at(&hd, 8) != FHDR;
        if encrypted {
            let mut r = rng_for(seed, &hd[0x14..0x94]);
            for off in [8usize, 0xC, 0x10] {
                let v = u32_at(&hd, off) ^ r.word();
                hd[off..off + 4].copy_from_slice(&v.to_le_bytes());
            }
        }
        if crc32fast::hash(&hd[0x10..0x94]) != seed {
            return Err(format!("{}: header check failed", path.display()));
        }
        let count = u32_at(&hd, 0x10) as usize;
        let mut entries = Vec::with_capacity(count);
        let mut pos = 0x94u64;
        let mut raw = vec![0u8; 0x200];
        for i in 0..count {
            f.seek(SeekFrom::Start(pos)).map_err(err)?;
            let n = (size - pos).min(0x200) as usize;
            raw.resize(n, 0);
            f.read_exact(&mut raw[..n]).map_err(err)?;
            let mut dec = raw[..n & !3].to_vec();
            if encrypted {
                let s = u32_at(&dec, 0);
                xor_words(&mut dec, rng_for(s, &hd[0x14..0x94]), 1);
            }
            let (magic, total, ext) = (u32_at(&dec, 4), u32_at(&dec, 8), u32_at(&dec, 12));
            if magic != RESD || total < 0x20 || pos + total as u64 > size {
                return Err(format!("{}: bad chunk {i} at {pos:#x}", path.display()));
            }
            let nul = dec[0x18..].iter().position(|&c| c == 0).ok_or("chunk name too long")? + 0x18;
            let name = String::from_utf8_lossy(&dec[0x18..nul]).into_owned();
            entries.push(Entry { name, ext, off: pos, total });
            pos += total as u64;
        }
        let end = pos;
        let mut foot = vec![0u8; (size - end) as usize];
        f.seek(SeekFrom::Start(end)).map_err(err)?;
        f.read_exact(&mut foot).map_err(err)?;
        if foot.len() % 4 != 0 {
            return Err("footer is not a whole number of words".into());
        }
        if encrypted {
            xor_words(&mut foot, rng_for(seed, &hd[0x14..0x94]), 0);
        }
        let groups = parse_groups(&foot)?;
        Ok(Self { path: path.to_path_buf(), hd, encrypted, entries, end, groups })
    }

    pub fn find_entry(&self, name: &str) -> Option<usize> {
        self.entries.iter().position(|e| e.name.eq_ignore_ascii_case(name))
    }
    pub fn find_group(&self, name: &str) -> Option<usize> {
        self.groups.iter().position(|g| g.name == name)
    }
    /// Parent group of each group (preorder with child counts).
    pub fn parents(&self) -> Vec<Option<usize>> {
        let mut parent = vec![None; self.groups.len()];
        let mut stack: Vec<(usize, u32)> = vec![];
        for (i, g) in self.groups.iter().enumerate() {
            while stack.last().is_some_and(|s| s.1 == 0) {
                stack.pop();
            }
            if let Some(s) = stack.last_mut() {
                parent[i] = Some(s.0);
                s.1 -= 1;
            }
            stack.push((i, g.kids));
        }
        parent
    }
    /// One past the last group of the subtree rooted at `i`.
    pub fn subtree_end(&self, i: usize) -> usize {
        let mut j = i + 1;
        for _ in 0..self.groups[i].kids {
            j = self.subtree_end(j);
        }
        j
    }

    /// Writes the archive with `new` chunks appended (stored uncompressed) and `groups` as the dependency table. Original
    /// chunks are copied byte for byte.
    pub fn write(&self, new: &[(String, Vec<u8>)], groups: &[Group], out: &Path) -> Result<(), String> {
        let table = self.hd[0x14..0x94].to_vec();
        let mut hd = self.hd.clone();
        hd[0x10..0x14].copy_from_slice(&((self.entries.len() + new.len()) as u32).to_le_bytes());
        let seed = crc32fast::hash(&hd[0x10..0x94]);
        hd[4..8].copy_from_slice(&seed.to_le_bytes());
        let mut foot = build_groups(groups);
        if self.encrypted {
            let mut r = rng_for(seed, &table);
            for off in [8usize, 0xC, 0x10] {
                let v = u32_at(&hd, off) ^ r.word();
                hd[off..off + 4].copy_from_slice(&v.to_le_bytes());
            }
            xor_words(&mut foot, rng_for(seed, &table), 0);
        }
        if let Some(d) = out.parent() {
            std::fs::create_dir_all(d).map_err(|e| e.to_string())?;
        }
        let err = |e: std::io::Error| format!("{}: {e}", out.display());
        let tmp = out.with_extension("drp.part");
        {
            let mut w = std::io::BufWriter::with_capacity(1 << 20, File::create(&tmp).map_err(err)?);
            w.write_all(&hd).map_err(err)?;
            let mut src = File::open(&self.path).map_err(err)?;
            src.seek(SeekFrom::Start(0x94)).map_err(err)?;
            let copied = std::io::copy(&mut (&mut src).take(self.end - 0x94), &mut w).map_err(err)?;
            if copied != self.end - 0x94 {
                return Err("source archive shorter than its index".into());
            }
            for (name, data) in new {
                let ext = name.rsplit_once('.').map(|x| x.1).ok_or_else(|| format!("{name}: no extension"))?;
                w.write_all(&make_chunk(name, ext_id(ext), data, &table, self.encrypted)).map_err(err)?;
            }
            w.write_all(&foot).map_err(err)?;
            w.flush().map_err(err)?;
        }
        std::fs::rename(&tmp, out).map_err(err)?;
        Ok(())
    }

    /// The stored payload of entry `i` if it is uncompressed (the chunks this module writes); None for Oodle chunks.
    pub fn raw_payload(&self, i: usize) -> Result<Option<Vec<u8>>, String> {
        let e = &self.entries[i];
        let mut f = File::open(&self.path).map_err(|x| x.to_string())?;
        f.seek(SeekFrom::Start(e.off)).map_err(|x| x.to_string())?;
        let mut b = vec![0u8; e.total as usize];
        f.read_exact(&mut b).map_err(|x| x.to_string())?;
        if self.encrypted {
            let s = u32_at(&b, 0);
            xor_words(&mut b, rng_for(s, &self.hd[0x14..0x94]), 1);
        }
        let (csize, dsize) = (u32_at(&b, 16) as usize, u32_at(&b, 20) as usize);
        if csize != dsize {
            return Ok(None);
        }
        let nul = b[0x18..].iter().position(|&c| c == 0).ok_or("name")? + 0x18;
        let at = (nul + 4) & !3;
        let data = b.get(at..at + dsize).ok_or("chunk shorter than its size")?.to_vec();
        if crc32fast::hash(&data) != u32_at(&b, 0) {
            return Err(format!("{}: payload crc differs", e.name));
        }
        Ok(Some(data))
    }
}

fn make_chunk(name: &str, ext: u32, data: &[u8], table: &[u8], enc: bool) -> Vec<u8> {
    let nm = name.as_bytes();
    let hdr = 0x18 + ((nm.len() + 4) & !3);
    let pad = (4 - data.len() % 4) % 4;
    let mut b = vec![0u8; hdr + data.len() + pad];
    let crc = crc32fast::hash(data);
    for (i, v) in [crc, RESD, b.len() as u32, ext, data.len() as u32, data.len() as u32].iter().enumerate() {
        b[4 * i..4 * i + 4].copy_from_slice(&v.to_le_bytes());
    }
    b[0x18..0x18 + nm.len()].copy_from_slice(nm);
    b[hdr..hdr + data.len()].copy_from_slice(data);
    if enc {
        xor_words(&mut b, rng_for(crc, table), 1);
    }
    b
}

pub fn parse_groups(foot: &[u8]) -> Result<Vec<Group>, String> {
    let w = |i: usize| u32_at(foot, 4 * i);
    let nw = foot.len() / 4;
    let mut out = vec![];
    if nw == 0 {
        return Ok(out);
    }
    let magic = w(0);
    let mut i = 0;
    while i < nw {
        if w(i) != magic || i + 3 > nw {
            return Err(format!("footer: no group at word {i}"));
        }
        let (n, kids) = (w(i + 1) as usize, w(i + 2));
        let j = (i + 3) * 4;
        let e = foot[j..].iter().position(|&c| c == 0).ok_or("footer: unterminated name")? + j;
        let name = String::from_utf8_lossy(&foot[j..e]).into_owned();
        let k = (e + 4) / 4;
        if k + n > nw {
            return Err("footer: group runs past the end".into());
        }
        out.push(Group { magic, kids, name, idx: (k..k + n).map(w).collect() });
        i = k + n;
    }
    Ok(out)
}

pub fn build_groups(groups: &[Group]) -> Vec<u8> {
    let mut b = vec![];
    for g in groups {
        for v in [g.magic, g.idx.len() as u32, g.kids] {
            b.extend_from_slice(&v.to_le_bytes());
        }
        b.extend_from_slice(g.name.as_bytes());
        b.resize(b.len() + 4 - g.name.len() % 4, 0);
        for v in &g.idx {
            b.extend_from_slice(&v.to_le_bytes());
        }
    }
    b
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn groups_round_trip() {
        let g = vec![
            Group { magic: 7, kids: 1, name: String::new(), idx: vec![0] },
            Group { magic: 7, kids: 0, name: "abc.drl".into(), idx: vec![1, 2, 3] },
            Group { magic: 7, kids: 0, name: "abcd".into(), idx: vec![] },
        ];
        assert_eq!(parse_groups(&build_groups(&g)).unwrap(), g);
    }

    #[test]
    fn chunk_decrypts() {
        let table: Vec<u8> = (0..128u32).map(|i| (i * 37 + 11) as u8).collect();
        let data = b"hello scenery".to_vec();
        let mut c = make_chunk("x.modelb", ext_id("modelb"), &data, &table, true);
        let s = u32_at(&c, 0);
        xor_words(&mut c, rng_for(s, &table), 1);
        assert_eq!(u32_at(&c, 4), RESD);
        assert_eq!(&c[0x18..0x20], b"x.modelb");
        assert_eq!(&c[0x24..0x24 + data.len()], &data[..]);
    }
}
