// Snap Recomp Studio viewer. three.js (vendored in ./vendor), no network at runtime.
import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { RoomEnvironment } from 'three/addons/environments/RoomEnvironment.js';

const $ = (s) => document.querySelector(s);
const cv = $('#cv');
const renderer = new THREE.WebGLRenderer({ canvas: cv, antialias: true, powerPreference: 'high-performance' });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFShadowMap;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
const pmrem = new THREE.PMREMGenerator(renderer);
const camera = new THREE.PerspectiveCamera(32, 1, 0.02, 100);
const controls = new OrbitControls(camera, cv);
controls.enableDamping = true;
controls.minDistance = 0.25;
controls.maxDistance = 12;
controls.maxPolarAngle = Math.PI * 0.5 - 0.02;
controls.autoRotateSpeed = 2.5;

const state = { manifest: null, decisions: [], filter: 'pending', sel: null, light: 'studio', view: 'lit', dist: 'mid', turn: false };
const side = { L: null, R: null };   // { scene, root, meshes, key }
const envCache = {};
const loader = new GLTFLoader();
let norm = { scale: 1, center: new THREE.Vector3(), height: 1 };   // from the original, applied to both sides

// ---------------------------------------------------------------- lighting presets
function skyScene(top, horizon, bottom, sun) {
  const s = new THREE.Scene();
  const mat = new THREE.ShaderMaterial({
    side: THREE.BackSide, depthWrite: false,
    uniforms: { top: { value: new THREE.Color(top) }, hor: { value: new THREE.Color(horizon) }, bot: { value: new THREE.Color(bottom) } },
    vertexShader: 'varying vec3 p; void main(){ p = normalize(position); gl_Position = projectionMatrix*modelViewMatrix*vec4(position,1.0); }',
    fragmentShader: 'uniform vec3 top; uniform vec3 hor; uniform vec3 bot; varying vec3 p; void main(){ float h = p.y; vec3 c = h>0.0 ? mix(hor, top, pow(h,0.6)) : mix(hor, bot, pow(-h,0.5)); gl_FragColor = vec4(c,1.0); }',
  });
  s.add(new THREE.Mesh(new THREE.SphereGeometry(50, 32, 16), mat));
  if (sun) {
    const m = new THREE.Mesh(new THREE.SphereGeometry(sun.size, 16, 8), new THREE.MeshBasicMaterial({ color: new THREE.Color(sun.color).multiplyScalar(sun.power) }));
    m.position.copy(sun.dir).normalize().multiplyScalar(45);
    s.add(m);
  }
  return s;
}
const PRESETS = {
  studio: { bg: 0x2b2e35, ground: 0x474b54, env: () => pmrem.fromScene(new RoomEnvironment(), 0.04).texture, envI: 0.9, exposure: 1.0,
    sun: { color: 0xffffff, i: 2.2, pos: [3, 5, 4] }, fill: { color: 0xbcd0ff, i: 0.6, pos: [-4, 2, -2] }, hemi: [0xffffff, 0x555a66, 0.2] },
  florio: { ground: 0x6f7f4c, env: () => pmrem.fromScene(skyScene(0x4d86d6, 0xcfe4f7, 0x6c7a4b, { dir: new THREE.Vector3(4, 6, 2), color: 0xfff0d0, power: 40, size: 3 }), 0.02).texture, envI: 1.0, exposure: 1.0,
    sun: { color: 0xfff1d6, i: 3.2, pos: [4, 6, 2] }, hemi: [0xbfdcff, 0x6b7a48, 0.45] },
  sunset: { ground: 0x4a3a36, env: () => pmrem.fromScene(skyScene(0x2a2f6e, 0xff8c4a, 0x2a1d22, { dir: new THREE.Vector3(5, 1.4, 3), color: 0xff9a52, power: 40, size: 4 }), 0.02).texture, envI: 0.9, exposure: 1.0,
    sun: { color: 0xff9448, i: 3.4, pos: [5, 1.6, 3] }, fill: { color: 0x6a74c8, i: 0.5, pos: [-4, 3, -3] }, hemi: [0x8a7ab0, 0x3a2a30, 0.3] },
  night: { ground: 0x1b212d, env: () => pmrem.fromScene(skyScene(0x03050f, 0x16204a, 0x070910, { dir: new THREE.Vector3(-3, 5, 2), color: 0xaecbff, power: 8, size: 1.5 }), 0.02).texture, envI: 0.7, exposure: 1.2,
    sun: { color: 0x9db6ff, i: 1.1, pos: [-3, 5, 2] }, hemi: [0x24305c, 0x0b0e18, 0.35] },
  cave: { ground: 0x2a2520, env: () => pmrem.fromScene(skyScene(0x050506, 0x0e0e10, 0x050505, null), 0.02).texture, envI: 0.35, exposure: 1.3,
    spot: { color: 0xffd9a8, i: 120, pos: [1.2, 5, 1.5] }, fill: { color: 0x4a7a96, i: 0.5, pos: [-3, 1.2, 2] }, hemi: [0x1c242c, 0x0a0907, 0.25] },
};
function getEnv(name) {
  if (!envCache[name]) envCache[name] = PRESETS[name].env();
  return envCache[name];
}
function makeScene(name) {
  const p = PRESETS[name];
  const sc = new THREE.Scene();
  const env = getEnv(name);
  sc.environment = env;
  sc.environmentIntensity = p.envI;
  if (p.bg !== undefined) sc.background = new THREE.Color(p.bg); else { sc.background = env; sc.backgroundBlurriness = 0.25; }
  sc.userData.exposure = p.exposure;
  if (p.hemi) sc.add(new THREE.HemisphereLight(p.hemi[0], p.hemi[1], p.hemi[2]));
  const shadowCam = (l) => {
    l.castShadow = true; l.shadow.mapSize.set(2048, 2048); l.shadow.radius = 5; l.shadow.bias = -0.0004; l.shadow.normalBias = 0.015;
    const c = l.shadow.camera; if (c.isOrthographicCamera) { c.left = -2.4; c.right = 2.4; c.top = 2.4; c.bottom = -2.4; c.near = 0.1; c.far = 20; }
  };
  if (p.sun) { const l = new THREE.DirectionalLight(p.sun.color, p.sun.i); l.position.set(...p.sun.pos); shadowCam(l); sc.add(l); }
  if (p.spot) { const l = new THREE.SpotLight(p.spot.color, p.spot.i, 0, 0.5, 0.6, 1.6); l.position.set(...p.spot.pos); l.target.position.set(0, 0.4, 0); shadowCam(l); l.shadow.camera.near = 0.5; l.shadow.camera.far = 14; sc.add(l, l.target); }
  if (p.fill) { const l = new THREE.DirectionalLight(p.fill.color, p.fill.i); l.position.set(...p.fill.pos); sc.add(l); }
  const g = new THREE.Mesh(new THREE.CircleGeometry(9, 64), new THREE.MeshStandardMaterial({ color: p.ground, roughness: 0.95, metalness: 0 }));
  g.rotation.x = -Math.PI / 2; g.receiveShadow = true; g.name = 'ground'; sc.add(g);
  return sc;
}

// ---------------------------------------------------------------- view-mode materials
function roughMat(src) {
  const m = new THREE.MeshBasicMaterial({ map: src.roughnessMap || null, side: THREE.DoubleSide });
  const flat = src.roughnessMap ? 0 : src.roughness;
  m.onBeforeCompile = (sh) => {
    sh.fragmentShader = sh.fragmentShader.replace('#include <map_fragment>',
      '#include <map_fragment>\n float rr = ' + (src.roughnessMap ? 'diffuseColor.g' : flat.toFixed(3)) + '; diffuseColor.rgb = vec3(pow(rr, 2.2));');
  };
  m.customProgramCacheKey = () => (src.roughnessMap ? 'rm' : 'rf' + flat);
  return m;
}
function modeMaterial(mesh, view) {
  const src = mesh.userData.lit;
  const c = mesh.userData.cache;
  if (view === 'lit') return src;
  if (c[view]) return c[view];
  let m;
  if (view === 'albedo') m = new THREE.MeshBasicMaterial({ map: src.map || null, color: src.map ? 0xffffff : src.color, side: THREE.DoubleSide, transparent: src.transparent, alphaTest: src.alphaTest });
  else if (view === 'normals') m = new THREE.MeshNormalMaterial({ normalMap: src.normalMap || null, normalScale: src.normalScale, side: THREE.DoubleSide });
  else if (view === 'roughness') m = roughMat(src);
  else m = new THREE.MeshStandardMaterial({ color: 0x3a3f49, roughness: 0.85, metalness: 0, side: THREE.DoubleSide, polygonOffset: true, polygonOffsetFactor: 1, polygonOffsetUnits: 1 });
  c[view] = m;
  return m;
}
const wireMat = new THREE.MeshBasicMaterial({ color: 0xbfe3ff, wireframe: true, transparent: true, opacity: 0.55, depthWrite: false });
function wireOverlay(m) {
  if (m.userData.wire) return m.userData.wire;
  const w = m.isSkinnedMesh ? new THREE.SkinnedMesh(m.geometry, wireMat) : new THREE.Mesh(m.geometry, wireMat);
  if (m.isSkinnedMesh) w.bind(m.skeleton, m.bindMatrix);
  w.position.copy(m.position); w.quaternion.copy(m.quaternion); w.scale.copy(m.scale);
  w.frustumCulled = false; w.renderOrder = 2;
  m.parent.add(w);
  m.userData.wire = w;
  return w;
}
function applyView(s) {
  if (!s) return;
  for (const m of s.meshes) {
    m.material = modeMaterial(m, state.view);
    if (state.view === 'wire') wireOverlay(m).visible = true; else if (m.userData.wire) m.userData.wire.visible = false;
  }
}

// ---------------------------------------------------------------- model loading
function disposeSide(s) {
  if (!s) return;
  s.scene.traverse((o) => {
    if (o.isMesh && o.userData.lit) {
      o.geometry.dispose();
      const l = o.userData.lit;
      for (const k of Object.keys(l)) if (l[k] && l[k].isTexture) l[k].dispose();
      l.dispose();
      for (const v of Object.values(o.userData.cache)) v.dispose();
    }
  });
}
function loadGltf(path) {
  return new Promise((res, rej) => loader.load('/' + path, res, undefined, rej));
}
async function buildSide(entry, lightName) {
  const gl = await loadGltf(entry.path);
  const root = gl.scene;
  const meshes = [];
  let tris = 0;
  root.traverse((o) => {
    if (o.isMesh) {
      o.frustumCulled = false;
      o.castShadow = true; o.receiveShadow = true;
      o.userData.lit = o.material;
      o.userData.cache = {};
      for (const k of ['map', 'normalMap', 'roughnessMap', 'aoMap', 'emissiveMap']) if (o.material[k]) o.material[k].anisotropy = renderer.capabilities.getMaxAnisotropy();
      o.material.side = THREE.DoubleSide;
      meshes.push(o);
      tris += (o.geometry.index ? o.geometry.index.count : o.geometry.attributes.position.count) / 3;
    }
  });
  const scene = makeScene(lightName);
  const holder = new THREE.Group();
  holder.add(root);
  scene.add(holder);
  return { scene, holder, root, meshes, tris, key: entry.path, light: lightName };
}
function fitNorm(s) {
  s.holder.updateMatrixWorld(true);
  const box = new THREE.Box3().setFromObject(s.root, true);
  const size = box.getSize(new THREE.Vector3());
  const sph = box.getBoundingSphere(new THREE.Sphere());
  const scale = 1 / Math.max(sph.radius, 1e-4);
  norm = { scale, center: new THREE.Vector3((box.min.x + box.max.x) / 2, box.min.y, (box.min.z + box.max.z) / 2), height: size.y * scale, midY: (box.min.y + box.max.y) / 2 };
}
function placeSide(s) {
  s.holder.scale.setScalar(norm.scale);
  s.holder.position.set(-norm.center.x * norm.scale, -norm.center.y * norm.scale, -norm.center.z * norm.scale);
}
function setMsg(t) { const m = $('#msg'); m.hidden = !t; m.textContent = t || ''; }

let loadToken = 0;
async function showSelection() {
  const c = state.sel;
  if (!c) return;
  const token = ++loadToken;
  const sp = state.manifest.species[c.species];
  setMsg('Loading ' + sp.name + ' ' + c.variant + ' ...');
  try {
    const needL = !side.L || side.L.key !== sp.original.path || side.L.light !== state.light;
    const [L, R] = await Promise.all([
      needL ? buildSide(sp.original, state.light) : Promise.resolve(side.L),
      buildSide(c, state.light),
    ]);
    if (token !== loadToken) { if (needL) disposeSide(L); disposeSide(R); return; }
    if (needL && side.L) disposeSide(side.L);
    if (side.R) disposeSide(side.R);
    side.L = L; side.R = R;
    fitNorm(L);
    placeSide(L); placeSide(R);
    applyView(L); applyView(R);
    $('#tagL').innerHTML = '<b>Original</b> ' + sp.name + '<br>' + L.tris.toLocaleString() + ' triangles';
    $('#tagR').innerHTML = '<b>' + (c.label || c.variant) + '</b> (' + c.variant + ')<br>' + R.tris.toLocaleString() + ' triangles' + (R.tris !== L.tris ? ' (x' + (R.tris / L.tris).toFixed(1) + ')' : '');
    setMsg('');
    if (!state.camSet || state.camSpecies !== c.species) { resetCamera(); state.camSet = true; state.camSpecies = c.species; }
    renderNotes();
  } catch (e) {
    console.error(e);
    setMsg('Load failed: ' + (e.message || e));
  }
}
function relight() {
  // scenes are cheap: rebuild both sides' scene around the same models
  for (const k of ['L', 'R']) {
    const s = side[k];
    if (!s) continue;
    s.scene.remove(s.holder);
    s.scene = makeScene(state.light);
    s.scene.add(s.holder);
    s.light = state.light;
  }
}

// ---------------------------------------------------------------- camera
function resetCamera() {
  const close = state.dist === 'close';
  const d = close ? 1.25 : 3.6;
  const ty = norm.height * (close ? 0.62 : 0.5);
  controls.target.set(0, ty, 0);
  camera.position.set(0.55 * d, ty + 0.22 * d, 0.82 * d);
  controls.update();
}
function setDist(d) {
  state.dist = d;
  document.querySelectorAll('[data-d]').forEach((b) => b.classList.toggle('on', b.dataset.d === d));
  resetCamera();
}

// ---------------------------------------------------------------- render loop
function resize() {
  const w = cv.clientWidth, h = cv.clientHeight;
  renderer.setSize(w, h, false);
  camera.aspect = (w / 2) / h;
  camera.updateProjectionMatrix();
}
window.addEventListener('resize', resize);
function frame() {
  requestAnimationFrame(frame);
  controls.autoRotate = state.turn;
  controls.update();
  const w = cv.clientWidth, h = cv.clientHeight;
  renderer.setScissorTest(true);
  for (const [k, x] of [['L', 0], ['R', w / 2]]) {
    const s = side[k];
    renderer.setViewport(x, 0, w / 2, h);
    renderer.setScissor(x, 0, w / 2, h);
    renderer.toneMappingExposure = s ? s.scene.userData.exposure : 1;
    if (s) renderer.render(s.scene, camera); else renderer.clear();
  }
}

// ---------------------------------------------------------------- list, decisions
function latestDecision(sp, v) {
  for (let i = state.decisions.length - 1; i >= 0; i--) { const d = state.decisions[i]; if (d.species === sp && d.variant === v) return d; }
  return null;
}
function keyOf(c) { return c.species + '/' + c.variant; }
function renderList() {
  const list = $('#list');
  list.innerHTML = '';
  const items = state.manifest.candidates.filter((c) => {
    const d = latestDecision(c.species, c.variant);
    return state.filter === 'all' || (state.filter === 'pending' ? !d : !!d);
  });
  if (!items.length) list.innerHTML = '<div class="item"><span class="m">Nothing here.</span></div>';
  for (const c of items) {
    const d = latestDecision(c.species, c.variant);
    const sp = state.manifest.species[c.species];
    const el = document.createElement('div');
    el.className = 'item' + (state.sel && keyOf(state.sel) === keyOf(c) ? ' sel' : '');
    el.innerHTML = '<div class="n">' + esc(sp ? sp.name : c.species) + ' <span class="m">' + esc(c.kind) + '</span> <span class="badge b-' + (d ? d.verdict : 'pending') + '">' + (d ? d.verdict : 'pending') + '</span></div>' +
      '<div class="m">' + esc(c.label || c.variant) + '</div><div class="m">' + esc(c.source_chat) + ' | ' + esc((c.date || '').replace('T', ' ').slice(0, 16)) + ' | ' + c.tris.toLocaleString() + ' tris</div>';
    el.onclick = () => select(c);
    list.appendChild(el);
  }
  state.visible = items;
}
function esc(s) { return String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }
function renderNotes() {
  const c = state.sel;
  if (!c) return;
  const sp = state.manifest.species[c.species];
  const maps = (c.materials || []).map((m) => m.material.replace(/^MI_/, '') + ': ' + Object.entries(m.maps).map(([k, v]) => k + '=' + v).join(', ')).join(' | ');
  $('#notes').innerHTML = '<b>' + esc(sp.name) + ' / ' + esc(c.variant) + '</b> from ' + esc(c.source_chat) + ', ' + esc(c.date) + '<br>' + esc(c.notes || '') + '<br><span title="material: source of each map">Maps: ' + esc(maps) + '</span>';
  const h = state.decisions.filter((d) => d.species === c.species && d.variant === c.variant).reverse();
  $('#hist').innerHTML = h.length ? 'History: ' + h.map((d) => esc(d.verdict) + ' (' + esc(d.timestamp.replace('T', ' ').slice(0, 16)) + ')' + (d.comment ? ' "' + esc(d.comment) + '"' : '')).join('; ') : '';
}
function select(c) {
  state.sel = c;
  $('#comment').value = '';
  $('#saved').textContent = '';
  renderList();
  showSelection();
  renderNotes();
}
async function decide(verdict) {
  const c = state.sel;
  if (!c) return;
  const body = { species: c.species, variant: c.variant, verdict, comment: $('#comment').value };
  const r = await fetch('/api/decision', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  if (!r.ok) { $('#saved').style.color = 'var(--bad)'; $('#saved').textContent = 'Save failed: ' + r.status; return; }
  const j = await r.json();
  state.decisions.push(j.decision);
  $('#saved').style.color = 'var(--ok)';
  $('#saved').textContent = 'Saved: ' + verdict;
  $('#comment').value = '';
  renderNotes();
  // move on to the next pending item when filtering by pending
  const keep = state.sel;
  renderList();
  if (state.filter === 'pending' && state.visible.length) { /* stay on this item; it leaves the pending list on next filter change */ }
  state.sel = keep;
}

// ---------------------------------------------------------------- wiring
function wire() {
  document.querySelectorAll('[data-f]').forEach((b) => (b.onclick = () => {
    state.filter = b.dataset.f;
    document.querySelectorAll('[data-f]').forEach((x) => x.classList.toggle('on', x === b));
    renderList();
  }));
  document.querySelectorAll('[data-v]').forEach((b) => (b.onclick = () => {
    state.view = b.dataset.v;
    document.querySelectorAll('[data-v]').forEach((x) => x.classList.toggle('on', x === b));
    applyView(side.L); applyView(side.R);
  }));
  document.querySelectorAll('[data-d]').forEach((b) => (b.onclick = () => setDist(b.dataset.d)));
  document.querySelectorAll('[data-verdict]').forEach((b) => (b.onclick = () => decide(b.dataset.verdict)));
  $('#light').onchange = (e) => { state.light = e.target.value; relight(); applyView(side.L); applyView(side.R); };
  $('#turn').onclick = () => { state.turn = !state.turn; $('#turn').textContent = 'Turntable: ' + (state.turn ? 'on' : 'off'); $('#turn').classList.toggle('on', state.turn); };
  $('#reset').onclick = resetCamera;
  window.addEventListener('keydown', (e) => {
    if (e.target.tagName === 'TEXTAREA' || e.target.tagName === 'SELECT') return;
    const v = state.visible || [];
    const i = v.findIndex((c) => state.sel && keyOf(c) === keyOf(state.sel));
    if (e.key === 'ArrowDown' && v[i + 1]) select(v[i + 1]);
    if (e.key === 'ArrowUp' && v[i - 1]) select(v[i - 1]);
  });
}

async function init() {
  resize();
  wire();
  const [m, d] = await Promise.all([fetch('/manifest.json').then((r) => r.json()), fetch('/decisions.json').then((r) => r.json())]);
  state.manifest = m;
  state.decisions = d;
  m.candidates.sort((a, b) => (b.date || '').localeCompare(a.date || ''));
  renderList();
  const q = new URLSearchParams(location.search);
  if (q.get('light') && PRESETS[q.get('light')]) { state.light = q.get('light'); $('#light').value = state.light; }
  if (q.get('view')) document.querySelector('[data-v="' + q.get('view') + '"]')?.click();
  if (q.get('dist')) setDist(q.get('dist'));
  if (q.get('turn') === '1') $('#turn').click();
  const want = q.get('sel') && m.candidates.find((c) => keyOf(c) === q.get('sel'));
  const first = want || (state.visible || [])[0] || m.candidates[0];
  if (first) select(first); else setMsg('No candidates yet. Run scripts/studio/export_studio.py.');
  requestAnimationFrame(frame);
  window.__studio = { state, side, renderer, camera, controls };   // for headless checks
}
init().catch((e) => { console.error(e); setMsg('Init failed: ' + e.message); });
