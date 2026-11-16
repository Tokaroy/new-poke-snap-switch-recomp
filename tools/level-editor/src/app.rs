//! Editor UI (egui). Two layers over one data model: the beginner view (select, drag along the ground, face the rail,
//! plain-language checks, one-button export) and the advanced view (raw offsets, hashes, quaternions, every model
//! instance, level parts). Every change goes through `edit::History`, so it can be undone and only the edited floats
//! change in the exported files.

use crate::course::{CourseEntry, DataRoot, LoadedCourse, Variant};
use crate::edit::{self, History, Issue, Sel, Writes};
use crate::gizmo;
use crate::ground;
use crate::place;
use crate::procgen;
use crate::reskin;
use crate::scenery;
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
    /// Rule-based fill around the rail with this many copies.
    Fill(usize),
    /// Import the model in the Import panel into the pack under this folder.
    Import(PathBuf),
    /// Write the Reskin panel's model over a species' meshes, into the pack under this folder.
    Reskin(PathBuf),
    /// Lay the Random course panel's course over this leg (running the generator first when asked).
    Random { run_generator: bool },
}

/// The Import a model panel: a user's model file becomes a new scenery model in a mod pack.
struct ImportForm {
    file: String,
    name: String,
    template: String,
    material: String,
    scale: f32,
    yaw: f32,
    height: f32,
    /// Template model groups and game materials of the open course's archive, read when the panel first opens.
    lists: Option<Result<(Vec<String>, Vec<String>), String>>,
    msg: Option<(bool, String)>,
}

impl Default for ImportForm {
    fn default() -> Self {
        Self { file: String::new(), name: String::new(), template: String::new(), material: String::new(), scale: 1.0, yaw: 0.0, height: 0.0, lists: None, msg: None }
    }
}

/// The Reskin a Pokémon panel: a user's model replaces a species' meshes; skeleton, animations and behavior stay.
#[derive(Default)]
struct ReskinForm {
    file: String,
    species: String,
    keep: String,
    /// species packs in the data, read when the panel first opens
    list: Option<Vec<String>>,
    /// mesh objects of the chosen species: (species, lines)
    objects: Option<(String, Result<Vec<String>, String>)>,
    msg: Option<(bool, String)>,
}

/// The Random course panel: a course from the separate snap-procgen tool, by its JSON file or by running it.
struct RandomForm {
    json: String,
    exe: String,
    seed: String,
    biome: String,
    replace: bool,
    keep_length: bool,
    msg: Option<(bool, String)>,
}

impl Default for RandomForm {
    fn default() -> Self {
        RandomForm { json: String::new(), exe: String::new(), seed: "1".into(), biome: "forest".into(), replace: true, keep_length: true, msg: None }
    }
}

/// Trees, rocks and bushes of a random course, waiting for the export that imports them as one model.
struct GenScenery {
    seed: u64,
    props: Vec<procgen::BuiltProp>,
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
    Terrain,
}

/// A terrain stroke in progress (one undo step).
struct GroundStroke {
    /// Flatten target: the ground height where the stroke started.
    flat_y: f32,
    /// Height grid before the stroke, so objects standing on the ground can follow it at the end.
    before: Vec<f32>,
    last: Vec3,
    last_rebuild: std::time::Instant,
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
    /// Rule values kept while a rule is switched off: slope, height range, rail distance range.
    rule_vals: (f32, (f32, f32), (f32, f32)),
    fill_n: usize,
    pub ground_op: ground::Op,
    pub ground_radius: f32,
    pub ground_strength: f32,
    /// Spawns and scenery standing on edited ground move with it.
    pub ground_carry: bool,
    gstroke: Option<GroundStroke>,
    import: ImportForm,
    /// built scenery of a random course, imported into the pack at the next export
    gen_scenery: Option<GenScenery>,
    /// a random course that replaces the course's own content: scenery inside this corridor is hidden at export
    gen_hide: Option<crate::hide::Corridor>,
    random: RandomForm,
    reskin: ReskinForm,
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
            rule_vals: (30.0, (-10.0, 30.0), (4.0, 25.0)),
            fill_n: 40,
            ground_op: ground::Op::Raise,
            ground_radius: 8.0,
            ground_strength: 0.5,
            ground_carry: true,
            gstroke: None,
            import: ImportForm::default(),
            gen_scenery: None,
            gen_hide: None,
            random: RandomForm::default(),
            reskin: ReskinForm::default(),
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
                self.import = ImportForm::default();
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
                if matches!(self.export_msg, Some((true, _))) && self.gen_hide.is_some() {
                    match self.hide_corridor(&root) {
                        Ok(m) => {
                            if let Some((_, msg)) = self.export_msg.as_mut() {
                                msg.push('\n');
                                msg.push_str(&m);
                            }
                        }
                        Err(e) => self.export_msg = Some((false, format!("random course, hiding the original scenery: {e}"))),
                    }
                }
                if matches!(self.export_msg, Some((true, _))) && self.gen_scenery.is_some() {
                    match self.import_gen_scenery(&root) {
                        Ok(m) => {
                            if let Some((_, msg)) = self.export_msg.as_mut() {
                                msg.push('\n');
                                msg.push_str(&m);
                            }
                        }
                        Err(e) => self.export_msg = Some((false, format!("random course scenery: {e}"))),
                    }
                }
            }
            Action::Import(root) => {
                let (Some(c), Ok(data)) = (&mut self.loaded, &self.data) else { return };
                let f = &self.import;
                let file = PathBuf::from(f.file.trim().trim_matches('"'));
                let name = if f.name.trim().is_empty() {
                    let stem = file.file_stem().and_then(|s| s.to_str()).unwrap_or("Model");
                    format!("SM_USR_{}", stem.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '_').collect::<String>())
                } else {
                    f.name.trim().to_string()
                };
                let spec = scenery::Spec {
                    file,
                    name,
                    template: f.template.clone(),
                    materials: if f.material.is_empty() { vec![] } else { vec![f.material.clone()] },
                    scale: f.scale,
                    place: (Vec3::ZERO, 0.0),
                };
                // at the selected object, else at the view's center; the height is above the ground there
                let at = self.sel.and_then(|s| edit::position(c, s)).unwrap_or(self.cam.target);
                let pos = Vec3::new(at.x, f.height, at.z);
                let (yaw, pack) = (f.yaw, self.pack_name.clone());
                let r = crate::import_pack::import_into(data, c, &spec, pos, yaw, None, &root, &pack);
                self.import.msg = Some(match r {
                    Ok(lines) => {
                        // the level bytes changed under the history's recorded edits; start it over
                        self.history = History::default();
                        self.ov_dirty = true;
                        (
                            true,
                            format!(
                                "Imported {} into {}. The editor's view still shows the course without it; ride the course in the game to see it.\n{}",
                                spec.name,
                                root.join(edit::sanitize_pack(&pack)).display(),
                                lines.join("\n")
                            ),
                        )
                    }
                    Err(e) => (false, e),
                });
            }
            Action::Random { run_generator } => {
                let f = &self.random;
                let json = if run_generator {
                    let out = std::env::temp_dir().join(format!("snap-random-course-{}.json", f.seed.trim()));
                    let r = std::process::Command::new(f.exe.trim().trim_matches('"'))
                        .args(["--seed", f.seed.trim(), "--biome", &f.biome, "--length-feet", "1300", "--size-feet", "1300", "--out"])
                        .arg(&out)
                        .output();
                    match r {
                        Ok(o) if o.status.success() => out,
                        Ok(o) => {
                            self.random.msg = Some((false, format!("snap-procgen failed: {}", String::from_utf8_lossy(&o.stderr).trim())));
                            return;
                        }
                        Err(e) => {
                            self.random.msg = Some((false, format!("could not run snap-procgen: {e}")));
                            return;
                        }
                    }
                } else {
                    PathBuf::from(f.json.trim().trim_matches('"'))
                };
                let o = procgen::Opts { replace: self.random.replace, keep_length: self.random.keep_length, ..procgen::Opts::default() };
                let r = self.generate_course(&json, o);
                self.random.msg = Some(match r {
                    Ok(m) => (true, format!("{m}\nSave as a mod pack to try it in the game (the trees, rocks and bushes are added then)."), ),
                    Err(e) => (false, e),
                });
            }
            Action::Reskin(root) => {
                let Ok(data) = &self.data else { return };
                let f = &self.reskin;
                let spec = reskin::Spec {
                    file: PathBuf::from(f.file.trim().trim_matches('"')),
                    species: f.species.clone(),
                    model: String::new(),
                    scale: 1.0,
                    keep: f.keep.split(',').map(|k| k.trim().to_string()).filter(|k| !k.is_empty()).collect(),
                    map: vec![],
                };
                let pack = self.pack_name.clone();
                let r = reskin::build(&spec, &data.root).and_then(|r| reskin::write_pack(&r, &root, &pack, &format!("Reskin of {}", spec.species)).map(|w| (r.report, w)));
                self.reskin.msg = Some(match r {
                    Ok((lines, w)) => (
                        true,
                        format!(
                            "Wrote {} file(s) to {}. The editor keeps showing markers; ride a course with {} in the game to see the new model.\n{}",
                            w.len(),
                            root.join(edit::sanitize_pack(&pack)).display(),
                            spec.species,
                            lines.join("\n")
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
            Action::Fill(n) => {
                if let Err(e) = self.fill_course(n) {
                    self.status = e;
                }
            }
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

    /// What the scatter rules need: the rail and which models are ground, sky or other.
    fn site(&self) -> place::Site {
        self.loaded.as_ref().map(place::Site::of).unwrap_or_default()
    }

    /// Rule-based fill around the rail: as far out as the rail-distance rule allows (40 units without it), following
    /// the brush's rules. One undo step.
    pub fn fill_course(&mut self, n: usize) -> Result<usize, String> {
        self.ensure_ground();
        let src = self.brush_src.ok_or("Pick what to place first: Brush, at the top of the right panel")?;
        let site = self.site();
        let margin = self.brush.rules.rail_dist.map(|r| r.1).unwrap_or(40.0);
        let (mut mn, mut mx) = site.rail_bounds(margin).ok_or("this course has no rail")?;
        // search from above the highest surface (sky and backdrops excluded) so cliffs are found from the top
        let top = self
            .ground
            .models
            .iter()
            .filter(|m| site.kinds.get(m.model) != Some(&place::SurfaceKind::Skip))
            .map(|m| m.max.y)
            .fold(mx.y, f32::max);
        mx.y = top;
        mn.y = mn.y.min(mx.y);
        self.sync_rng();
        let mut taken = vec![];
        let spots = place::fill(&self.ground, &site, &self.brush, &mut self.rng, mn, mx, n, &mut taken);
        let mut placed = 0;
        for sp in spots {
            match self.place_spot(sp, "Fill with", Some("fill")) {
                Ok(_) => placed += 1,
                Err(e) => {
                    self.status = e;
                    break;
                }
            }
        }
        self.history.end_interaction();
        if matches!(src, Sel::Model(_)) && placed > 0 {
            self.geo_dirty = true;
        }
        self.ov_dirty = true;
        self.status = format!(
            "Filled {placed} of {n} copies of {} (one undo step){}",
            self.label(src),
            if placed < n { "; the rules leave room for fewer" } else { "" }
        );
        Ok(placed)
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
        let site = self.site();
        let Some(st) = self.stroke.as_mut() else { return };
        let mut taken = std::mem::take(&mut st.taken);
        let spots = place::dab(&self.ground, &site, &self.brush, &mut self.rng, center, &mut taken);
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

    /// Makes the terrain grids on first use (from the unedited ground).
    pub fn ensure_terrain(&mut self) -> Result<(), String> {
        if self.loaded.as_ref().map(|c| c.terrain.is_some()).unwrap_or(true) {
            return self.loaded.as_ref().map(|_| ()).ok_or("no course loaded".into());
        }
        self.ensure_ground();
        let c = self.loaded.as_ref().ok_or("no course loaded")?;
        let t = ground::Terrain::build(c, &self.ground).ok_or("this course has no ground model to edit")?;
        self.loaded.as_mut().unwrap().terrain = Some(t);
        Ok(())
    }

    pub fn ground_stroke_begin(&mut self, at: Vec3) {
        if let Err(e) = self.ensure_terrain() {
            self.status = e;
            return;
        }
        let t = self.loaded.as_ref().and_then(|c| c.terrain.as_ref()).unwrap();
        let flat_y = { let h = t.height_at(at.x, at.z); if h.is_nan() { at.y } else { h } };
        self.gstroke = Some(GroundStroke { flat_y, before: t.delta.clone(), last: at, last_rebuild: std::time::Instant::now() });
        self.ground_dab(at);
    }

    /// One dab of the terrain brush. The 3D view refreshes about ten times a second during a stroke.
    pub fn ground_dab(&mut self, center: Vec3) {
        let Some(st) = self.gstroke.as_mut() else { return };
        st.last = center;
        let flat_y = st.flat_y;
        let Some(t) = self.loaded.as_ref().and_then(|c| c.terrain.as_ref()) else { return };
        let w = t.dab(self.ground_op, center, self.ground_radius, self.ground_strength, flat_y);
        let label = format!("{} ground", self.ground_op.label());
        self.apply(label, Some("ground"), Ok(w));
        if let Some(st) = self.gstroke.as_mut() {
            if st.last_rebuild.elapsed().as_millis() >= 100 {
                st.last_rebuild = std::time::Instant::now();
                self.geo_dirty = true;
            }
        }
    }

    /// Ends the stroke. With "carry", spawns and scenery that stood on the ground before the stroke move up or down
    /// with it, in the same undo step.
    pub fn ground_stroke_end(&mut self) {
        let Some(st) = self.gstroke.take() else { return };
        self.geo_dirty = true;
        if self.ground_carry && !self.ground_op.is_layer() {
            let w = self.carry_writes(&st.before);
            let n = w.len();
            if n > 0 {
                let label = format!("{} ground", self.ground_op.label());
                self.apply(label, Some("ground"), Ok(w));
            }
        }
        self.history.end_interaction();
        self.status = format!("{} ground (one undo step)", self.ground_op.label());
    }

    /// Moves that keep spawns and scenery standing on the ground after its heights changed from `before`.
    fn carry_writes(&self, before: &[f32]) -> edit::Writes {
        let mut w: edit::Writes = vec![];
        {
            if let Some(c) = self.loaded.as_ref() {
                if let Some(t) = c.terrain.as_ref() {
                    let site = self.site();
                    let moved = |p: Vec3| -> Option<f32> {
                        let old = t.sample(before, p.x, p.z);
                        let d = t.dy(p.x, p.z) - old;
                        let b = t.base_at(p.x, p.z);
                        (d.abs() > 1e-3 && !b.is_nan() && (p.y - (b + old)).abs() < 1.5).then_some(d)
                    };
                    let mut sels = vec![];
                    for (li, l) in c.levels.iter().enumerate() {
                        for (si, s) in l.spawns.iter().enumerate() {
                            sels.push((Sel::Spawn(li, si), Vec3::from(s.pos)));
                        }
                    }
                    for (mi, m) in c.models.iter().enumerate() {
                        if m.inst.alive && !t.models.contains(&mi) && site.kinds.get(mi) != Some(&place::SurfaceKind::Skip) {
                            sels.push((Sel::Model(mi), Vec3::from(m.inst.pos)));
                        }
                    }
                    for (s, p) in sels {
                        if let Some(d) = moved(p) {
                            if edit::move_mode(c, s).is_ok() {
                                if let Ok(mw) = edit::move_by(c, s, Vec3::Y * d) {
                                    w.extend(mw);
                                }
                            }
                        }
                    }
                }
            }
        }
        w
    }

    /// Flowers a random course can copy: the course's own flower props that have their own spawn point.
    fn flower_sources(c: &LoadedCourse) -> Vec<Sel> {
        let mut out: Vec<(String, Sel)> = vec![];
        for (li, l) in c.levels.iter().enumerate() {
            for (si, sp) in l.spawns.iter().enumerate() {
                let flower = (sp.label.starts_with("CH_000_Flower") || sp.label.starts_with("CH_001_Flower")) && sp.alive && sp.alive_off.is_none();
                if flower && Self::can_copy(c, Sel::Spawn(li, si)).is_ok() && sp.pos != [0.0; 3] && !out.iter().any(|o| o.0 == sp.label) {
                    out.push((sp.label.clone(), Sel::Spawn(li, si)));
                }
            }
        }
        out.into_iter().map(|o| o.1).collect()
    }

    /// Writes the course's own scenery meshes with every object inside the random course's corridor collapsed (drawing
    /// nothing), and the ground collision with the rocks there collapsed, into the pack. Ground, water, sky, backdrops,
    /// lighting helpers and imported models stay.
    fn hide_corridor(&mut self, root: &std::path::Path) -> Result<String, String> {
        let Some(cor) = self.gen_hide.clone() else { return Ok(String::new()) };
        let c = self.loaded.as_ref().ok_or("no course loaded")?;
        let arch = root.join(edit::sanitize_pack(&self.pack_name)).join(c.archive.replace('/', std::path::MAIN_SEPARATOR_STR));
        std::fs::create_dir_all(&arch).map_err(|e| e.to_string())?;
        let inside = |lo: Vec3, hi: Vec3| cor.holds(lo, hi);
        let mut done = std::collections::BTreeSet::new();
        let (mut objects, mut files, mut shapes) = (0, 0, 0);
        let mut groups = vec![];
        for m in &c.models {
            let n = &m.inst.name;
            let skip = ground::is_terrain_name(n)
                || place::Site::kind_of_name(n) == place::SurfaceKind::Skip
                || n.contains("LightingModel")
                || n.contains("Water")
                || n.starts_with("SM_USR");
            let Some(src) = m.source.as_ref().filter(|_| !skip) else { continue };
            let Some(dir) = src.parent() else { continue };
            if !dir.to_string_lossy().replace('\\', "/").to_lowercase().ends_with(&c.archive.to_lowercase()) {
                continue;
            }
            let Ok(modl) = ssbh_data::prelude::ModlData::from_file(src) else { continue };
            let mut hid = 0;
            for f in crate::hide::mesh_files(dir, &modl.mesh_file_name) {
                if !done.insert(f.clone()) {
                    continue;
                }
                if let Some((b, k)) = crate::hide::collapse_mesh(&f, ground::inst_xf(&m.inst), &inside)? {
                    let out = arch.join(f.file_name().ok_or("bad file name")?);
                    std::fs::write(&out, b).map_err(|e| format!("{}: {e}", out.display()))?;
                    files += 1;
                    hid = hid.max(k);
                }
            }
            if hid > 0 {
                objects += hid;
                groups.push(format!("{n} {hid}"));
            }
        }
        // rocks in the ground collision: build on the pack's copy (the terrain export wrote it)
        for m in c.models.iter().filter(|m| ground::is_terrain_name(&m.inst.name)) {
            let Some(src) = &m.source else { continue };
            let fname = format!("{}.DC4A8177", m.inst.name);
            let packed = arch.join(&fname);
            let path = if packed.exists() { packed.clone() } else { src.parent().ok_or("model without folder")?.join(&fname) };
            let Ok(b) = std::fs::read(&path) else { continue };
            let (nb, k) = crate::hide::collapse_collision(&b, ground::inst_xf(&m.inst), &inside)?;
            if k > 0 {
                std::fs::write(&packed, nb).map_err(|e| format!("{}: {e}", packed.display()))?;
                shapes += k;
            }
        }
        let shadows = match self.clear_ghost_shadows(&arch, &cor) {
            Ok(s) => s,
            Err(e) => format!("Shadows of the hidden scenery stay: {e}."),
        };
        Ok(format!(
            "Original scenery along the rail hidden: {objects} objects in {files} mesh files ({}); {shapes} rock collision shapes removed. {shadows}",
            groups.join(", ")
        ))
    }

    /// Clears the baked sun shadows of the scenery `hide_corridor` hides from the course's Shadow textures (see
    /// `shadow.rs`), writing the changed textures into the pack beside the meshes. Reads the original files only.
    fn clear_ghost_shadows(&self, arch: &std::path::Path, cor: &crate::hide::Corridor) -> Result<String, String> {
        use ssbh_data::mesh_data::VectorData;
        use ssbh_data::prelude::*;
        let c = self.loaded.as_ref().ok_or("no course loaded")?;
        let sun = crate::shadow::sun_for(&c.archive).ok_or("no sun direction known for this course")?;
        let v3 = |d: &VectorData| -> Vec<Vec3> {
            match d {
                VectorData::Vector2(v) => v.iter().map(|a| Vec3::new(a.x, a.y, 0.0)).collect(),
                VectorData::Vector3(v) => v.iter().map(|a| Vec3::new(a.x, a.y, a.z)).collect(),
                VectorData::Vector4(v) => v.iter().map(|a| Vec3::new(a.x, a.y, a.z)).collect(),
            }
        };
        struct Obj {
            model: String,
            name: String,
            sub: u64,
            pos: Vec<Vec3>,
            uv: Vec<[f32; 2]>,
            idx: Vec<u32>,
            hidden: bool,
        }
        // every object that can cast or receive a sun shadow, in world space; hidden by the same rule as the meshes
        let mut objs: Vec<Obj> = vec![];
        let mut dir_of_course = None;
        for m in &c.models {
            let n = &m.inst.name;
            if place::Site::kind_of_name(n) == place::SurfaceKind::Skip || n.contains("LightingModel") || n.contains("Water") || n.starts_with("SM_USR") {
                continue;
            }
            let Some(src) = m.source.as_ref() else { continue };
            let Some(dir) = src.parent() else { continue };
            let ours = dir.to_string_lossy().replace('\\', "/").to_lowercase().ends_with(&c.archive.to_lowercase());
            if ours && dir_of_course.is_none() {
                dir_of_course = Some(dir.to_path_buf());
            }
            let Ok(modl) = ModlData::from_file(src) else { continue };
            let Ok(mesh) = MeshData::from_file(dir.join(&modl.mesh_file_name)) else { continue };
            let xf = ground::inst_xf(&m.inst);
            let can_hide = ours && !ground::is_terrain_name(n);
            for o in &mesh.objects {
                let Some(p) = o.positions.first() else { continue };
                let pos: Vec<Vec3> = v3(&p.data).into_iter().map(|q| xf.transform_point3(q)).collect();
                if pos.is_empty() {
                    continue;
                }
                let lo = pos.iter().fold(Vec3::splat(f32::MAX), |a, q| a.min(*q));
                let hi = pos.iter().fold(Vec3::splat(f32::MIN), |a, q| a.max(*q));
                let uv = o
                    .texture_coordinates
                    .last()
                    .map(|t| v3(&t.data).into_iter().map(|q| [q.x, q.y]).collect::<Vec<_>>())
                    .filter(|u| u.len() == pos.len())
                    .unwrap_or_default();
                let count = pos.len() as u32;
                let mut idx: Vec<u32> = o.vertex_indices.iter().copied().filter(|&i| i < count).collect();
                idx.truncate(idx.len() / 3 * 3);
                objs.push(Obj { model: n.clone(), name: o.name.clone(), sub: o.subindex, pos, uv, idx, hidden: can_hide && cor.holds(lo, hi) });
            }
        }
        let tris = |o: &Obj| -> Vec<[Vec3; 3]> { o.idx.chunks_exact(3).map(|t| [o.pos[t[0] as usize], o.pos[t[1] as usize], o.pos[t[2] as usize]]).collect() };
        let hidden: Vec<[Vec3; 3]> = objs.iter().filter(|o| o.hidden).flat_map(|o| tris(o)).collect();
        let Some(mut map) = crate::shadow::SunMap::new(sun, &hidden, 0.1, 2.0) else { return Ok(String::new()) };
        for o in objs.iter().filter(|o| !o.hidden) {
            for t in tris(o) {
                map.draw(&t, false);
            }
        }
        // the course's Shadow textures by (model, shape, subindex)
        let dir = dir_of_course.ok_or("course folder not found")?;
        let mut files = std::collections::HashMap::new();
        for e in std::fs::read_dir(&dir).map_err(|e| e.to_string())?.flatten() {
            let f = e.file_name().to_string_lossy().to_string();
            let Some(stem) = f.strip_suffix("^Shadow.nutexb") else { continue };
            let p: Vec<&str> = stem.split('^').collect();
            if p.len() == 5 {
                if let Ok(sub) = p[4].parse::<u64>() {
                    files.insert((p[1].to_string(), p[3].to_string(), sub), f.clone());
                }
            }
        }
        let (mut textures, mut total) = (0, crate::shadow::Stats::default());
        for o in objs.iter().filter(|o| !o.hidden && !o.uv.is_empty()) {
            if !o.pos.iter().any(|p| map.covers(*p)) {
                continue;
            }
            let Some(f) = files.get(&(o.model.clone(), o.name.clone(), o.sub)) else { continue };
            let Ok(b) = std::fs::read(dir.join(f)) else { continue };
            // only ground inside the rail strip: the strip plus the reach of a tall tree's shadow at this sun
            let reach = cor.width + 20.0;
            let idx: Vec<u32> = o
                .idx
                .chunks_exact(3)
                .filter(|t| t.iter().any(|&i| cor.dist(glam::Vec2::new(o.pos[i as usize].x, o.pos[i as usize].z)) < reach))
                .flatten()
                .copied()
                .collect();
            if idx.is_empty() {
                continue;
            }
            let (nb, st) = match crate::shadow::clear_texture(&b, &o.uv, &o.pos, &idx, &map, 0.3) {
                Ok(r) => r,
                Err(_) => continue,
            };
            total.shadowed += st.shadowed;
            total.agree += st.agree;
            if let Some(nb) = nb {
                let out = arch.join(f);
                std::fs::write(&out, nb).map_err(|e| format!("{}: {e}", out.display()))?;
                textures += 1;
                total.cleared += st.cleared;
            }
        }
        Ok(format!(
            "Their baked sun shadows cleared: {} texels in {textures} shadow textures (the sun map finds {:.0}% of the baked shadow it covers).",
            total.cleared,
            100.0 * total.agree as f32 / total.shadowed.max(1) as f32
        ))
    }

    /// Imports a random course's trees, rocks and bushes into the pack as one model with the course's materials. The
    /// level gains an instance, so the undo history starts over (as after an import).
    fn import_gen_scenery(&mut self, root: &std::path::Path) -> Result<String, String> {
        let Some(gs) = self.gen_scenery.take() else { return Ok(String::new()) };
        let n = gs.props.len();
        let (Some(c), Ok(data)) = (&mut self.loaded, &self.data) else { return Err("no course loaded".into()) };
        // the importer puts the model on the surface under its anchor; the OBJ is written relative to that same point
        let a = gs.props[0].pos;
        let (_, surf) = render::build_geometry(c, render::Layers::default());
        let h = surf.raycast(Vec3::new(a.x, 1000.0, a.z), Vec3::NEG_Y, 2000.0, None).map(|h| h.point.y).unwrap_or(0.0);
        let anchor = Vec3::new(a.x, h, a.z);
        let dir = std::env::temp_dir().join(format!("snap-random-course-{}", gs.seed));
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let (obj, mtl, mats) = procgen::scenery_obj(&gs.props, anchor, "scenery.mtl");
        std::fs::write(dir.join("scenery.obj"), obj).map_err(|e| e.to_string())?;
        std::fs::write(dir.join("scenery.mtl"), mtl).map_err(|e| e.to_string())?;
        let spec = scenery::Spec {
            file: dir.join("scenery.obj"),
            name: format!("SM_USR_Random{}", gs.seed),
            template: "SM_ENV_001_1_Rock2".into(),
            materials: mats,
            scale: 1.0,
            place: (Vec3::ZERO, 0.0),
        };
        let pack = self.pack_name.clone();
        let lines = match crate::import_pack::import_into(data, c, &spec, Vec3::new(a.x, 0.0, a.z), 0.0, None, root, &pack) {
            Ok(l) => l,
            Err(e) => {
                self.gen_scenery = Some(gs);
                return Err(e);
            }
        };
        self.history = History::default();
        self.ov_dirty = true;
        Ok(format!("Random course scenery: {} props as {}.\n{}", n, spec.name, lines.join("\n")))
    }

    /// Owner list item 7: lays a course made by snap-procgen (its JSON) over this leg's first rail part: ground relief
    /// and texture in a band along the rail, a swinging and climbing rail with the same point count and ends, copies of
    /// the course's own scenery and Pokémon at the generated spots. One undo step.
    pub fn generate_course(&mut self, json: &std::path::Path, o: procgen::Opts) -> Result<String, String> {
        let g = procgen::Gen::load(json)?;
        self.ensure_terrain()?;
        self.ensure_ground();
        let c = self.loaded.as_ref().ok_or("no course loaded")?;
        let (l, pi) = edit::rail_ref(c).ok_or("this course has no rail")?;
        let rail: Vec<Vec3> = c.levels[l].paths[pi].segments.first().ok_or("the rail has no parts")?.iter().map(|p| Vec3::from(*p)).collect();
        let t = c.terrain.as_ref().ok_or("this course has no ground to reshape")?;
        // rays see the course's geometry except the sky, backdrops and lighting helper shapes
        let model_rays = &self.ground;
        let solid = |m: usize| c.models.get(m).map(|x| place::Site::kind_of_name(&x.inst.name) != place::SurfaceKind::Skip && !x.inst.name.contains("LightingModel")).unwrap_or(false);
        let clear = |p: Vec3, d: Vec3, max: f32| model_rays.raycast_where(p, d, max, &solid).map(|h| h.point.distance(p));
        let plan = procgen::plan(&g, &rail, t, o, &clear)?;
        let before = t.delta.clone();
        let flowers = Self::flower_sources(c);
        if std::env::var_os("SNAP_PROCGEN_DEBUG").is_some() {
            for (i, (a, b)) in rail.iter().zip(&plan.rail).enumerate() {
                eprintln!("rail {i}: {a:?} -> {b:?}");
            }
        }
        // Pokémon to copy: flyers (well above the ground) for air spawns, the rest for grass, tree and water spawns
        let (mut air, mut land) = (vec![], vec![]);
        for (_, s) in Self::species_sources(c) {
            let lift = self.src_height(s);
            if lift > 1.5 {
                air.push((s, lift));
            } else {
                land.push((s, 0.0));
            }
        }
        let key = Some("random course");
        let label = || format!("Random course (seed {}, {})", g.seed, g.biome);
        // 1. ground heights and texture, then everything standing on the ground follows
        let w: edit::Writes = plan.height.iter().map(|&(i, v)| (ground::LEVEL_HEIGHT, i, v)).chain(plan.layer.iter().map(|&(i, v)| (ground::LEVEL_LAYER, i, v))).collect();
        self.apply(label(), key, Ok(w));
        let w = self.carry_writes(&before);
        self.apply(label(), key, Ok(w));
        // 2. rail
        let c = self.loaded.as_ref().unwrap();
        let w = edit::set_rail_part(c, 0, &plan.rail)?;
        self.apply(label(), key, Ok(w));
        // 3. scenery and Pokémon on the new ground
        let ground_y = |me: &Self, p: glam::Vec2| -> Option<f32> {
            let t = me.loaded.as_ref()?.terrain.as_ref()?;
            let h = t.height_at(p.x, p.y);
            (!h.is_nan()).then_some(h)
        };
        // trees, rocks and bushes are built as one model at export; flowers are copies of the course's flower props
        let mut built = vec![];
        let mut flower_n = 0;
        let mut skipped = 0;
        for (k, pr) in plan.props.iter().enumerate() {
            let Some(y) = ground_y(self, pr.pos) else {
                skipped += 1;
                continue;
            };
            if pr.class == "flower" {
                if flowers.is_empty() {
                    skipped += 1;
                    continue;
                }
                let src = flowers[(k + g.seed as usize) % flowers.len()];
                let c = self.loaded.as_mut().unwrap();
                let rot = edit::rot_of(c, src).map(|r| place::orient(r, Some(pr.yaw), Vec3::Y, false));
                match edit::place_copy(c, src, Vec3::new(pr.pos.x, y, pr.pos.y), rot, 1.0) {
                    Ok((_, w)) => {
                        self.apply(label(), key, Ok(w));
                        flower_n += 1;
                    }
                    Err(_) => skipped += 1,
                }
            } else {
                built.push(procgen::BuiltProp { class: pr.class.clone(), pos: Vec3::new(pr.pos.x, y, pr.pos.y), yaw: pr.yaw, scale: pr.scale });
            }
        }
        let mut placed = std::collections::BTreeMap::<String, usize>::new();
        for b in &built {
            *placed.entry(b.class.clone()).or_default() += 1;
        }
        placed.insert("flower".into(), flower_n);
        self.gen_scenery = (!built.is_empty()).then(|| GenScenery { seed: g.seed, props: built });
        // generated spawn points, by habitat: air -> a flyer at its own height; tree -> a ground species at the foot of the
        // nearest generated tree; grass and water -> a ground species on the ground. Each habitat walks through its
        // species in turn, so neighbors differ.
        let trees: Vec<Vec3> = self.gen_scenery.as_ref().map(|g| g.props.iter().filter(|p| p.class == "tree").map(|p| p.pos).collect()).unwrap_or_default();
        let mut pokemon = vec![];
        let mut by_habitat = std::collections::BTreeMap::<String, (usize, Vec<String>)>::new();
        for sp in plan.spawns.iter() {
            let air_spawn = sp.habitat == "air" && !air.is_empty();
            let list = if air_spawn { &air } else { &land };
            if list.is_empty() {
                continue;
            }
            let mut at = sp.pos;
            if sp.habitat == "tree" {
                if let Some(t) = trees.iter().map(|t| glam::Vec2::new(t.x, t.z)).filter(|t| t.distance(sp.pos) < 6.0).min_by(|a, b| a.distance(sp.pos).total_cmp(&b.distance(sp.pos))) {
                    // beside the trunk, on the rail's side
                    at = t + (sp.pos - t).normalize_or_zero() * 1.2;
                }
            }
            let Some(y) = ground_y(self, at) else { continue };
            let e = by_habitat.entry(sp.habitat.clone()).or_default();
            let (src, lift) = list[(e.0 + g.seed as usize) % list.len()];
            e.0 += 1;
            let c = self.loaded.as_mut().unwrap();
            let rot = edit::rot_of(c, src).map(|r| place::orient(r, Some(sp.yaw), Vec3::Y, false));
            if let Ok((ns, w)) = edit::place_copy(c, src, Vec3::new(at.x, y + lift, at.y), rot, 1.0) {
                self.apply(label(), key, Ok(w));
                let name = self.label(ns);
                by_habitat.get_mut(&sp.habitat).unwrap().1.push(name.clone());
                pokemon.push(name);
            }
        }
        // 4. replace: the course's own Pokémon and props inside the corridor go 100 units under the ground (spawns
        // cannot be deleted from the file); routes move whole. The scenery there is hidden at export.
        let mut sunk = 0;
        self.gen_hide = None;
        if o.replace {
            let cor = crate::hide::Corridor { rail: plan.rail.iter().map(|p| glam::Vec2::new(p.x, p.z)).collect(), width: o.band, max_size: 3.0 * o.band };
            let c = self.loaded.as_ref().unwrap();
            let mut moved_routes = std::collections::BTreeSet::new();
            let mut w: edit::Writes = vec![];
            for (li, l) in c.levels.iter().enumerate() {
                for (si, sp) in l.spawns.iter().enumerate() {
                    if sp.alive_off.is_some() || sp.pos == [0.0; 3] {
                        continue;
                    }
                    let s = Sel::Spawn(li, si);
                    let routed = sp.pos_from_path || sp.pos_off.is_none();
                    if routed {
                        let Some(pi) = sp.path_idx else { continue };
                        let pts: Vec<Vec3> = l.paths[pi].segments.iter().flatten().map(|p| Vec3::from(*p)).collect();
                        let inn = pts.iter().filter(|p| cor.has_point(**p)).count();
                        if pts.is_empty() || inn * 2 < pts.len() || !moved_routes.insert((li, pi)) {
                            continue;
                        }
                    } else if !cor.has_point(Vec3::from(sp.pos)) {
                        continue;
                    }
                    if let Ok(mw) = edit::move_by(c, s, Vec3::new(0.0, -100.0, 0.0)) {
                        w.extend(mw);
                        sunk += 1;
                    }
                }
            }
            self.apply(label(), key, Ok(w));
            self.gen_hide = Some(cor);
        }
        self.history.end_interaction();
        self.geo_dirty = true;
        self.ov_dirty = true;
        self.layers.rail_points = true;
        let c = self.loaded.as_ref().unwrap();
        let st = edit::rail_stats(c).ok_or("no rail")?;
        let issues: Vec<String> = edit::checks(c, &self.ground).into_iter().filter(|i| matches!(i.sel, Sel::Rail(..))).map(|i| i.text).collect();
        let msg = format!(
            "Random course seed {} ({}, rail {:.0} ft), {:.3} units per foot: ground reshaped at {} points, rail swings up to {:.1} units ({}, part 0 {:.1} -> {:.1} units) and moves {:+.1} to {:+.1} units in height; {}; scenery {:?} (trees, rocks and bushes are added as one model at export){}; {} Pokémon ({}); {}rail checks: {}",
            g.seed,
            g.biome,
            g.rail_length(),
            plan.scale,
            plan.height.len(),
            plan.swing,
            if (plan.stretch - 1.0).abs() < 0.005 {
                "the generated swing".to_string()
            } else if plan.stretch < 0.0 {
                format!("the generated swing mirrored at {:.0}% to keep the ride time", -plan.stretch * 100.0)
            } else {
                format!("{:.0}% of the generated swing to keep the ride time", plan.stretch * 100.0)
            },
            plan.length.0,
            plan.length.1,
            plan.rail_relief.0,
            plan.rail_relief.1,
            st.summary(),
            placed,
            if skipped > 0 { format!(" ({skipped} spots had no matching model or ground)") } else { String::new() },
            pokemon.len(),
            by_habitat.iter().map(|(h, (_, v))| format!("{h}: {}", v.join(", "))).collect::<Vec<_>>().join("; "),
            if o.replace { format!("original Pokémon and props moved under the ground: {sunk} (the scenery along the rail is hidden at export); ") } else { String::new() },
            if issues.is_empty() { "none".into() } else { issues.join("; ") }
        );
        self.status = msg.clone();
        Ok(msg)
    }

    /// Command-line terrain stroke: `n` dabs of the current terrain brush at (x, z).
    pub fn ground_at(&mut self, op: ground::Op, x: f32, z: f32, n: usize) -> Result<(), String> {
        self.ensure_terrain()?;
        let t = self.loaded.as_ref().and_then(|c| c.terrain.as_ref()).unwrap();
        let y = t.height_at(x, z);
        if y.is_nan() {
            return Err(format!("no ground at {x}, {z}"));
        }
        self.ground_op = op;
        let p = Vec3::new(x, y, z);
        self.ground_stroke_begin(p);
        for _ in 1..n.max(1) {
            self.ground_dab(p);
        }
        self.ground_stroke_end();
        Ok(())
    }

    /// Command-line scatter: one dab of `brush` around (x, z), as a drag in the Scatter tool would make.
    pub fn scatter_at(&mut self, src: Sel, x: f32, z: f32) -> Result<usize, String> {
        self.ensure_ground();
        let c = self.loaded.as_ref().ok_or("no course loaded")?;
        Self::can_copy(c, src)?;
        self.brush_src = Some(src);
        let (p, _) = place::surface_below(&self.ground, &self.site(), Vec3::new(x, 0.0, z), 1000.0, false).ok_or("no ground at that spot")?;
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
                if self.tool == Tool::Terrain && self.loaded.is_some() {
                    self.terrain_ui(ui);
                    ui.separator();
                } else if self.tool != Tool::Select && self.loaded.is_some() {
                    self.placing_ui(ui, &mut acts);
                    ui.separator();
                }
                self.inspector(ui, &mut acts);
                ui.separator();
                self.checks_ui(ui, &mut acts);
                self.export_ui(ui, &mut acts);
                self.import_ui(ui, &mut acts);
                self.reskin_ui(ui, &mut acts);
                self.random_ui(ui, &mut acts);
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
        let ground_radius = &mut self.ground_radius;
        ui.input_mut(|i| {
            if *tool == Tool::Terrain {
                if i.consume_key(Modifiers::NONE, Key::OpenBracket) {
                    *ground_radius = (*ground_radius / 1.25).max(1.0);
                }
                if i.consume_key(Modifiers::NONE, Key::CloseBracket) {
                    *ground_radius = (*ground_radius * 1.25).min(60.0);
                }
            }
            if i.consume_key(Modifiers::COMMAND | Modifiers::SHIFT, Key::Z) || i.consume_key(Modifiers::COMMAND, Key::Y) {
                acts.push(Action::Redo);
            } else if i.consume_key(Modifiers::COMMAND, Key::Z) {
                acts.push(Action::Undo);
            }
            if i.consume_key(Modifiers::NONE, Key::Escape) {
                acts.push(Action::Select(None));
            }
            for (k, t) in [(Key::S, Tool::Select), (Key::P, Tool::Place), (Key::B, Tool::Scatter), (Key::T, Tool::Terrain)] {
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

    /// Brush settings for the Terrain tool.
    fn terrain_ui(&mut self, ui: &mut egui::Ui) {
        ui.heading("Terrain");
        ui.label("Drag over the ground to shape it or paint its top texture layer (on Florio, grass over dirt). Each stroke is one undo step. Alt+drag turns the camera.");
        ui.horizontal_wrapped(|ui| {
            for op in ground::Op::ALL {
                ui.selectable_value(&mut self.ground_op, op, op.label()).on_hover_text(op.tip());
            }
        });
        ui.add(egui::Slider::new(&mut self.ground_radius, 1.0..=60.0).text("size").logarithmic(true))
            .on_hover_text("Brush radius in world units. [ and ] change it too.");
        ui.add(egui::Slider::new(&mut self.ground_strength, 0.05..=1.0).text("strength"))
            .on_hover_text("How much each dab changes the ground. Low values build up slowly as you drag.");
        ui.checkbox(&mut self.ground_carry, "Pokémon and scenery follow the ground")
            .on_hover_text("When a stroke raises or lowers the ground, spawns and scenery standing on it move with it");
        let info = self.loaded.as_ref().and_then(|c| c.terrain.as_ref()).map(|t| (t.models.len(), t.edited()));
        match info {
            Some((n, true)) => ui.weak(format!("{n} ground model(s) edited. Save as a mod pack writes their mesh, collision and navmesh.")),
            Some((n, false)) => ui.weak(format!("{n} ground model(s) ready to edit.")),
            None => ui.weak("The height grid is made from the ground on the first stroke."),
        };
    }

    /// Brush settings for the Place and Scatter tools.
    fn placing_ui(&mut self, ui: &mut egui::Ui, acts: &mut Vec<Action>) {
        let Some(c) = &self.loaded else { return };
        let scatter = self.tool == Tool::Scatter;
        ui.heading(if scatter { "Scatter" } else { "Place" });
        ui.label(if scatter {
            "Drag over the ground to paint copies of the brush, or fill the course by rules below. Each stroke or fill is one undo step. Alt+drag turns the camera."
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
        if !scatter {
            return;
        }
        let (slope, height, rail) = &mut self.rule_vals;
        let r = &mut b.rules;
        // rules set elsewhere (command line) show their own values
        if let Some(v) = r.max_slope {
            *slope = v;
        }
        if let Some(v) = r.height {
            *height = v;
        }
        if let Some(v) = r.rail_dist {
            *rail = v;
        }
        egui::CollapsingHeader::new("Rules: where copies may land").default_open(true).show(ui, |ui| {
            ui.weak("They apply to brush strokes and to Fill. A single Place click ignores them.");
            let mut on = r.max_slope.is_some();
            ui.horizontal(|ui| {
                ui.checkbox(&mut on, "Slope at most").on_hover_text("Skip ground steeper than this, in degrees from flat (cliffs, rock faces)");
                ui.add_enabled(on, egui::DragValue::new(slope).range(0.0..=90.0).suffix("°"));
            });
            r.max_slope = on.then_some(*slope);
            let mut on = r.height.is_some();
            ui.horizontal(|ui| {
                ui.checkbox(&mut on, "Height from").on_hover_text("Only between these world heights (Y), e.g. keep trees out of a valley or off a peak");
                ui.add_enabled(on, egui::DragValue::new(&mut height.0).speed(0.1));
                ui.label("to");
                ui.add_enabled(on, egui::DragValue::new(&mut height.1).speed(0.1));
            });
            r.height = on.then_some((height.0.min(height.1), height.0.max(height.1)));
            let mut on = r.rail_dist.is_some();
            ui.horizontal(|ui| {
                ui.checkbox(&mut on, "Distance from the rail")
                    .on_hover_text("Only this far from the camera rail, measured flat. A near edge keeps the view clear; a far edge keeps copies where the player can see them.");
                ui.add_enabled(on, egui::DragValue::new(&mut rail.0).speed(0.1).range(0.0..=500.0));
                ui.label("to");
                ui.add_enabled(on, egui::DragValue::new(&mut rail.1).speed(0.1).range(0.0..=500.0));
            });
            r.rail_dist = on.then_some((rail.0.min(rail.1), rail.0.max(rail.1)));
            ui.checkbox(&mut r.ground_only, "Only on the ground").on_hover_text("Land on the course's ground models, not on trees, rocks or water");
        });
        ui.horizontal(|ui| {
            ui.label("Fill the course with");
            ui.add(egui::DragValue::new(&mut self.fill_n).range(1..=500)).on_hover_text("How many copies Fill tries to place");
            ui.label("copies");
            if ui
                .add_enabled(self.brush_src.is_some(), egui::Button::new("Fill"))
                .on_hover_text("Spreads copies over the area around the rail (out to the rail-distance rule's far edge, or 40 units), following the rules. One undo step.")
                .clicked()
            {
                acts.push(Action::Fill(self.fill_n));
            }
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

    fn import_ui(&mut self, ui: &mut egui::Ui, acts: &mut Vec<Action>) {
        let Some(c) = &self.loaded else { return };
        egui::CollapsingHeader::new("Import a model").default_open(false).show(ui, |ui| {
            if self.import.lists.is_none() {
                self.import.lists = Some(match &self.data {
                    Ok(d) => crate::drp::Archive::open(&crate::import_pack::romfs_archive(d, None, &c.archive)).map(|a| (scenery::templates(&a), scenery::materials(&a))),
                    Err(e) => Err(e.clone()),
                });
            }
            let f = &mut self.import;
            ui.horizontal(|ui| {
                ui.label("Model file");
                ui.text_edit_singleline(&mut f.file).on_hover_text("Full path of a .glb, .gltf or .obj file. It stays on your PC; only the pack gets a converted copy.");
            });
            ui.horizontal(|ui| {
                ui.label("Name");
                ui.add(egui::TextEdit::singleline(&mut f.name).hint_text("SM_USR_<file name>"))
                    .on_hover_text("The new model's name in the course: letters, digits and _ only, and new to this course");
            });
            match f.lists.as_ref() {
                Some(Ok((templates, materials))) => {
                    egui::ComboBox::from_label("Template")
                        .selected_text(if f.template.is_empty() { "choose" } else { f.template.as_str() })
                        .show_ui(ui, |ui| {
                            for t in templates {
                                ui.selectable_value(&mut f.template, t.clone(), t);
                            }
                        })
                        .response
                        .on_hover_text("A model of this course to copy settings from: its vertex layout, shading and lightmap. A rock or a ground piece suits most props.");
                    egui::ComboBox::from_label("Material")
                        .selected_text(if f.material.is_empty() { "choose" } else { f.material.as_str() })
                        .show_ui(ui, |ui| {
                            for m in materials {
                                ui.selectable_value(&mut f.material, m.clone(), m);
                            }
                        })
                        .response
                        .on_hover_text("The game material every part of the model uses (its textures and shader). For one material per part, use --import with --material on the command line.");
                }
                Some(Err(e)) => {
                    ui.colored_label(egui::Color32::LIGHT_RED, format!("Can't read the course archive: {e}"));
                }
                None => {}
            }
            ui.horizontal(|ui| {
                ui.add(egui::DragValue::new(&mut f.scale).speed(0.01).range(0.001..=1000.0).prefix("scale "))
                    .on_hover_text("Size multiplier; glTF files are in meters");
                ui.add(egui::DragValue::new(&mut f.yaw).speed(1.0).range(-360.0..=360.0).suffix("°").prefix("turn "))
                    .on_hover_text("Turn around the vertical axis, in degrees");
                ui.add(egui::DragValue::new(&mut f.height).speed(0.05).prefix("height "))
                    .on_hover_text("Height above the ground under the placement point");
            });
            let at = if self.sel.is_some() { "the selected object" } else { "the center of the view" };
            ui.weak(format!("Goes at {at}, into the pack \"{}\" in the Folder above.", self.pack_name));
            let ready = !f.file.trim().is_empty() && !f.template.is_empty() && !f.material.is_empty();
            if ui
                .add_enabled(ready, egui::Button::new("Import into the pack"))
                .on_hover_text("Converts the model, adds it to the course archive with collision, places it, and writes everything into the pack. A second import into the same pack adds to it.")
                .on_disabled_hover_text("Fill in the model file, template and material first")
                .clicked()
            {
                acts.push(Action::Import(PathBuf::from(&self.export_dir)));
            }
            if let Some((ok, m)) = &f.msg {
                ui.colored_label(if *ok { egui::Color32::LIGHT_GREEN } else { egui::Color32::LIGHT_RED }, m);
            }
        });
    }

    fn random_ui(&mut self, ui: &mut egui::Ui, acts: &mut Vec<Action>) {
        if self.loaded.is_none() {
            return;
        }
        egui::CollapsingHeader::new("Random course").default_open(false).show(ui, |ui| {
            let f = &mut self.random;
            ui.weak("Lays a course made from a seed over this leg's first rail part: new hills and hollows along the rail, a winding path, trees, rocks, flowers and Pokémon. The rail keeps its start and end, so the rest of the leg still follows. One undo step.");
            ui.horizontal(|ui| {
                ui.label("Course file");
                ui.text_edit_singleline(&mut f.json).on_hover_text("Full path of a course .json written by snap-procgen (format 2)");
            });
            ui.checkbox(&mut f.replace, "Replace the course's own scenery and Pokémon along the rail")
                .on_hover_text("Hides the original trees, rocks, plants and Pokémon within 10 units of the new rail, so only the random course shows there. The scenery is hidden when you save the mod pack.");
            ui.checkbox(&mut f.keep_length, "Keep the original ride time")
                .on_hover_text("Widens, narrows or mirrors the rail's sideways swing until the ride is as long as the original. On some courses this leaves only a small swing; untick it to keep the course's full swing and accept a shorter or longer ride.");
            if ui.add_enabled(!f.json.trim().is_empty(), egui::Button::new("Lay this course over the leg")).on_disabled_hover_text("Fill in the course file first").clicked() {
                acts.push(Action::Random { run_generator: false });
            }
            ui.separator();
            ui.horizontal(|ui| {
                ui.label("Generator");
                ui.text_edit_singleline(&mut f.exe).on_hover_text("Full path of snap-procgen.exe, a separate program that makes courses from a seed");
            });
            ui.horizontal(|ui| {
                ui.label("Seed");
                ui.add(egui::TextEdit::singleline(&mut f.seed).desired_width(80.0)).on_hover_text("Any whole number; the same seed makes the same course");
                egui::ComboBox::from_label("Look")
                    .selected_text(f.biome.as_str())
                    .show_ui(ui, |ui| {
                        for b in ["forest", "beach", "volcano", "cave", "snow", "night"] {
                            ui.selectable_value(&mut f.biome, b.to_string(), b);
                        }
                    })
                    .response
                    .on_hover_text("The generator's terrain style: how steep the hills are, how much is bare ground");
            });
            let ready = !f.exe.trim().is_empty() && f.seed.trim().parse::<u64>().is_ok();
            if ui
                .add_enabled(ready, egui::Button::new("Make a course from the seed"))
                .on_hover_text("Runs the generator, then lays its course over the leg")
                .on_disabled_hover_text("Fill in the generator and a whole-number seed first")
                .clicked()
            {
                acts.push(Action::Random { run_generator: true });
            }
            if let Some((ok, m)) = &f.msg {
                ui.colored_label(if *ok { egui::Color32::LIGHT_GREEN } else { egui::Color32::LIGHT_RED }, m);
            }
        });
    }

    fn reskin_ui(&mut self, ui: &mut egui::Ui, acts: &mut Vec<Action>) {
        let Ok(data) = &self.data else { return };
        // a selected Pokémon marker suggests its species
        let picked = match (self.sel, &self.loaded) {
            (Some(Sel::Spawn(l, i)), Some(c)) => c.levels.get(l).and_then(|l| l.spawns.get(i)).and_then(|s| {
                let at = s.label.find("CH_")?;
                s.label.get(at..at + 6).filter(|id| id[3..].chars().all(|ch| ch.is_ascii_digit())).map(str::to_string)
            }),
            _ => None,
        };
        egui::CollapsingHeader::new("Reskin a Pokémon").default_open(false).show(ui, |ui| {
            let f = &mut self.reskin;
            if f.list.is_none() {
                f.list = Some(reskin::species(&data.root));
            }
            if f.species.is_empty() {
                if let Some(p) = &picked {
                    f.species = p.clone();
                }
            }
            ui.horizontal(|ui| {
                ui.label("Model file");
                ui.text_edit_singleline(&mut f.file).on_hover_text(
                    "Full path of a .glb, .gltf or .obj file. A glTF rigged to the species' skeleton (bone names as in the game) animates fully; any other model follows the nearest bone.",
                );
            });
            egui::ComboBox::from_label("Species")
                .selected_text(if f.species.is_empty() { "choose" } else { f.species.as_str() })
                .show_ui(ui, |ui| {
                    for sp in f.list.iter().flatten() {
                        ui.selectable_value(&mut f.species, sp.clone(), sp);
                    }
                })
                .response
                .on_hover_text("The Pokémon whose look is replaced. Its skeleton, animations, sounds and behavior stay. Selecting its marker in the view picks it too.");
            if !f.species.is_empty() && f.objects.as_ref().map(|o| o.0 != f.species).unwrap_or(true) {
                let r = reskin::objects(&data.root, &f.species, "").map(|(_, objs)| objs.iter().map(|o| format!("{}  ({}, {} vertices)", o.name, o.material, o.vertices)).collect());
                f.objects = Some((f.species.clone(), r));
            }
            ui.horizontal(|ui| {
                ui.label("Keep");
                ui.add(egui::TextEdit::singleline(&mut f.keep).hint_text("mesh objects to keep, comma separated"))
                    .on_hover_text("Original mesh objects to leave as they are, such as the eyes. Every other object without a part of your model draws nothing.");
            });
            if let Some((_, objs)) = &f.objects {
                match objs {
                    Ok(lines) => {
                        egui::CollapsingHeader::new(format!("{} mesh objects", lines.len())).id_salt("reskin_objects").show(ui, |ui| {
                            for l in lines {
                                ui.weak(l);
                            }
                        });
                    }
                    Err(e) => {
                        ui.colored_label(egui::Color32::LIGHT_RED, e);
                    }
                }
            }
            ui.weak(format!("Writes into the pack \"{}\" in the Folder above.", self.pack_name));
            let ready = !f.file.trim().is_empty() && !f.species.is_empty();
            if ui
                .add_enabled(ready, egui::Button::new("Write the reskin"))
                .on_hover_text("Converts the model to the species' meshes (and its distance versions) and writes them into the pack")
                .on_disabled_hover_text("Fill in the model file and species first")
                .clicked()
            {
                acts.push(Action::Reskin(PathBuf::from(&self.export_dir)));
            }
            if let Some((ok, m)) = &f.msg {
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
            ui.selectable_value(&mut self.tool, Tool::Terrain, "Terrain")
                .on_hover_text("Drag over the ground to raise, lower, smooth or flatten it, or to paint its top texture layer (T)");
            if t0 != self.tool {
                self.stroke_end();
                self.ground_stroke_end();
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
        // the brush never lands on the sky dome or the backdrop
        let kinds: Vec<place::SurfaceKind> = if painting { self.site().kinds } else { vec![] };
        let surface_at = |me: &Self, at: egui::Pos2| {
            let (o, d) = pick::screen_ray(&me.cam, rect, at);
            me.ground.raycast_where(o, d, 1e5, &|m| kinds.get(m) != Some(&place::SurfaceKind::Skip))
        };
        if painting && resp.drag_started_by(egui::PointerButton::Primary) {
            // Scatter paints; Place leaves the drag to turn the camera
            let origin = ui.input(|i| i.pointer.press_origin()).unwrap_or(rect.center());
            if self.tool == Tool::Scatter {
                if let Some(h) = surface_at(self, origin) {
                    self.stroke_begin(h.point);
                }
            } else if self.tool == Tool::Terrain {
                if let Some(h) = surface_at(self, origin) {
                    self.ground_stroke_begin(h.point);
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
        } else if resp.dragged_by(egui::PointerButton::Primary) && self.gstroke.is_some() {
            if let Some(h) = resp.interact_pointer_pos().and_then(|at| surface_at(self, at)) {
                self.ground_dab(h.point);
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
            self.ground_stroke_end();
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
                    (Tool::Terrain, Some(h)) => {
                        self.ground_stroke_begin(h.point);
                        self.ground_stroke_end();
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
                if self.tool == Tool::Scatter || self.tool == Tool::Terrain {
                    let r = if self.tool == Tool::Terrain { self.ground_radius } else { self.brush.radius };
                    let pts: Vec<egui::Pos2> = (0..=40)
                        .filter_map(|i| {
                            let a = i as f32 / 40.0 * std::f32::consts::TAU;
                            pick::project(&self.cam, rect, h.point + Vec3::new(a.sin(), 0.0, a.cos()) * r)
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
            if self.brush_src.is_none() && self.tool != Tool::Terrain {
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
                    Tool::Terrain => "left drag: shape or paint the ground   Alt+drag: orbit   right drag: pan   wheel: zoom   S select   [ ] brush size",
                });
            });
        });
    }
}
