//! Baked sun shadows of hidden scenery: clearing them from the ground's shadow textures.
//!
//! Every lightmapped object of a course has a `<level>^<model>^<node id>^<shape>^<n>^Shadow.nutexb`: BC3 (sRGB) with
//! black color and the sun shadow in alpha (255 = in shadow), drawn through the object's last UV set. When the random
//! course hides trees along the rail, their shadows stay in these textures. This module finds, for each shadowed texel,
//! what casts it: all course triangles are drawn into a shadow map seen from the sun, one depth layer for the hidden
//! objects and one for everything that stays. A texel is cleared only when a hidden object covers it from the sun and
//! nothing that stays does, so cliffs, kept trees and hills keep their shadows. Only the 4x4 blocks that change are
//! re-encoded; every other byte of the file stays.
//!
//! The sun direction is the one `tools/hd-shadows` fitted against the originals (docs/hd-shadows.md, "Sun per course").

use glam::{Vec2, Vec3};

/// Direction toward the sun for a course, from the HD-shadows calibration. None for courses not calibrated yet.
pub fn sun_for(archive: &str) -> Option<Vec3> {
    let a = archive.to_ascii_lowercase();
    // Florio Day: azimuth about 5 to 6, elevation about 45 to 48 degrees
    let (az, el) = if a.contains("lc_001_a") { (5.5f32, 46.5f32) } else { return None };
    let (az, el) = (az.to_radians(), el.to_radians());
    Some(Vec3::new(el.cos() * az.cos(), el.sin(), el.cos() * az.sin()).normalize())
}

const FOOTER: usize = 0xb0;

/// A BC3 nutexb with one mip level, as the game's Shadow textures are.
pub struct Bc3 {
    pub w: usize,
    pub h: usize,
    bh_log2: u32,
}

impl Bc3 {
    pub fn parse(b: &[u8]) -> Result<Self, String> {
        if b.len() < FOOTER {
            return Err("too short for a nutexb".into());
        }
        let f = &b[b.len() - FOOTER..];
        let u = |o: usize| u32::from_le_bytes(f[o..o + 4].try_into().unwrap()) as usize;
        let (w, h, d, fmt, dim, mips, layers) = (u(0x84), u(0x88), u(0x8c), u(0x90), u(0x94), u(0x98), u(0xa0));
        if !(fmt == 0x4a0 || fmt == 0x4a5) || dim != 2 || mips != 1 || layers != 1 || d != 1 {
            return Err(format!("not a one-level BC3 texture (format {fmt:#x}, {mips} mips)"));
        }
        let hb = h.div_ceil(4);
        let hh = hb + hb / 2;
        let bh_log2 = if hh >= 128 { 4 } else if hh >= 64 { 3 } else if hh >= 32 { 2 } else if hh >= 16 { 1 } else { 0 };
        // level 0 keeps the texture's block height unless the texture is short
        let mut l = bh_log2;
        while l > 0 && (8usize << (l - 1)) >= hb {
            l -= 1;
        }
        let me = Bc3 { w, h, bh_log2: l };
        if me.size() > b.len() - FOOTER {
            return Err("layout is larger than the data".into());
        }
        Ok(me)
    }
    fn wb(&self) -> usize {
        self.w.div_ceil(4)
    }
    fn hb(&self) -> usize {
        self.h.div_ceil(4)
    }
    fn size(&self) -> usize {
        let rowb = self.wb() * 16;
        rowb.div_ceil(64) * (512 << self.bh_log2) * self.hb().div_ceil(8 << self.bh_log2)
    }
    /// Byte offset of block (bx, by) in the Tegra block-linear layout (16-byte blocks sit whole in one GOB line).
    pub fn block_at(&self, bx: usize, by: usize) -> usize {
        let x = bx * 16;
        let y = by;
        let gob_rows = 8usize << self.bh_log2;
        let width_gobs = (self.wb() * 16).div_ceil(64);
        let block_size = 512usize << self.bh_log2;
        (y / gob_rows) * width_gobs * block_size + (x / 64) * block_size + ((y % gob_rows) / 8) * 512 + ((x % 64) / 32) * 256
            + ((y % 8) / 2) * 64
            + ((x % 32) / 16) * 32
            + (y % 2) * 16
    }
}

/// The 16 alpha values of a BC3 block's alpha half (row-major in the 4x4 block).
pub fn alpha_decode(a: &[u8]) -> [u8; 16] {
    let (a0, a1) = (a[0] as u32, a[1] as u32);
    let mut pal = [0u32; 8];
    pal[0] = a0;
    pal[1] = a1;
    if a0 > a1 {
        for k in 1..7 {
            pal[k + 1] = ((7 - k as u32) * a0 + k as u32 * a1 + 3) / 7;
        }
    } else {
        for k in 1..5 {
            pal[k + 1] = ((5 - k as u32) * a0 + k as u32 * a1 + 2) / 5;
        }
        pal[6] = 0;
        pal[7] = 255;
    }
    let mut bits = 0u64;
    for i in 0..6 {
        bits |= (a[2 + i] as u64) << (8 * i);
    }
    let mut out = [0u8; 16];
    for (i, o) in out.iter_mut().enumerate() {
        *o = pal[((bits >> (3 * i)) & 7) as usize] as u8;
    }
    out
}

/// An alpha half for 16 values: the 8-value mode between the block's lowest and highest value.
pub fn alpha_encode(v: &[u8; 16]) -> [u8; 8] {
    let hi = *v.iter().max().unwrap();
    let lo = *v.iter().min().unwrap();
    let mut out = [0u8; 8];
    out[0] = hi;
    out[1] = lo;
    if hi == lo {
        return out;
    }
    let span = (hi - lo) as f32;
    let mut bits = 0u64;
    for (i, &x) in v.iter().enumerate() {
        let l = (((x - lo) as f32) / span * 7.0).round().clamp(0.0, 7.0) as u64;
        let code = if l == 7 { 0 } else if l == 0 { 1 } else { 8 - l };
        bits |= code << (3 * i);
    }
    for i in 0..6 {
        out[2 + i] = (bits >> (8 * i)) as u8;
    }
    out
}

/// What the sun sees: two depth layers (distance toward the sun) over a grid across the sun's direction.
pub struct SunMap {
    s: Vec3,
    u: Vec3,
    v: Vec3,
    min: Vec2,
    cell: f32,
    nx: usize,
    ny: usize,
    /// highest depth of anything that stays
    kept: Vec<f32>,
    /// highest depth of the hidden objects
    hidden: Vec<f32>,
}

impl SunMap {
    /// A map covering the shadows of `hidden` (world-space triangles), cells of `cell` units.
    pub fn new(sun: Vec3, hidden: &[[Vec3; 3]], cell: f32, margin: f32) -> Option<Self> {
        let s = sun.normalize();
        let u = Vec3::Y.cross(s).normalize_or_zero();
        let u = if u == Vec3::ZERO { Vec3::X } else { u };
        let v = s.cross(u).normalize();
        let (mut lo, mut hi) = (Vec2::splat(f32::MAX), Vec2::splat(f32::MIN));
        for t in hidden {
            for p in t {
                let q = Vec2::new(p.dot(u), p.dot(v));
                lo = lo.min(q);
                hi = hi.max(q);
            }
        }
        if lo.x > hi.x {
            return None;
        }
        let min = lo - Vec2::splat(margin);
        let size = hi - lo + Vec2::splat(2.0 * margin);
        let (nx, ny) = ((size.x / cell).ceil() as usize + 1, (size.y / cell).ceil() as usize + 1);
        if nx * ny > 64_000_000 {
            return None;
        }
        let mut m = SunMap { s, u, v, min, cell, nx, ny, kept: vec![f32::MIN; nx * ny], hidden: vec![f32::MIN; nx * ny] };
        for t in hidden {
            m.draw(t, true);
        }
        Some(m)
    }
    fn cell_of(&self, p: Vec3) -> Option<(usize, usize, f32)> {
        let q = (Vec2::new(p.dot(self.u), p.dot(self.v)) - self.min) / self.cell;
        if q.x < 0.0 || q.y < 0.0 {
            return None;
        }
        let (x, y) = (q.x as usize, q.y as usize);
        (x < self.nx && y < self.ny).then_some((x, y, p.dot(self.s)))
    }
    /// Draws one triangle into a layer: every cell whose center it covers, plus the cells of its corners (thin leaves).
    pub fn draw(&mut self, t: &[Vec3; 3], hidden: bool) {
        let q: Vec<Vec2> = t.iter().map(|p| (Vec2::new(p.dot(self.u), p.dot(self.v)) - self.min) / self.cell).collect();
        let d: Vec<f32> = t.iter().map(|p| p.dot(self.s)).collect();
        let lo = q[0].min(q[1]).min(q[2]);
        let hi = q[0].max(q[1]).max(q[2]);
        if hi.x < 0.0 || hi.y < 0.0 || lo.x >= self.nx as f32 || lo.y >= self.ny as f32 {
            return;
        }
        let nx = self.nx;
        let layer = if hidden { &mut self.hidden } else { &mut self.kept };
        let mut put = |x: usize, y: usize, z: f32| {
            let c = &mut layer[y * nx + x];
            if z > *c {
                *c = z;
            }
        };
        for k in 0..3 {
            if q[k].x >= 0.0 && q[k].y >= 0.0 && (q[k].x as usize) < self.nx && (q[k].y as usize) < self.ny {
                put(q[k].x as usize, q[k].y as usize, d[k]);
            }
        }
        let det = (q[1].x - q[0].x) * (q[2].y - q[0].y) - (q[1].y - q[0].y) * (q[2].x - q[0].x);
        if det.abs() < 1e-9 {
            return;
        }
        let x0 = lo.x.floor().max(0.0) as usize;
        let y0 = lo.y.floor().max(0.0) as usize;
        let x1 = (hi.x.ceil() as usize).min(self.nx - 1);
        let y1 = (hi.y.ceil() as usize).min(self.ny - 1);
        for y in y0..=y1 {
            for x in x0..=x1 {
                let p = Vec2::new(x as f32 + 0.5, y as f32 + 0.5);
                let w1 = ((p.x - q[0].x) * (q[2].y - q[0].y) - (p.y - q[0].y) * (q[2].x - q[0].x)) / det;
                let w2 = ((q[1].x - q[0].x) * (p.y - q[0].y) - (q[1].y - q[0].y) * (p.x - q[0].x)) / det;
                let w0 = 1.0 - w1 - w2;
                if w0 >= 0.0 && w1 >= 0.0 && w2 >= 0.0 {
                    put(x, y, w0 * d[0] + w1 * d[1] + w2 * d[2]);
                }
            }
        }
    }
    /// Whether the point's shadow comes from hidden objects only: something hidden lies between it and the sun (within
    /// one cell, for soft edges), and nothing that stays does.
    pub fn hidden_only(&self, p: Vec3, bias: f32) -> bool {
        let Some((x, y, d)) = self.cell_of(p) else { return false };
        if self.kept[y * self.nx + x] > d + bias {
            return false;
        }
        for yy in y.saturating_sub(1)..=(y + 1).min(self.ny - 1) {
            for xx in x.saturating_sub(1)..=(x + 1).min(self.nx - 1) {
                if self.hidden[yy * self.nx + xx] > d + bias {
                    return true;
                }
            }
        }
        false
    }
    /// Whether anything (hidden or not) lies between the point and the sun: the shadow the bake should have.
    pub fn any(&self, p: Vec3, bias: f32) -> bool {
        let Some((x, y, d)) = self.cell_of(p) else { return false };
        self.kept[y * self.nx + x] > d + bias || self.hidden[y * self.nx + x] > d + bias
    }
    pub fn covers(&self, p: Vec3) -> bool {
        self.cell_of(p).is_some()
    }
}

/// Counts of one texture's pass: texels cleared, and how well the map agrees with the bake on the shadowed texels it
/// covers (shadowed texels the map also finds in shadow, out of all shadowed texels it covers).
#[derive(Default, Debug, Clone, Copy)]
pub struct Stats {
    pub cleared: usize,
    pub shadowed: usize,
    pub agree: usize,
}

/// One Shadow texture with the hidden objects' shadows cleared, or None when nothing changes. `uv`, `pos` and `idx`
/// are the receiving object's light-map UVs, world positions and triangles.
pub fn clear_texture(b: &[u8], uv: &[[f32; 2]], pos: &[Vec3], idx: &[u32], map: &SunMap, bias: f32) -> Result<(Option<Vec<u8>>, Stats), String> {
    let t = Bc3::parse(b)?;
    let (w, h) = (t.w, t.h);
    // 0 = not on the object, 1 = keep, 2 = clear
    let mut mark = vec![0u8; w * h];
    let mut st = Stats::default();
    let alpha_at = |x: usize, y: usize| -> u8 {
        let o = t.block_at(x / 4, y / 4);
        alpha_decode(&b[o..o + 8])[(y % 4) * 4 + x % 4]
    };
    for tri in idx.chunks_exact(3) {
        let (i0, i1, i2) = (tri[0] as usize, tri[1] as usize, tri[2] as usize);
        if i0 >= pos.len() || i1 >= pos.len() || i2 >= pos.len() {
            continue;
        }
        if !(map.covers(pos[i0]) || map.covers(pos[i1]) || map.covers(pos[i2])) {
            continue;
        }
        let f = |p: [f32; 2]| Vec2::new(p[0] * w as f32, p[1] * h as f32);
        let (a, bb, c) = (f(uv[i0]), f(uv[i1]), f(uv[i2]));
        let det = (bb.x - a.x) * (c.y - a.y) - (bb.y - a.y) * (c.x - a.x);
        if det.abs() < 1e-9 {
            continue;
        }
        let lo = a.min(bb).min(c);
        let hi = a.max(bb).max(c);
        let x0 = lo.x.floor().max(0.0) as usize;
        let y0 = lo.y.floor().max(0.0) as usize;
        let x1 = (hi.x.ceil() as usize).min(w - 1);
        let y1 = (hi.y.ceil() as usize).min(h - 1);
        if x1 < x0 || y1 < y0 || (x1 - x0) * (y1 - y0) > 4_000_000 {
            continue;
        }
        for y in y0..=y1 {
            for x in x0..=x1 {
                let p = Vec2::new(x as f32 + 0.5, y as f32 + 0.5);
                let w1 = ((p.x - a.x) * (c.y - a.y) - (p.y - a.y) * (c.x - a.x)) / det;
                let w2 = ((bb.x - a.x) * (p.y - a.y) - (bb.y - a.y) * (p.x - a.x)) / det;
                let w0 = 1.0 - w1 - w2;
                if w0 < -1e-4 || w1 < -1e-4 || w2 < -1e-4 || mark[y * w + x] != 0 {
                    continue;
                }
                let wp = pos[i0] * w0 + pos[i1] * w1 + pos[i2] * w2;
                let al = alpha_at(x, y);
                if !map.covers(wp) {
                    mark[y * w + x] = 1;
                    continue;
                }
                if al >= 128 {
                    st.shadowed += 1;
                    if map.any(wp, bias) {
                        st.agree += 1;
                    }
                }
                mark[y * w + x] = if al > 0 && map.hidden_only(wp, bias) { 2 } else { 1 };
            }
        }
    }
    // the gutter around a cleared texel (off the object, sampled by filtering at island edges) is cleared too
    let src = mark.clone();
    for y in 0..h {
        for x in 0..w {
            if src[y * w + x] != 0 {
                continue;
            }
            let near = (y.saturating_sub(1)..=(y + 1).min(h - 1)).any(|yy| (x.saturating_sub(1)..=(x + 1).min(w - 1)).any(|xx| src[yy * w + xx] == 2));
            if near {
                mark[y * w + x] = 2;
            }
        }
    }
    let mut out = b.to_vec();
    let mut changed = false;
    for by in 0..t.hb() {
        for bx in 0..t.wb() {
            let o = t.block_at(bx, by);
            let mut a = alpha_decode(&b[o..o + 8]);
            let mut any = false;
            for k in 0..16 {
                let (x, y) = (bx * 4 + k % 4, by * 4 + k / 4);
                if x < w && y < h && mark[y * w + x] == 2 && a[k] != 0 {
                    a[k] = 0;
                    st.cleared += 1;
                    any = true;
                }
            }
            if any {
                out[o..o + 8].copy_from_slice(&alpha_encode(&a));
                changed = true;
            }
        }
    }
    Ok((changed.then_some(out), st))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn alpha_block_round_trips() {
        let v: [u8; 16] = [0, 0, 255, 255, 0, 36, 73, 109, 146, 182, 219, 255, 0, 0, 0, 0];
        let e = alpha_encode(&v);
        let d = alpha_decode(&e);
        for k in 0..16 {
            assert!((d[k] as i32 - v[k] as i32).abs() <= 1, "{k}: {} {}", d[k], v[k]);
        }
        assert_eq!(alpha_decode(&alpha_encode(&[7; 16])), [7; 16]);
    }

    #[test]
    fn sun_map_tells_hidden_from_kept() {
        let sun = Vec3::new(0.0, 1.0, 0.0);
        let hid = [[Vec3::new(0.0, 5.0, 0.0), Vec3::new(2.0, 5.0, 0.0), Vec3::new(0.0, 5.0, 2.0)]];
        let mut m = SunMap::new(sun, &hid, 0.1, 4.0).unwrap();
        // under the hidden triangle only
        assert!(m.hidden_only(Vec3::new(0.3, 0.0, 0.3), 0.2));
        // beside it nothing casts a shadow
        assert!(!m.hidden_only(Vec3::new(3.0, 0.0, 3.0), 0.2));
        // a kept roof over the same spot keeps the shadow
        m.draw(&[Vec3::new(-1.0, 8.0, -1.0), Vec3::new(3.0, 8.0, -1.0), Vec3::new(-1.0, 8.0, 3.0)], false);
        assert!(!m.hidden_only(Vec3::new(0.3, 0.0, 0.3), 0.2));
        assert!(m.any(Vec3::new(0.3, 0.0, 0.3), 0.2));
    }
}
