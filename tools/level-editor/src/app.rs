//! Editor UI (egui). Two layers over one data model: the beginner view (select, drag along the ground, face the rail,
//! plain-language checks, one-button export) and the advanced view (raw offsets, hashes, quaternions, every model
//! instance, level parts). Every change goes through `edit::History`, so it can be undone and only the edited floats
//! change in the exported files.

use crate::course::{CourseEntry, DataRoot, LoadedCourse, Variant};
use crate::edit::{self, History, Issue, Sel, Writes};
use crate::gizmo;
use crate::place;
use crate::level::kind_name;
use crate::pick::{self, Ground};
use crate::render::{self, GpuScene, Layers, OrbitCamera, Viewport};
use egui_wgpu::wgpu;
use glam::Vec3;
use std::path::PathBuf;

pub struct Gpu<'a> {
    pub device: &'a wgpu::Device,
    pub queue: &'a wgpu::Queue,
    pub egui: &'a mut egui_wgpu::Renderer,
}

enum Action {
    Select(Option<Sel>),
    Focus(Vec3),
    MoveTo(Sel, Vec3),
    SetYaw(Sel, f32),
    SetScale(usize, Vec3),
    DropToGround(Sel),
    FaceRail(Sel),
    /// Copy a selection; at the given point, or beside the original when None.
    Copy(Sel, Option<Vec3>),
    Remove(Sel),
    ResetRail,
    Undo,
    Redo,
    Export(PathBuf),
    OpenCourse(usize),
    LoadVariant(u32),
    Rebuild,
    /// Makes this the object that Place and Scatter put down.
    SetBrush(Sel),
}

struct Drag {
    sel: Sel,
    height: f32,
}

/// What a left click or drag in the 3D view does.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tool {
    Select,
    Place,
    Scatter,
}

/// A scatter stroke in progress (one undo step).
struct Stroke {
    last: Vec3,
    taken: Vec<Vec3>,
    placed: Vec<Vec3>,
}

pub struct EditorApp {
    data: Result<DataRoot, String>,
    courses: Vec<CourseEntry>,
    filter: String,
    selected_course: Option<usize>,
    variants: Vec<Variant>,
    pub loaded: Option<LoadedCourse>,
    load_error: Option<String>,
    layers: Layers,
    cam: OrbitCamera,
    viewport: Option<Viewport>,
    tex_id: Option<egui::TextureId>,
    geo_gpu: Option<GpuScene>,
    ov_gpu: Option<GpuScene>,
    pub ground: Ground,
    geo_dirty: bool,
    ov_dirty: bool,
    scene_tris: usize,
    fog_dist: f32,
    pub sel: Option<Sel>,
    pub history: History,
    pub advanced: bool,
    lock_scenery: bool,
    drag: Option<Drag>,
    status: String,
    issues: Vec<Issue>,
    issues_version: Option<u64>,
    pack_name: String,
    export_dir: String,
    export_msg: Option<(bool, String)>,
    pending_switch: Option<(Option<usize>, u32)>,
    pub expand_all: bool,
    /// Rail points on each side that follow a dragged rail point.
    pub rail_soft: usize,
    /// Grid and angle snapping for drags, the gizmo and the Facing slider.
    pub snap: gizmo::Snap,
    gizmo_drag: Option<gizmo::GizmoDrag>,
    /// Shared search over the Add a Pokémon palette, the Pokémon list and the scenery list.
    search: String,
    pub tool: Tool,
    pub brush: place::Brush,
    /// The object Place and Scatter copy.
    pub brush_src: Option<Sel>,
    rng: place::Rng,
    rng_seed: u64,
    stroke: Option<Stroke>,
}

fn col32(c: [f32; 3]) -> egui::Color32 {
    egui::Color32::from_rgb((c[0] * 255.0) as u8, (c[1] * 255.0) as u8, (c[2] * 255.0) as u8)
}

impl EditorApp {
    pub fn new(data: Result<DataRoot, String>) -> Self {
        let courses = data.as_ref().map(|d| d.courses()).unwrap_or_default();
        let export_dir = std::env::current_dir().map(|d| d.join("mods_out").display().to_string()).unwrap_or("mods_out".into());
        Self {
            data,
            courses,
            filter: String::new(),
            selected_course: None,
            variants: vec![],
            loaded: None,
            load_error: None,
            layers: Layers::default(),
            cam: OrbitCamera { target: Vec3::ZERO, yaw: 0.6, pitch: 0.8, dist: 100.0 },
            viewport: None,
            tex_id: None,
            geo_gpu: None,
            ov_gpu: None,
            ground: Ground::default(),
            geo_dirty: true,
            ov_dirty: true,
            scene_tris: 0,
            fog_dist: 400.0,
            sel: None,
            history: History::default(),
            advanced: false,
            lock_scenery: true,
            drag: None,
            status: String::new(),
            issues: vec![],
            issues_version: None,
            pack_name: String::new(),
            export_dir,
            export_msg: None,
            pending_switch: None,
            expand_all: false,
            rail_soft: edit::RAIL_SOFT,
            snap: gizmo::Snap::default(),
            gizmo_drag: None,
            search: String::new(),
            tool: Tool::Select,
            brush: place::Brush::default(),
            brush_src: None,
            rng: place::Rng::new(place::Brush::default().seed),
            rng_seed: place::Brush::default().seed,
            stroke: None,
        }
    }

    pub fn open(&mut self, course: &str, variant: u32) -> Result<(), String> {
        let idx = self
            .courses
            .iter()
            .position(|c| c.name.eq_ignore_ascii_case(course))
            .ok_or_else(|| format!("course pack {course} not found"))?;
        self.select_course(idx);
        self.load_variant(variant);
        match &self.load_error {
            Some(e) => Err(e.clone()),
            None => Ok(()),
        }
    }

    fn select_course(&mut self, idx: usize) {
        self.selected_course = Some(idx);
        self.variants = match &self.data {
            Ok(d) => d.variants(&self.courses[idx]).unwrap_or_default(),
            Err(_) => vec![],
        };
    }

    fn load_variant(&mut self, no: u32) {
        let Some(idx) = self.selected_course else { return };
        let course = self.courses[idx].clone();
        let Ok(data) = &mut self.data else { return };
        match data.load(&course, no) {
            Ok(c) => {
                self.load_error = None;
                self.frame_course(&c);
                self.pack_name = format!("{}-leg{}-edit", c.course, c.variant);
                self.loaded = Some(c);
                self.sel = None;
                self.history = History::default();
                self.issues_version = None;
                self.export_msg = None;
                self.geo_dirty = true;
                self.ov_dirty = true;
                self.status = "Click a Pokémon marker to select it, then drag it to move it.".into();
            }
            Err(e) => self.load_error = Some(e),
        }
    }

    fn unsaved(&self) -> usize {
        self.loaded.as_ref().map(|c| c.levels.iter().filter(|l| l.dirty).count()).unwrap_or(0)
    }

    fn frame_course(&mut self, c: &LoadedCourse) {
        // Frame the rail plus spawns (the playable area); geometry includes huge backdrops.
        let mut pts: Vec<Vec3> = vec![];
        if let Some(r) = c.rail() {
            pts.extend(r.segments.iter().flatten().map(|p| Vec3::from(*p)));
        }
        for l in &c.levels {
            pts.extend(l.spawns.iter().map(|s| Vec3::from(s.pos)));
        }
        if pts.is_empty() {
            pts.push(Vec3::ZERO);
        }
        let min = pts.iter().fold(Vec3::splat(f32::MAX), |a, b| a.min(*b));
        let max = pts.iter().fold(Vec3::splat(f32::MIN), |a, b| a.max(*b));
        self.cam = OrbitCamera::frame(min, max);
        self.fog_dist = (max - min).length() * 3.0;
    }

    fn label(&self, s: Sel) -> String {
        let Some(c) = &self.loaded else { return String::new() };
        match s {
            Sel::Spawn(l, i) => c.levels.get(l).and_then(|l| l.spawns.get(i)).map(|s| s.label.clone()).unwrap_or_default(),
            Sel::Model(m) => c.models.get(m).map(|m| m.inst.name.clone()).unwrap_or_default(),
            Sel::Rail(_, 0, i) => format!("Rail point {i}"),
            Sel::Rail(_, s, i) => format!("Rail point {i} (part {s})"),
        }
    }

    /// What the gizmo acts on: the selection, its position, its heading and whether it has one (rail points do not).
    /// None when nothing movable is selected.
    fn gizmo_target(&self) -> Option<(Sel, Vec3, f32, bool)> {
        let c = self.loaded.as_ref()?;
        let s = self.sel?;
        edit::move_mode(c, s).ok()?;
        let p = edit::position(c, s)?;
        let rot = edit::rot_of(c, s);
        Some((s, p, rot.map(edit::yaw_of).unwrap_or(0.0), rot.is_some()))
    }

    fn skip_model(s: Sel) -> Option<usize> {
        match s {
            Sel::Model(m) => Some(m),
            _ => None,
        }
    }

    /// Applies writes from an edit helper, reporting failures in the status line.
    fn apply(&mut self, label: String, key: Option<&str>, w: Result<Writes, String>) {
        let Some(c) = &mut self.loaded else { return };
        match w {
            Ok(w) => {
                self.history.apply(c, &label, key, w);
                self.ov_dirty = true;
                self.status = label;
            }
            Err(e) => self.status = e,
        }
    }

    pub fn move_sel_to(&mut self, s: Sel, target: Vec3, key: Option<&str>) {
        let Some(c) = &self.loaded else { return };
        let Some(cur) = edit::position(c, s) else { return };
        let d = target - cur;
        if d.length_squared() < 1e-10 {
            return;
        }
        let w = match s {
            Sel::Rail(l, a, b) => edit::move_rail(c, l, a, b, d, self.rail_soft),
            _ => edit::move_by(c, s, d),
        };
        let label = if matches!(s, Sel::Rail(..)) { "Bend the rail".to_string() } else { format!("Move {}", self.label(s)) };
        if matches!(s, Sel::Model(_)) {
            self.geo_dirty = true;
        }
        self.apply(label, key, w);
    }

    fn run(&mut self, a: Action) {
        match a {
            Action::Select(s) => {
                self.sel = s;
                self.ov_dirty = true;
                if let Some(s) = s {
                    let mode = self.loaded.as_ref().map(|c| edit::move_mode(c, s));
                    self.status = match mode {
                        Some(Ok(m)) => format!("{}: dragging it {m}", self.label(s)),
                        Some(Err(e)) => format!("{}: {e}", self.label(s)),
                        None => String::new(),
                    };
                }
            }
            Action::Focus(p) => {
                self.cam.target = p;
                self.cam.dist = 18.0;
            }
            Action::MoveTo(s, p) => self.move_sel_to(s, p, Some("move")),
            Action::SetYaw(s, y) => {
                let Some(c) = &self.loaded else { return };
                let w = edit::set_yaw(c, s, y).ok_or("rotation is not stored for this one".to_string());
                let label = format!("Turn {}", self.label(s));
                if matches!(s, Sel::Model(_)) {
                    self.geo_dirty = true;
                }
                self.apply(label, Some("yaw"), w);
            }
            Action::SetScale(m, v) => {
                let Some(c) = &self.loaded else { return };
                let w = edit::set_scale(c, m, v).ok_or("scale is not stored for this model".to_string());
                let label = format!("Resize {}", self.label(Sel::Model(m)));
                self.geo_dirty = true;
                self.apply(label, Some("scale"), w);
            }
            Action::DropToGround(Sel::Rail(..)) => self.status = "The rail rides above the ground; drag the point instead".into(),
            Action::DropToGround(s) => {
                let Some(c) = &self.loaded else { return };
                let Some(cur) = edit::position(c, s) else { return };
                match self.ground.below(cur, Self::skip_model(s)) {
                    Some(g) => {
                        self.move_sel_to(s, Vec3::new(cur.x, g.y, cur.z), None);
                        self.history.end_interaction();
                    }
                    None => self.status = "No ground below this point (is the scenery layer on?)".into(),
                }
            }
            Action::FaceRail(s) => {
                let Some(c) = &self.loaded else { return };
                let Some(cur) = edit::position(c, s) else { return };
                match edit::yaw_facing_rail(c, cur) {
                    Some(y) => {
                        self.run(Action::SetYaw(s, y));
                        self.history.end_interaction();
                    }
                    None => self.status = "No rail found".into(),
                }
            }
            Action::Copy(s, at) => {
                let Some(c) = &self.loaded else { return };
                let Some(cur) = edit::position(c, s) else { return };
                let at = at.unwrap_or_else(|| {
                    // beside the original, at the same height above the ground
                    let side = cur + Vec3::new(3.0, 0.0, 3.0);
                    match (self.ground.below(cur, Self::skip_model(s)), self.ground.below(side, Self::skip_model(s))) {
                        (Some(g0), Some(g1)) if matches!(s, Sel::Spawn(..)) => Vec3::new(side.x, g1.y + (cur.y - g0.y), side.z),
                        _ => side,
                    }
                });
                let label = format!("Copy {}", self.label(s));
                let Some(c) = &mut self.loaded else { return };
                match edit::add_copy(c, s, Some(at)) {
                    Ok((ns, w)) => {
                        self.apply(label, None, Ok(w));
                        self.history.end_interaction();
                        self.sel = Some(ns);
                        self.geo_dirty = true;
                        self.ov_dirty = true;
                        self.status = format!("{}: drag it where you want it", self.status);
                    }
                    Err(e) => self.status = e,
                }
            }
            Action::Remove(s) => {
                let Some(c) = &self.loaded else { return };
                let w = edit::remove(c, s);
                let label = format!("Remove {}", self.label(s));
                if w.is_ok() {
                    self.sel = None;
                }
                self.geo_dirty = true;
                self.apply(label, None, w);
                self.history.end_interaction();
            }
            Action::ResetRail => {
                let Some(c) = &self.loaded else { return };
                let w = edit::reset_rail(c);
                self.apply("Reset the rail".into(), None, w);
                self.history.end_interaction();
            }
            Action::Undo => {
                if let Some(c) = &mut self.loaded {
                    self.status = match self.history.undo(c) {
                        Some(l) => format!("Undid: {l}"),
                        None => "Nothing to undo".into(),
                    };
                    self.ov_dirty = true;
                    self.geo_dirty = true;
                }
            }
            Action::Redo => {
                if let Some(c) = &mut self.loaded {
                    self.status = match self.history.redo(c) {
                        Some(l) => format!("Redid: {l}"),
                        None => "Nothing to redo".into(),
                    };
                    self.ov_dirty = true;
                    self.geo_dirty = true;
                }
            }
            Action::Export(root) => {
                let Some(c) = &self.loaded else { return };
                let desc = format!("Edited {} leg {} made with the Snap level editor ({} changes)", c.course, c.variant, self.history.steps());
                self.export_msg = Some(match edit::export(c, &root, &self.pack_name, &desc, false) {
                    Ok(files) => (
                        true,
                        format!(
                            "Exported {} file(s) to {}. The pack is switched on in mods.json and loads at the next game start; set \"enabled\": false there (or delete the pack folder) to go back to the original course.",
                            files.len(),
                            root.join(edit::sanitize_pack(&self.pack_name)).display()
                        ),
                    ),
                    Err(e) => (false, e),
                });
            }
            Action::OpenCourse(i) => {
                if self.unsaved() > 0 {
                    self.pending_switch = Some((Some(i), 0));
                } else {
                    self.select_course(i);
                    let first = self.variants.first().map(|v| v.no).unwrap_or(1);
                    self.load_variant(first);
                }
            }
            Action::LoadVariant(no) => {
                if self.unsaved() > 0 {
                    self.pending_switch = Some((None, no));
                } else {
                    self.load_variant(no);
                }
            }
            Action::Rebuild => {
                self.geo_dirty = true;
                self.ov_dirty = true;
            }
            Action::SetBrush(s) => match self.loaded.as_ref().map(|c| Self::can_copy(c, s)) {
                Some(Ok(())) => {
                    self.brush_src = Some(s);
                    if self.tool == Tool::Select {
                        self.tool = Tool::Place;
                    }
                    self.status = format!("Brush: {}. Click the ground to place one, or switch to Scatter and drag.", self.label(s));
                }
                Some(Err(e)) => self.status = e,
                None => {}
            },
        }
    }

    /// Whether `s` can be copied (and so used as the Place and Scatter brush).
    fn can_copy(c: &LoadedCourse, s: Sel) -> Result<(), String> {
        match s {
            Sel::Spawn(l, i) => {
                let sp = &c.levels[l].spawns[i];
                if sp.pos_from_path || sp.pos_off.is_none() || sp.rot_off.is_none() {
                    Err("Only Pokémon with their own spawn point can be copied (this one follows a route)".into())
                } else {
                    Ok(())
                }
            }
            Sel::Model(_) => Ok(()),
            Sel::Rail(..) => Err("Rail points cannot be copied".into()),
        }
    }

    /// One source spawn per species that has its own spawn point (route-following ones cannot be copied yet).
    fn species_sources(c: &LoadedCourse) -> std::collections::BTreeMap<String, Sel> {
        let mut seen = std::collections::BTreeMap::new();
        for (li, l) in c.levels.iter().enumerate() {
            for (si, sp) in l.spawns.iter().enumerate() {
                // CH_000 and CH_001 entries are flowers, branches and other props
                let pokemon = sp.label.starts_with("CH_") && !sp.label.starts_with("CH_000") && !sp.label.starts_with("CH_001");
                if sp.alive && sp.alive_off.is_none() && !sp.pos_from_path && sp.pos_off.is_some() && sp.pos != [0.0; 3] && pokemon {
                    seen.entry(sp.label.clone()).or_insert(Sel::Spawn(li, si));
                }
            }
        }
        seen
    }

    /// Builds the picking ground when no frame has drawn yet (command-line runs).
    pub fn ensure_ground(&mut self) {
        if self.ground.models.is_empty() {
            if let Some(c) = &self.loaded {
                self.ground = render::build_geometry(c, self.layers).1;
            }
        }
    }

    /// Height the brush source keeps above the ground, so a flying Pokémon stays in the air. 0 for scenery.
    fn src_height(&self, s: Sel) -> f32 {
        if !matches!(s, Sel::Spawn(..)) {
            return 0.0;
        }
        let Some(p) = self.loaded.as_ref().and_then(|c| edit::position(c, s)) else { return 0.0 };
        self.ground.below(p, None).map(|g| (p.y - g.y).max(0.0)).unwrap_or(0.0)
    }

    /// Puts one copy of the brush source at `spot`. Copies with the same `key` merge into one undo step.
    fn place_spot(&mut self, spot: place::Spot, verb: &str, key: Option<&str>) -> Result<Sel, String> {
        let s = self.brush_src.ok_or("Pick what to place first: Brush, at the top of the right panel")?;
        let c = self.loaded.as_ref().ok_or("no course loaded")?;
        let is_model = matches!(s, Sel::Model(_));
        let rot = edit::rot_of(c, s).map(|r| place::orient(r, spot.yaw, spot.normal, self.brush.align && is_model));
        let at = spot.pos + Vec3::Y * self.src_height(s);
        let label = format!("{verb} {}", self.label(s));
        let c = self.loaded.as_mut().ok_or("no course loaded")?;
        let (ns, w) = edit::place_copy(c, s, at, rot, if is_model { spot.scale_mul } else { 1.0 })?;
        self.apply(label, key, Ok(w));
        Ok(ns)
    }

    fn sync_rng(&mut self) {
        if self.rng_seed != self.brush.seed {
            self.rng = place::Rng::new(self.brush.seed);
            self.rng_seed = self.brush.seed;
        }
    }

    /// Place tool: one copy where the surface was hit; it becomes the selection so the gizmo can fine-tune it.
    fn place_one(&mut self, hit: pick::Hit) {
        self.sync_rng();
        let spot = place::single(&self.brush, &mut self.rng, hit.point, hit.normal);
        match self.place_spot(spot, "Place", None) {
            Ok(ns) => {
                self.history.end_interaction();
                self.sel = Some(ns);
                if matches!(ns, Sel::Model(_)) {
                    self.geo_dirty = true;
                }
                self.ov_dirty = true;
            }
            Err(e) => self.status = e,
        }
    }

    fn stroke_begin(&mut self, at: Vec3) {
        self.sync_rng();
        self.stroke = Some(Stroke { last: at, taken: vec![], placed: vec![] });
        self.stroke_dab(at);
    }

    /// Scatter tool: drops a dab of copies around `center`. The ground is rebuilt only when the stroke ends, so copies
    /// from this stroke never stack on each other.
    fn stroke_dab(&mut self, center: Vec3) {
        let Some(st) = self.stroke.as_mut() else { return };
        let mut taken = std::mem::take(&mut st.taken);
        let spots = place::dab(&self.ground, &self.brush, &mut self.rng, center, &mut taken);
        let mut placed = vec![];
        for sp in spots {
            match self.place_spot(sp, "Scatter", Some("scatter")) {
                Ok(_) => placed.push(sp.pos),
                Err(e) => {
                    self.status = e;
                    break;
                }
            }
        }
        if let Some(st) = self.stroke.as_mut() {
            st.taken = taken;
            st.last = center;
            st.placed.extend(placed);
        }
    }

    fn stroke_end(&mut self) {
        let Some(st) = self.stroke.take() else { return };
        self.history.end_interaction();
        if let Some(s) = self.brush_src {
            if matches!(s, Sel::Model(_)) && !st.placed.is_empty() {
                self.geo_dirty = true;
            }
            self.status = format!("Scattered {} copies of {} (one undo step)", st.placed.len(), self.label(s));
        }
        self.ov_dirty = true;
    }

    /// Command-line scatter: one dab of `brush` around (x, z), as a drag in the Scatter tool would make.
    pub fn scatter_at(&mut self, src: Sel, x: f32, z: f32) -> Result<usize, String> {
        self.ensure_ground();
        let c = self.loaded.as_ref().ok_or("no course loaded")?;
        Self::can_copy(c, src)?;
        self.brush_src = Some(src);
        let (p, _) = place::surface_below(&self.ground, Vec3::new(x, 0.0, z), 1000.0).ok_or("no ground at that spot")?;
        self.stroke_begin(p);
        let n = self.stroke.as_ref().map(|s| s.placed.len()).unwrap_or(0);
        self.stroke_end();
        Ok(n)
    }

    pub fn select(&mut self, s: Option<Sel>, focus: bool) {
        self.run(Action::Select(s));
        if let (true, Some(p)) = (focus, s.and_then(|s| self.loaded.as_ref().and_then(|c| edit::position(c, s)))) {
            self.run(Action::Focus(p));
            self.cam.dist = 30.0;
        }
    }

    /// Command-line move (same path as a drag in the UI).
    pub fn move_by(&mut self, s: Sel, d: Vec3) -> Result<(), String> {
        let c = self.loaded.as_ref().ok_or("no course loaded")?;
        let w = edit::move_by(c, s, d)?;
        let label = format!("Move {}", self.label(s));
        self.apply(label, None, Ok(w));
        Ok(())
    }

    pub fn copy_to(&mut self, s: Sel, at: Vec3) -> Result<Sel, String> {
        self.run(Action::Copy(s, Some(at)));
        match self.sel {
            Some(ns) if ns != s => Ok(ns),
            _ => Err(self.status.clone()),
        }
    }

    /// Command-line rail bend (same path as dragging rail point `i` of the first rail part).
    pub fn bend_rail(&mut self, i: usize, d: Vec3, soft: usize) -> Result<String, String> {
        let c = self.loaded.as_ref().ok_or("no course loaded")?;
        let (l, _) = edit::rail_ref(c).ok_or("this course has no rail")?;
        let w = edit::move_rail(c, l, 0, i, d, soft)?;
        let n = w.len() / 3;
        self.apply("Bend the rail".into(), None, Ok(w));
        self.history.end_interaction();
        self.layers.rail_points = true;
        self.select(Some(Sel::Rail(l, 0, i)), true);
        let c = self.loaded.as_ref().unwrap();
        let st = edit::rail_stats(c).ok_or("no rail")?;
        let issues: Vec<String> = edit::checks(c, &self.ground).into_iter().filter(|i| matches!(i.sel, Sel::Rail(..))).map(|i| i.text).collect();
        Ok(format!(
            "{n} rail points moved; ride length {:.2} (original {:.2}, {:+.1}%, about {:+.1} s); rail checks: {}",
            st.length,
            st.orig_length,
            (st.length / st.orig_length - 1.0) * 100.0,
            st.seconds_change(),
            if issues.is_empty() { "none".into() } else { issues.join("; ") }
        ))
    }

    pub fn move_model_by(&mut self, m: usize, d: Vec3) -> Result<(), String> {
        let c = self.loaded.as_ref().ok_or("no course loaded")?;
        let w = edit::move_by(c, Sel::Model(m), d)?;
        let label = format!("Move {}", self.label(Sel::Model(m)));
        self.geo_dirty = true;
        self.apply(label, None, Ok(w));
        Ok(())
    }

    pub fn export_to(&mut self, root: PathBuf, pack: &str) -> Result<String, String> {
        self.pack_name = pack.to_string();
        self.run(Action::Export(root));
        match &self.export_msg {
            Some((true, m)) => Ok(m.clone()),
            Some((false, e)) => Err(e.clone()),
            None => Err("export did not run".into()),
        }
    }

    pub fn ui(&mut self, ui: &mut egui::Ui, gpu: &mut Gpu) {
        let mut acts = vec![];
        self.shortcuts(ui, &mut acts);
        egui::Panel::top("top").show(ui, |ui| self.top_bar(ui, &mut acts));
        egui::Panel::left("courses").default_size(180.0).show(ui, |ui| self.course_list(ui, &mut acts));
        egui::Panel::right("parts").default_size(400.0).show(ui, |ui| {
            if self.loaded.is_some() {
                ui.horizontal(|ui| {
                    ui.add(egui::TextEdit::singleline(&mut self.search).hint_text("Search Pokémon and scenery").desired_width(260.0)).on_hover_text(
                        "Shortens the Add a Pokémon buttons and the Pokémon and scenery lists to names containing this text, e.g. CH_02 or Tree",
                    );
                    if !self.search.is_empty() && ui.small_button("x").on_hover_text("Clear the search").clicked() {
                        self.search.clear();
                    }
                });
                ui.separator();
            }
            egui::ScrollArea::vertical().show(ui, |ui| {
                if self.tool != Tool::Select && self.loaded.is_some() {
                    self.placing_ui(ui, &mut acts);
                    ui.separator();
                }
                self.inspector(ui, &mut acts);
                ui.separator();
                self.checks_ui(ui, &mut acts);
                self.export_ui(ui, &mut acts);
                ui.separator();
                self.parts(ui, &mut acts);
            });
        });
        egui::CentralPanel::default().show(ui, |ui| self.viewport_ui(ui, gpu, &mut acts));
        self.switch_dialog(ui);
        for a in acts {
            self.run(a);
        }
        if ui.input(|i| i.pointer.any_released()) {
            self.history.end_interaction();
        }
    }

    fn shortcuts(&mut self, ui: &mut egui::Ui, acts: &mut Vec<Action>) {
        if ui.ctx().memory(|m| m.focused().is_some()) {
            return;
        }
        use egui::{Key, Modifiers};
        let sel = self.sel;
        let pos = sel.and_then(|s| self.loaded.as_ref().and_then(|c| edit::position(c, s)));
        let tool = &mut self.tool;
        ui.input_mut(|i| {
            if i.consume_key(Modifiers::COMMAND | Modifiers::SHIFT, Key::Z) || i.consume_key(Modifiers::COMMAND, Key::Y) {
                acts.push(Action::Redo);
            } else if i.consume_key(Modifiers::COMMAND, Key::Z) {
                acts.push(Action::Undo);
            }
            if i.consume_key(Modifiers::NONE, Key::Escape) {
                acts.push(Action::Select(None));
            }
            for (k, t) in [(Key::S, Tool::Select), (Key::P, Tool::Place), (Key::B, Tool::Scatter)] {
                if i.consume_key(Modifiers::NONE, k) {
                    *tool = t;
                }
            }
            if let Some(s) = sel {
                if i.consume_key(Modifiers::NONE, Key::G) {
                    acts.push(Action::DropToGround(s));
                }
                if i.consume_key(Modifiers::NONE, Key::R) {
                    acts.push(Action::FaceRail(s));
                }
                if i.consume_key(Modifiers::COMMAND, Key::D) {
                    acts.push(Action::Copy(s, None));
                }
                if i.consume_key(Modifiers::NONE, Key::Delete) {
                    acts.push(Action::Remove(s));
                }
                if i.consume_key(Modifiers::NONE, Key::F) {
                    if let Some(p) = pos {
                        acts.push(Action::Focus(p));
                    }
                }
            }
        });
    }

    fn top_bar(&mut self, ui: &mut egui::Ui, acts: &mut Vec<Action>) {
        ui.horizontal(|ui| {
            ui.heading("Snap level editor");
            ui.separator();
            ui.selectable_value(&mut self.advanced, false, "Beginner").on_hover_text("The essentials: select, move, turn, check, export");
            ui.selectable_value(&mut self.advanced, true, "Advanced").on_hover_text("Adds raw values, offsets, hashes and every part of the course");
            ui.separator();
            let u = ui.add_enabled(self.history.undo_label().is_some(), egui::Button::new("Undo"));
            if u.on_hover_text(self.history.undo_label().map(|l| format!("Undo {l} (Ctrl+Z)")).unwrap_or_default()).clicked() {
                acts.push(Action::Undo);
            }
            let r = ui.add_enabled(self.history.redo_label().is_some(), egui::Button::new("Redo"));
            if r.on_hover_text(self.history.redo_label().map(|l| format!("Redo {l} (Ctrl+Y)")).unwrap_or_default()).clicked() {
                acts.push(Action::Redo);
            }
            let n = self.unsaved();
            if n > 0 {
                ui.colored_label(egui::Color32::from_rgb(255, 200, 80), format!("{} edits in {n} file(s), not exported", self.history.steps()));
            }
            if self.advanced {
                ui.separator();
                match &self.data {
                    Ok(d) => ui.weak(format!("data: {}", d.root.display())),
                    Err(e) => ui.colored_label(egui::Color32::LIGHT_RED, e),
                };
            }
        });
        if let Some(idx) = self.selected_course {
            ui.horizontal(|ui| {
                ui.label(format!("Course {}", self.courses[idx].name));
                let cur = self.loaded.as_ref().map(|c| c.variant);
                for v in &self.variants {
                    let r = ui.selectable_label(cur == Some(v.no), format!("leg {}", v.no)).on_hover_text("Open this leg of the course (its own set of level files)");
                    if r.clicked() && cur != Some(v.no) {
                        acts.push(Action::LoadVariant(v.no));
                    }
                }
                if let Some(c) = &self.loaded {
                    ui.weak(format!("loaded in {} ms", c.load_ms));
                }
                if let Some(e) = &self.load_error {
                    ui.colored_label(egui::Color32::LIGHT_RED, e);
                }
            });
        } else if let Err(e) = &self.data {
            ui.colored_label(egui::Color32::LIGHT_RED, e);
        }
    }

    fn switch_dialog(&mut self, ui: &mut egui::Ui) {
        let Some((course, variant)) = self.pending_switch else { return };
        let mut choice = None;
        egui::Window::new("Unsaved edits").collapsible(false).resizable(false).show(ui.ctx(), |ui| {
            ui.label("This course has edits that are not exported. Switching course throws them away.");
            ui.horizontal(|ui| {
                if ui.button("Discard edits and switch").clicked() {
                    choice = Some(true);
                }
                if ui.button("Keep editing").clicked() {
                    choice = Some(false);
                }
            });
        });
        match choice {
            Some(true) => {
                self.pending_switch = None;
                match course {
                    Some(i) => {
                        self.select_course(i);
                        let first = self.variants.first().map(|v| v.no).unwrap_or(1);
                        self.load_variant(first);
                    }
                    None => self.load_variant(variant),
                }
            }
            Some(false) => self.pending_switch = None,
            None => {}
        }
    }

    fn course_list(&mut self, ui: &mut egui::Ui, acts: &mut Vec<Action>) {
        ui.label(format!("{} courses", self.courses.len()));
        ui.add(egui::TextEdit::singleline(&mut self.filter).hint_text("filter, e.g. LC_001"))
            .on_hover_text("Type part of a course name to shorten the list");
        let f = self.filter.to_lowercase();
        egui::ScrollArea::vertical().show(ui, |ui| {
            for (i, c) in self.courses.iter().enumerate() {
                if !f.is_empty() && !c.name.to_lowercase().contains(&f) {
                    continue;
                }
                let sel = self.selected_course == Some(i);
                let r = ui.selectable_label(sel, &c.name);
                if sel && self.expand_all {
                    r.scroll_to_me(Some(egui::Align::Center));
                }
                if r.clicked() && !sel {
                    acts.push(Action::OpenCourse(i));
                }
            }
        });
    }

    fn inspector(&mut self, ui: &mut egui::Ui, acts: &mut Vec<Action>) {
        let Some(c) = &self.loaded else {
            ui.heading("Start here");
            ui.label("Pick a course on the left. Each course opens as a copy you can change; the original game files are never edited.");
            return;
        };
        let Some(s) = self.sel else {
            ui.heading("Nothing selected");
            ui.label("Click a Pokémon marker in the 3D view (or a name in the list below) to select it.");
            ui.label("Drag a selected Pokémon to slide it along the ground. Ctrl+Z undoes, Ctrl+Y redoes.");
            ui.separator();
            ui.heading("Add a Pokémon");
            ui.label("Click one to put a copy on the ground in the middle of the view. Then drag it where you want it.");
            let seen = Self::species_sources(c);
            let painting = self.tool != Tool::Select;
            if painting {
                ui.weak("Place and Scatter are on: a click here picks the brush instead of adding one in the middle of the view.");
            }
            let target = self.ground.below(self.cam.target + Vec3::Y * 50.0, None).unwrap_or(self.cam.target);
            let f = self.search.to_lowercase();
            let shown: Vec<_> = seen.iter().filter(|(l, _)| f.is_empty() || l.to_lowercase().contains(&f)).collect();
            if shown.is_empty() {
                ui.weak("No species here match the search.");
            }
            ui.horizontal_wrapped(|ui| {
                for (label, src) in shown {
                    let b = egui::Button::new(egui::RichText::new(label).color(col32(render::species_color(label))));
                    let tip = if painting {
                        "Use this species as the Place and Scatter brush"
                    } else {
                        "Copies this species, with its behavior, from the course"
                    };
                    if ui.add(b).on_hover_text(tip).clicked() {
                        acts.push(if painting { Action::SetBrush(*src) } else { Action::Copy(*src, Some(target)) });
                    }
                }
            });
            ui.weak("Scenery: select a model and press Make a copy (Ctrl+D).");
            return;
        };
        let pos = edit::position(c, s).unwrap_or(Vec3::ZERO);
        match s {
            Sel::Spawn(li, si) => {
                let sp = &c.levels[li].spawns[si];
                ui.heading(egui::RichText::new(&sp.label).color(col32(render::species_color(&sp.label))));
                ui.label(if sp.label.starts_with("CH_0") && !sp.label.starts_with("CH_000") {
                    "Pokémon spawn"
                } else {
                    "Spawn (scenery object or Pokémon)"
                });
            }
            Sel::Model(m) => {
                ui.heading(&c.models[m].inst.name);
                ui.label("Scenery model");
            }
            Sel::Rail(..) => {
                ui.heading(self.label(s));
                ui.label("A point of the camera rail. Drag it to bend the ride; the points next to it follow so the ride stays smooth.");
                ui.add(egui::Slider::new(&mut self.rail_soft, 0..=8).text("points that follow on each side"))
                    .on_hover_text("0 moves only this point. Higher numbers bend a longer stretch of the rail so the ride stays smooth.");
                if let Some(st) = edit::rail_stats(c) {
                    ui.label(st.summary());
                }
            }
        }
        let is_rail = matches!(s, Sel::Rail(..));
        let mode = edit::move_mode(c, s);
        match &mode {
            Ok(m) => ui.weak(format!("Moving it {m}.")),
            Err(e) => ui.colored_label(egui::Color32::from_rgb(255, 200, 80), format!("Cannot move: {e}.")),
        };
        let movable = mode.is_ok();
        ui.add_enabled_ui(movable, |ui| {
            let mut p = pos;
            let mut changed = false;
            ui.horizontal(|ui| {
                ui.label("Position");
                for (k, n) in ["X", "Y", "Z"].iter().enumerate() {
                    ui.label(*n);
                    changed |= ui
                        .add(egui::DragValue::new(&mut p[k]).speed(0.05).fixed_decimals(2))
                        .on_hover_text(format!("{n} in world units. Drag sideways to change it, or double-click to type a number. Y is up."))
                        .changed();
                }
            });
            if changed {
                acts.push(Action::MoveTo(s, p));
            }
        });
        let rot = edit::rot_of(c, s);
        if let Some(rot) = rot {
            let mut deg = edit::yaw_of(rot).to_degrees();
            let mut sl = egui::Slider::new(&mut deg, -180.0..=180.0).text("Facing").suffix("°");
            if self.snap.angle > 0.0 {
                sl = sl.step_by(self.snap.angle as f64);
            }
            let tip = "Which way it faces, turning around the up axis. 0° looks along +Z. The yellow ring in the 3D view does the same.";
            if ui.add(sl).on_hover_text(tip).changed() {
                acts.push(Action::SetYaw(s, self.snap.yaw(deg.to_radians())));
            }
        } else if !is_rail {
            ui.weak("Facing: not stored for this one");
        }
        if let Sel::Model(m) = s {
            let pm = &c.models[m];
            if pm.inst.scale_off.is_some() {
                let mut sc = pm.inst.scale;
                if self.advanced {
                    let mut ch = false;
                    ui.horizontal(|ui| {
                        ui.label("Scale");
                        for v in sc.iter_mut() {
                            ch |= ui
                                .add(egui::DragValue::new(v).speed(0.01).range(0.01..=100.0))
                                .on_hover_text("Scale on one axis (X, Y, Z); 1 is the original size")
                                .changed();
                        }
                    });
                    if ch {
                        acts.push(Action::SetScale(m, Vec3::from(sc)));
                    }
                } else {
                    let mut u = sc[0];
                    if ui
                        .add(egui::Slider::new(&mut u, 0.1..=5.0).text("Size").logarithmic(true))
                        .on_hover_text("Grows or shrinks the model evenly; 1 is the original size")
                        .changed()
                    {
                        let f = if sc[0].abs() > 1e-6 { u / sc[0] } else { 1.0 };
                        acts.push(Action::SetScale(m, Vec3::from(sc) * f));
                    }
                }
            }
        }
        ui.horizontal(|ui| {
            if is_rail {
                if ui.button("Reset the rail").on_hover_text("Puts every rail point back where the game has it (one undo step)").clicked() {
                    acts.push(Action::ResetRail);
                }
            } else {
                if ui.add_enabled(movable, egui::Button::new("Drop to ground")).on_hover_text("Puts it on the ground straight below (G)").clicked() {
                    acts.push(Action::DropToGround(s));
                }
                if ui
                    .add_enabled(rot.is_some(), egui::Button::new("Face the rail"))
                    .on_hover_text("Turns it to face the nearest point of the camera rail, so the player sees its front (R)")
                    .clicked()
                {
                    acts.push(Action::FaceRail(s));
                }
            }
            if ui.button("Show me").on_hover_text("Moves the camera to look at it (F)").clicked() {
                acts.push(Action::Focus(pos));
            }
            if ui.button("Deselect").on_hover_text("Clears the selection (Esc)").clicked() {
                acts.push(Action::Select(None));
            }
        });
        if !is_rail { ui.horizontal(|ui| {
            if ui.add_enabled(movable, egui::Button::new("Make a copy")).on_hover_text("A copy with the same behavior appears beside it (Ctrl+D)").clicked() {
                acts.push(Action::Copy(s, None));
            }
            let brushable = Self::can_copy(c, s);
            if ui
                .add_enabled(brushable.is_ok(), egui::Button::new("Use as brush"))
                .on_hover_text("Place and Scatter put down copies of this one")
                .on_disabled_hover_text(brushable.err().unwrap_or_default())
                .clicked()
            {
                acts.push(Action::SetBrush(s));
            }
            let added = edit::is_added(c, s);
            if ui
                .add_enabled(added, egui::Button::new("Remove"))
                .on_hover_text("Removes this added copy from the course (Delete)")
                .on_disabled_hover_text("Only things you added can be removed for now")
                .clicked()
            {
                acts.push(Action::Remove(s));
            }
        }); }
        if self.advanced {
            egui::CollapsingHeader::new("Raw values").default_open(true).show(ui, |ui| match s {
                Sel::Spawn(li, si) => {
                    let l = &c.levels[li];
                    let sp = &l.spawns[si];
                    let off = |o: Option<usize>| o.map(|o| format!("0x{o:x}")).unwrap_or("absent".into());
                    ui.monospace(format!("level      {}", l.name));
                    ui.monospace(format!("entity     {:08x}", sp.entity));
                    ui.monospace(format!("templates  {}", sp.templates.iter().map(|h| format!("{h:08x}")).collect::<Vec<_>>().join(" ")));
                    ui.monospace(format!("path       {}", sp.path.map(|p| format!("{p:08x}")).unwrap_or("none".into())));
                    ui.monospace(format!("pos        {:?}{}", sp.pos, if sp.pos_from_path { " (path start)" } else { "" }));
                    ui.monospace(format!("rot xyzw   {:?}", sp.rot));
                    ui.monospace(format!("pos offset {}", off(sp.pos_off)));
                    ui.monospace(format!("rot offset {}", off(sp.rot_off)));
                    for t in &sp.templates {
                        if let Some(tp) = l.templates.get(t) {
                            ui.monospace(format!("template   {} {} {}", tp.ch_name, tp.skeleton, tp.anim));
                        }
                    }
                }
                Sel::Model(m) => {
                    let pm = &c.models[m];
                    let off = |o: Option<usize>| o.map(|o| format!("0x{o:x}")).unwrap_or("absent".into());
                    ui.monospace(format!("level      {}", pm.level));
                    ui.monospace(format!("file       {}", pm.source.as_ref().map(|p| p.display().to_string()).unwrap_or("not found".into())));
                    ui.monospace(format!("pos        {:?}", pm.inst.pos));
                    ui.monospace(format!("rot xyzw   {:?}", pm.inst.rot));
                    ui.monospace(format!("scale      {:?}", pm.inst.scale));
                    ui.monospace(format!("offsets    pos {} rot {} scale {}", off(pm.inst.pos_off), off(pm.inst.rot_off), off(pm.inst.scale_off)));
                }
                Sel::Rail(li, si, i) => {
                    if let Some((_, pi)) = edit::rail_ref(c) {
                        let p = &c.levels[li].paths[pi];
                        ui.monospace(format!("level      {}", c.levels[li].name));
                        ui.monospace(format!("path       {:08x} segment {si} point {i} of {}", p.entity, p.segments[si].len()));
                        ui.monospace(format!("pos        {:?}", p.segments[si][i]));
                        ui.monospace(format!("offset     0x{:x}", p.offs[si][i]));
                        if let Some(o) = edit::rail_original(c) {
                            ui.monospace(format!("original   {:?}", o[si][i]));
                        }
                    }
                }
            });
        }
    }

    /// Brush settings for the Place and Scatter tools.
    fn placing_ui(&mut self, ui: &mut egui::Ui, acts: &mut Vec<Action>) {
        let Some(c) = &self.loaded else { return };
        let scatter = self.tool == Tool::Scatter;
        ui.heading(if scatter { "Scatter" } else { "Place" });
        ui.label(if scatter {
            "Drag over the ground to paint copies of the brush. Each stroke is one undo step. Alt+drag turns the camera."
        } else {
            "Click the ground or any surface to put down a copy of the brush. Drag turns the camera."
        });
        let cur = self.brush_src.map(|s| self.label(s)).unwrap_or_else(|| "nothing yet".into());
        ui.horizontal(|ui| {
            ui.label("Brush");
            egui::ComboBox::from_id_salt("brush_src")
                .selected_text(cur)
                .width(160.0)
                .show_ui(ui, |ui| {
                    for (label, src) in Self::species_sources(c) {
                        if ui.selectable_label(self.brush_src == Some(src), egui::RichText::new(&label).color(col32(render::species_color(&label)))).clicked() {
                            acts.push(Action::SetBrush(src));
                        }
                    }
                })
                .response
                .on_hover_text("The Pokémon species to put down. For scenery, select a model and press Use as brush.");
            if let Some(s) = self.sel {
                let ok = Self::can_copy(c, s);
                if ui
                    .add_enabled(ok.is_ok() && self.brush_src != Some(s), egui::Button::new("Use the selection"))
                    .on_hover_text("Makes the selected Pokémon or scenery model the brush")
                    .on_disabled_hover_text(ok.err().unwrap_or("It is already the brush".into()))
                    .clicked()
                {
                    acts.push(Action::SetBrush(s));
                }
            }
        });
        let model = matches!(self.brush_src, Some(Sel::Model(_)));
        let b = &mut self.brush;
        if scatter {
            ui.add(egui::Slider::new(&mut b.radius, 1.0..=40.0).text("radius").logarithmic(true))
                .on_hover_text("Size of the circle copies land in, in world units");
            ui.add(egui::Slider::new(&mut b.count, 1..=20).text("copies per dab"))
                .on_hover_text("How many copies drop each time the brush moves on by the spacing");
            ui.add(egui::Slider::new(&mut b.spacing, 0.5..=30.0).text("spacing").logarithmic(true))
                .on_hover_text("How far the mouse moves before the next dab, in world units");
            ui.add(egui::Slider::new(&mut b.min_gap, 0.0..=10.0).text("gap between copies"))
                .on_hover_text("Copies from one stroke keep at least this far apart, so they do not pile up");
        }
        ui.checkbox(&mut b.random_yaw, "Random heading").on_hover_text("Each copy faces a random way. Off keeps the brush's own heading.");
        ui.add_enabled_ui(model, |ui| {
            ui.horizontal(|ui| {
                ui.label("Random size");
                ui.add(egui::DragValue::new(&mut b.scale.0).speed(0.01).range(0.05..=20.0).prefix("from "))
                    .on_hover_text("Smallest size, as a multiple of the brush's size");
                ui.add(egui::DragValue::new(&mut b.scale.1).speed(0.01).range(0.05..=20.0).prefix("to "))
                    .on_hover_text("Largest size; set both to 1 to keep the brush's size");
            })
            .response
            .on_disabled_hover_text("Pokémon spawns store no size; this applies to scenery models");
            ui.checkbox(&mut b.align, "Tilt to the slope")
                .on_hover_text("Scenery leans with the ground under it")
                .on_disabled_hover_text("Pokémon always stand upright; this applies to scenery models");
        });
        ui.horizontal(|ui| {
            ui.label("Seed");
            ui.add(egui::DragValue::new(&mut b.seed)).on_hover_text("The same seed gives the same random layout. Change it for a different one.");
        });
    }

    fn checks_ui(&mut self, ui: &mut egui::Ui, acts: &mut Vec<Action>) {
        let Some(c) = &self.loaded else { return };
        if self.issues_version != Some(self.history.version) && !self.ground.models.is_empty() {
            self.issues = edit::checks(c, &self.ground);
            self.issues_version = Some(self.history.version);
        }
        let title = if self.issues.is_empty() { "Checks: all good".to_string() } else { format!("Checks: {} to look at", self.issues.len()) };
        let advanced = self.advanced;
        egui::CollapsingHeader::new(title).default_open(true).show(ui, |ui| {
            if self.issues.is_empty() {
                ui.weak("No Pokémon under the ground or far from the rail.");
            }
            for (n, is) in self.issues.iter().enumerate() {
                if n >= 12 && !advanced {
                    ui.weak(format!("...and {} more (Advanced shows all)", self.issues.len() - n));
                    break;
                }
                if ui.link(&is.text).clicked() {
                    acts.push(Action::Select(Some(is.sel)));
                    if let Some(p) = edit::position(c, is.sel) {
                        acts.push(Action::Focus(p));
                    }
                }
            }
        });
    }

    fn export_ui(&mut self, ui: &mut egui::Ui, acts: &mut Vec<Action>) {
        if self.loaded.is_none() {
            return;
        }
        let has = self.unsaved() > 0;
        egui::CollapsingHeader::new("Save as a mod pack").default_open(true).show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.label("Pack name");
                ui.text_edit_singleline(&mut self.pack_name)
                    .on_hover_text("The pack's name; its folder is named after it");
            });
            ui.horizontal(|ui| {
                ui.label("Folder");
                ui.text_edit_singleline(&mut self.export_dir).on_hover_text("Where Export to folder writes the pack");
            });
            ui.horizontal(|ui| {
                if ui
                    .add_enabled(has, egui::Button::new("Export to folder"))
                    .on_hover_text("Writes only the changed level files, as a mod pack, into the folder above")
                    .clicked()
                {
                    acts.push(Action::Export(PathBuf::from(&self.export_dir)));
                }
                if let Some(g) = edit::game_mods_dir() {
                    if ui
                        .add_enabled(has, egui::Button::new("Install into my game"))
                        .on_hover_text(format!("Writes the pack to {}; the game loads it at the next start", g.display()))
                        .clicked()
                    {
                        acts.push(Action::Export(g));
                    }
                }
            });
            if !has {
                ui.weak("Make a change first. Only changed files are written; the original game stays untouched.");
            }
            if let Some((ok, m)) = &self.export_msg {
                ui.colored_label(if *ok { egui::Color32::LIGHT_GREEN } else { egui::Color32::LIGHT_RED }, m);
            }
        });
    }

    fn parts(&mut self, ui: &mut egui::Ui, acts: &mut Vec<Action>) {
        let (open, advanced, cur_sel) = (self.expand_all, self.advanced, self.sel);
        let Some(c) = &mut self.loaded else { return };
        let total_spawns: usize = c.levels.iter().map(|l| l.spawns.len()).sum();
        let f = self.search.to_lowercase();
        let hit = |name: &str| f.is_empty() || name.to_lowercase().contains(&f);
        egui::CollapsingHeader::new(format!("Pokémon and objects ({total_spawns})")).default_open(open || !f.is_empty()).show(ui, |ui| {
            for (li, l) in c.levels.iter().enumerate() {
                for (si, s) in l.spawns.iter().enumerate() {
                    if !s.alive || !hit(&s.label) {
                        continue;
                    }
                    let unplaced = !s.pos_from_path && s.pos == [0.0; 3];
                    let txt = if s.alive_off.is_some() {
                        format!("{}  [added]", s.label)
                    } else if unplaced {
                        format!("{}  [placed by the game, not by this file]", s.label)
                    } else {
                        format!(
                            "{}  ({:.1}, {:.1}, {:.1}){}",
                            s.label,
                            s.pos[0],
                            s.pos[1],
                            s.pos[2],
                            if s.pos_from_path { "  [walks a route]" } else { "" }
                        )
                    };
                    let sel = cur_sel == Some(Sel::Spawn(li, si));
                    let rt = egui::RichText::new(txt);
                    let rt = if sel { rt.strong() } else { rt.color(col32(render::species_color(&s.label))) };
                    let r = ui.selectable_label(sel, rt);
                    if r.clicked() {
                        acts.push(Action::Select(Some(Sel::Spawn(li, si))));
                    }
                    if r.double_clicked() {
                        acts.push(Action::Focus(Vec3::from(s.pos)));
                    }
                }
            }
        });
        let missing = c.models.iter().filter(|m| m.mesh.is_none()).count();
        let title = format!("Scenery ({} models{})", c.models.len(), if missing > 0 { format!(", {missing} not found") } else { String::new() });
        egui::CollapsingHeader::new(title).default_open((open && advanced) || !f.is_empty()).show(ui, |ui| {
            ui.weak("Tick to show or hide. Click a name to select it.");
            for (mi, m) in c.models.iter_mut().enumerate() {
                if !m.inst.alive || !hit(&m.inst.name) {
                    continue;
                }
                ui.horizontal(|ui| {
                    if ui.checkbox(&mut m.visible, "").on_hover_text("Show or hide this model in the 3D view only; the export is not affected").changed() {
                        acts.push(Action::Rebuild);
                    }
                    let tris = m.mesh.as_ref().map(|x| x.indices.len() / 3).unwrap_or(0);
                    let added = if m.inst.alive_off.is_some() { "  [added]" } else { "" };
                    let t = if advanced { format!("{}{added}  {} tris", m.inst.name, tris) } else { format!("{}{added}", m.inst.name) };
                    if let Some(e) = &m.error {
                        ui.colored_label(egui::Color32::LIGHT_RED, format!("{t}: {e}"));
                    } else if ui.selectable_label(cur_sel == Some(Sel::Model(mi)), t).clicked() {
                        acts.push(Action::Select(Some(Sel::Model(mi))));
                    }
                });
            }
        });
        let rail_len: Vec<(usize, f32)> =
            c.rail().map(|r| (0..r.segments.len()).map(|i| (r.segments[i].len(), r.length(i))).collect()).unwrap_or_default();
        let stats = edit::rail_stats(c);
        let rail_start = edit::rail_ref(c).map(|(l, _)| Sel::Rail(l, 0, 0));
        let mut show_points = self.layers.rail_points;
        egui::CollapsingHeader::new("Camera rail").default_open(true).show(ui, |ui| {
            if rail_len.is_empty() {
                ui.label("No rail found.");
                return;
            }
            if let Some(st) = &stats {
                ui.label(st.summary()).on_hover_text(format!(
                    "The camera keeps its speed ({:.2} units a second, measured in game on Florio) and rides the extra length. Original length {:.1} units.",
                    edit::RAIL_SPEED,
                    st.orig_length
                ));
            }
            ui.checkbox(&mut show_points, "Show rail points so I can bend the ride")
                .on_hover_text("Click a rail point, then drag it. Keep the length close to the original so the ride takes the same time.");
            ui.horizontal(|ui| {
                if let Some(s) = rail_start {
                    if ui.button("Select the start").on_hover_text("Selects the rail's first point").clicked() {
                        acts.push(Action::Select(Some(s)));
                    }
                }
                if ui.button("Reset the rail").on_hover_text("Puts every rail point back where the game has it (one undo step)").clicked() {
                    acts.push(Action::ResetRail);
                }
            });
            if advanced {
                for (i, (pts, len)) in rail_len.iter().enumerate() {
                    ui.monospace(format!("part {i}: {pts} points, {len:.1} units"));
                }
            }
        });
        if show_points != self.layers.rail_points {
            self.layers.rail_points = show_points;
            self.ov_dirty = true;
        }
        if !advanced {
            return;
        }
        egui::CollapsingHeader::new(format!("Level files ({})", c.levels.len())).default_open(false).show(ui, |ui| {
            if let Some(v) = c.variants.iter().find(|v| v.no == c.variant) {
                for lr in &v.levels {
                    match &lr.name {
                        Some(n) => ui.label(format!("{:08x}  {n}", lr.hash)),
                        None => ui.weak(format!("{:08x}  no entry with that name in the pack", lr.hash)),
                    };
                }
            }
            for e in &c.level_errors {
                ui.colored_label(egui::Color32::LIGHT_RED, e);
            }
            for l in &c.levels {
                ui.collapsing(format!("{}  ({}){}", l.name, l.kind(), if l.dirty { "  edited" } else { "" }), |ui| {
                    ui.label(format!("{} bytes, {} entities, {} scene nodes, {} paths", l.size, l.entity_count, l.node_count, l.paths.len()));
                    for (k, n) in &l.component_kinds {
                        ui.monospace(format!("component {k}: {n}"));
                    }
                    if !l.species.is_empty() {
                        ui.label(format!("species used: {:?}", l.species));
                    }
                    if !l.env_params.is_empty() {
                        ui.label(format!("environment parameters: {}", l.env_params.join(", ")));
                    }
                });
            }
            ui.collapsing("Component kinds", |ui| {
                ui.weak("slot number of the component sub-table; names are working labels");
                for s in [1usize, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15] {
                    ui.monospace(format!("{s:>2}: {}", kind_name(s)));
                }
            });
        });
    }

    fn pick(&self, rect: egui::Rect, at: egui::Pos2) -> Option<Sel> {
        let c = self.loaded.as_ref()?;
        if self.layers.rail && self.layers.rail_points {
            if let Some((rl, pi)) = edit::rail_ref(c) {
                let mut best: Option<(f32, Sel)> = None;
                for (si, seg) in c.levels[rl].paths[pi].segments.iter().enumerate() {
                    for (i, p) in seg.iter().enumerate() {
                        let Some(sp) = pick::project(&self.cam, rect, Vec3::from(*p)) else { continue };
                        let d = sp.distance(at);
                        if d < 12.0 && best.map(|b| d < b.0).unwrap_or(true) {
                            best = Some((d, Sel::Rail(rl, si, i)));
                        }
                    }
                }
                if let Some((_, s)) = best {
                    return Some(s);
                }
            }
        }
        if self.layers.spawns {
            let mut best: Option<(f32, Sel)> = None;
            for (li, l) in c.levels.iter().enumerate() {
                for (si, s) in l.spawns.iter().enumerate() {
                    if !s.alive || (!s.pos_from_path && s.pos == [0.0; 3]) {
                        continue;
                    }
                    let Some(p) = pick::project(&self.cam, rect, Vec3::from(s.pos) + Vec3::Y * 0.6) else { continue };
                    let d = p.distance(at);
                    if d < 14.0 && best.map(|b| d < b.0).unwrap_or(true) {
                        best = Some((d, Sel::Spawn(li, si)));
                    }
                }
            }
            if let Some((_, s)) = best {
                return Some(s);
            }
        }
        if !self.lock_scenery {
            let (o, d) = pick::screen_ray(&self.cam, rect, at);
            return self.ground.raycast(o, d, 1e5, None).map(|h| Sel::Model(h.model));
        }
        None
    }

    fn viewport_ui(&mut self, ui: &mut egui::Ui, gpu: &mut Gpu, acts: &mut Vec<Action>) {
        ui.horizontal(|ui| {
            let t0 = self.tool;
            ui.selectable_value(&mut self.tool, Tool::Select, "Select").on_hover_text("Click to select, drag to move (S)");
            ui.selectable_value(&mut self.tool, Tool::Place, "Place").on_hover_text("Click a surface to put down a copy of the brush (P)");
            ui.selectable_value(&mut self.tool, Tool::Scatter, "Scatter")
                .on_hover_text("Drag over the ground to paint several copies with random heading and size (B)");
            if t0 != self.tool {
                self.stroke_end();
            }
            ui.separator();
            let before = self.layers;
            ui.checkbox(&mut self.layers.geometry, "scenery").on_hover_text("Show the course's ground and scenery models");
            ui.checkbox(&mut self.layers.rail, "rail").on_hover_text("Show the camera rail the player rides");
            ui.checkbox(&mut self.layers.rail_points, "rail points").on_hover_text("Show the rail's points; click one and drag it to bend the ride");
            ui.checkbox(&mut self.layers.spawns, "Pokémon").on_hover_text("Show a marker for every Pokémon and object spawn");
            ui.checkbox(&mut self.layers.spawn_links, "their routes").on_hover_text("Show the route each walking Pokémon follows");
            ui.checkbox(&mut self.layers.paths, "all paths").on_hover_text("Show every path in the level files, including ones no Pokémon uses");
            if before.geometry != self.layers.geometry {
                self.geo_dirty = true;
            }
            if before != self.layers {
                self.ov_dirty = true;
            }
            ui.separator();
            ui.checkbox(&mut self.lock_scenery, "lock scenery")
                .on_hover_text("When locked, clicks in the 3D view only pick Pokémon, so the ground cannot be moved by accident");
            if ui.button("Frame course").on_hover_text("Moves the camera so the whole course fits in view").clicked() {
                if let Some(c) = self.loaded.take() {
                    self.frame_course(&c);
                    self.loaded = Some(c);
                }
            }
            ui.separator();
            let fmt_grid = |g: f32| if g == 0.0 { "off".to_string() } else { format!("{g}") };
            egui::ComboBox::from_id_salt("snap_grid")
                .selected_text(format!("grid {}", fmt_grid(self.snap.grid)))
                .width(70.0)
                .show_ui(ui, |ui| {
                    for g in gizmo::GRID_STEPS {
                        ui.selectable_value(&mut self.snap.grid, g, fmt_grid(g));
                    }
                })
                .response
                .on_hover_text("Grid snapping: drags and the gizmo arrows move in steps of this many world units");
            let fmt_ang = |a: f32| if a == 0.0 { "off".to_string() } else { format!("{a}°") };
            egui::ComboBox::from_id_salt("snap_angle")
                .selected_text(format!("turn {}", fmt_ang(self.snap.angle)))
                .width(70.0)
                .show_ui(ui, |ui| {
                    for a in gizmo::ANGLE_STEPS {
                        ui.selectable_value(&mut self.snap.angle, a, fmt_ang(a));
                    }
                })
                .response
                .on_hover_text("Angle snapping: the yellow ring and the Facing slider turn in steps of this many degrees");
        });
        let avail = ui.available_size() - egui::vec2(0.0, 24.0);
        let ppp = ui.ctx().pixels_per_point();
        let (w, h) = ((avail.x * ppp).max(16.0) as u32, (avail.y * ppp).max(16.0) as u32);
        let vp = self.viewport.get_or_insert_with(|| Viewport::new(gpu.device));
        if vp.resize(gpu.device, w, h) {
            let view = vp.color_view.as_ref().unwrap();
            match self.tex_id {
                Some(id) => gpu.egui.update_egui_texture_from_wgpu_texture(gpu.device, view, wgpu::FilterMode::Linear, id),
                None => self.tex_id = Some(gpu.egui.register_native_texture(gpu.device, view, wgpu::FilterMode::Linear)),
            }
        }
        if let Some(c) = &self.loaded {
            if self.geo_dirty {
                let (s, g) = render::build_geometry(c, self.layers);
                self.scene_tris = s.triangles;
                self.geo_gpu = Some(GpuScene::upload(gpu.device, &s));
                self.ground = g;
                self.geo_dirty = false;
                self.ov_dirty = true;
                self.issues_version = None;
            }
            if self.ov_dirty {
                let s = render::build_overlay(c, self.layers, self.sel, &self.ground);
                self.ov_gpu = Some(GpuScene::upload(gpu.device, &s));
                self.ov_dirty = false;
            }
        }
        let scenes: Vec<&GpuScene> = self.geo_gpu.iter().chain(self.ov_gpu.iter()).collect();
        vp.render(gpu.device, gpu.queue, &scenes, &self.cam, self.fog_dist);
        let Some(id) = self.tex_id else { return };
        let resp = ui.add(egui::Image::new((id, avail)).sense(egui::Sense::click_and_drag()));
        let rect = resp.rect;

        let gz = self.gizmo_target();
        let painting = self.tool != Tool::Select && self.loaded.is_some() && !ui.input(|i| i.modifiers.alt);
        let surface_at = |me: &Self, at: egui::Pos2| {
            let (o, d) = pick::screen_ray(&me.cam, rect, at);
            me.ground.raycast(o, d, 1e5, None)
        };
        if painting && resp.drag_started_by(egui::PointerButton::Primary) {
            // Scatter paints; Place leaves the drag to turn the camera
            let origin = ui.input(|i| i.pointer.press_origin()).unwrap_or(rect.center());
            if self.tool == Tool::Scatter {
                if let Some(h) = surface_at(self, origin) {
                    self.stroke_begin(h.point);
                }
            }
        } else if resp.drag_started_by(egui::PointerButton::Primary) {
            let origin = ui.input(|i| i.pointer.press_origin()).unwrap_or(rect.center());
            self.gizmo_drag = gz.and_then(|(_, p, yaw, ring)| {
                gizmo::hit(&self.cam, rect, p, ring, origin).and_then(|h| gizmo::begin(&self.cam, rect, p, yaw, h, origin))
            });
            if self.gizmo_drag.is_some() {
                // the handle wins over whatever is behind it
            } else if let Some(s) = self.pick(rect, origin) {
                let movable = self.loaded.as_ref().map(|c| edit::move_mode(c, s).is_ok()).unwrap_or(false);
                acts.push(Action::Select(Some(s)));
                if movable {
                    let cur = self.loaded.as_ref().and_then(|c| edit::position(c, s)).unwrap_or(Vec3::ZERO);
                    let g = self.ground.below(cur, Self::skip_model(s)).map(|g| cur.y - g.y).unwrap_or(0.0);
                    self.drag = Some(Drag { sel: s, height: g.max(0.0) });
                }
            }
        }
        let d = resp.drag_delta();
        if resp.dragged_by(egui::PointerButton::Primary) && self.stroke.is_some() {
            if let Some(h) = resp.interact_pointer_pos().and_then(|at| surface_at(self, at)) {
                let last = self.stroke.as_ref().map(|s| s.last).unwrap_or(h.point);
                if h.point.distance(last) >= self.brush.spacing {
                    self.stroke_dab(h.point);
                }
            }
        } else if resp.dragged_by(egui::PointerButton::Primary) {
            if let (Some(g), Some((s, ..)), Some(at)) = (self.gizmo_drag, gz, resp.interact_pointer_pos()) {
                match g.update(&self.cam, rect, at, self.snap) {
                    Some(gizmo::DragResult::Move(p)) => self.move_sel_to(s, p, Some("gizmo")),
                    Some(gizmo::DragResult::Yaw(y)) => acts.push(Action::SetYaw(s, y)),
                    None => {}
                }
            } else {
                match self.drag.as_ref().map(|dr| (dr.sel, dr.height)) {
                    Some((s, h)) => {
                        if let Some(at) = resp.interact_pointer_pos() {
                            let (o, dir) = pick::screen_ray(&self.cam, rect, at);
                            if let Some(hit) = self.ground.raycast(o, dir, 1e5, Self::skip_model(s)) {
                                let mut p = hit.point;
                                if self.snap.grid > 0.0 {
                                    // snap X and Z, then find the ground again at the snapped spot
                                    let q = self.snap.xz(p);
                                    p = self.ground.below(q + Vec3::Y * 5.0, Self::skip_model(s)).unwrap_or(q);
                                }
                                self.move_sel_to(s, p + Vec3::Y * h, Some("drag"));
                            }
                        }
                    }
                    None => self.cam.orbit(d.x, d.y),
                }
            }
        } else if resp.dragged_by(egui::PointerButton::Secondary) || resp.dragged_by(egui::PointerButton::Middle) {
            self.cam.pan(d.x, d.y);
        }
        if resp.drag_stopped() {
            self.drag = None;
            self.gizmo_drag = None;
            self.stroke_end();
            self.history.end_interaction();
        }
        if resp.clicked() {
            let at = resp.interact_pointer_pos().unwrap_or(rect.center());
            if painting {
                match (self.tool, surface_at(self, at)) {
                    (Tool::Place, Some(h)) => self.place_one(h),
                    (Tool::Scatter, Some(h)) => {
                        self.stroke_begin(h.point);
                        self.stroke_end();
                    }
                    _ => self.status = "Nothing to place on there (is the scenery layer on?)".into(),
                }
            } else {
                acts.push(Action::Select(self.pick(rect, at)));
            }
        }
        if resp.double_clicked() {
            if let Some(p) = self.sel.and_then(|s| self.loaded.as_ref().and_then(|c| edit::position(c, s))) {
                acts.push(Action::Focus(p));
            }
        }
        if resp.hovered() {
            let scroll = ui.input(|i| i.smooth_scroll_delta.y);
            if scroll != 0.0 {
                self.cam.zoom(scroll / 50.0);
            }
        }
        let info = match &self.loaded {
            Some(c) => format!(
                "{} leg {}   {} triangles   rail {}   {} spawns",
                c.course,
                c.variant,
                self.scene_tris,
                edit::rail_stats(c).map(|s| format!("{:.1} units", s.length)).unwrap_or("none".into()),
                c.levels.iter().map(|l| l.spawns.len()).sum::<usize>()
            ),
            None => "no course loaded".into(),
        };
        ui.painter().text(rect.left_top() + egui::vec2(8.0, 8.0), egui::Align2::LEFT_TOP, info, egui::FontId::proportional(14.0), egui::Color32::WHITE);
        if let Some(s) = self.sel {
            if let Some(p) = self.loaded.as_ref().and_then(|c| edit::position(c, s)) {
                if let Some(sp) = pick::project(&self.cam, rect, p + Vec3::Y * 1.6) {
                    ui.painter().text(sp, egui::Align2::CENTER_BOTTOM, self.label(s), egui::FontId::proportional(15.0), egui::Color32::WHITE);
                }
            }
        }
        // brush cursor and this stroke's copies (scenery copies show as dots until the stroke ends)
        if painting {
            let painter = ui.painter().with_clip_rect(rect);
            let col = egui::Color32::from_rgb(120, 230, 255);
            if let Some(h) = resp.hover_pos().and_then(|at| surface_at(self, at)) {
                if self.tool == Tool::Scatter {
                    let pts: Vec<egui::Pos2> = (0..=40)
                        .filter_map(|i| {
                            let a = i as f32 / 40.0 * std::f32::consts::TAU;
                            pick::project(&self.cam, rect, h.point + Vec3::new(a.sin(), 0.0, a.cos()) * self.brush.radius)
                        })
                        .collect();
                    painter.add(egui::Shape::line(pts, egui::Stroke::new(2.0, col)));
                } else if let Some(c) = pick::project(&self.cam, rect, h.point) {
                    painter.circle_stroke(c, 7.0, egui::Stroke::new(2.0, col));
                    if let Some(t) = pick::project(&self.cam, rect, h.point + h.normal.normalize_or_zero() * 1.5) {
                        painter.line_segment([c, t], egui::Stroke::new(2.0, col));
                    }
                }
            }
            if let Some(st) = &self.stroke {
                for p in &st.placed {
                    if let Some(c) = pick::project(&self.cam, rect, *p) {
                        painter.circle_filled(c, 3.5, col);
                    }
                }
            }
            if self.brush_src.is_none() {
                painter.text(rect.center_top() + egui::vec2(0.0, 36.0), egui::Align2::CENTER_TOP, "Pick a brush first (right panel)", egui::FontId::proportional(16.0), col);
            }
        }
        // gizmo on top, from the position after this frame's edits
        if let (false, Some((_, p, _, ring))) = (painting, self.gizmo_target()) {
            let active = self.gizmo_drag.map(|g| g.handle).or_else(|| resp.hover_pos().and_then(|at| gizmo::hit(&self.cam, rect, p, ring, at)));
            gizmo::draw(&ui.painter().with_clip_rect(rect), &self.cam, rect, p, ring, active);
            if let (None, Some(h)) = (self.gizmo_drag, active) {
                let tip = match h {
                    gizmo::Handle::Axis(1) => "Drag to raise or lower",
                    gizmo::Handle::Axis(_) => "Drag to move along this axis only",
                    gizmo::Handle::Ring => "Drag around the ring to turn it",
                };
                if let Some(at) = resp.hover_pos() {
                    ui.painter().text(at + egui::vec2(14.0, 14.0), egui::Align2::LEFT_TOP, tip, egui::FontId::proportional(13.0), egui::Color32::WHITE);
                }
            }
        }
        ui.horizontal(|ui| {
            ui.label(&self.status);
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                ui.weak(match self.tool {
                    Tool::Select => "left drag: orbit / move selected   arrows: move on one axis   ring: turn   right drag: pan   wheel: zoom   G ground   R face rail   F show",
                    Tool::Place => "click: place a copy   left drag: orbit   right drag: pan   wheel: zoom   S select   B scatter",
                    Tool::Scatter => "left drag: paint copies   Alt+drag: orbit   right drag: pan   wheel: zoom   S select   P place",
                });
            });
        });
    }
}
