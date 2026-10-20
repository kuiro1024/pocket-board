/* =====================================================================
   サイト上で触れる PocketBoard（デモ）— 本体のソースに合わせた挙動
   - 描画: GlassHighlightEffect の GLASS_FRAG を WebGL2 に移植（プロモ動画と同じ）
   - 外からドロップ: その場にそのまま置く（出現アニメ無し）。サイトは Web リンクのランチャー（favicon ／ 頭文字の丸）
   - 左ドラッグ: バブル移動（持ち上げ 1.025）／右下の印でリサイズ／空白は範囲選択
   - クリック: アクティブ化（青い枠が脈打つ）。動画は再生/一時停止（280ms 待ってダブルクリックでなければ）
   - ダブルクリック: 全画面（ウィンドウが中身の対比へ 500ms InOutCubic、カメラも同時）。もう一度で戻る（300ms）
   - ダブルクリックの2回目を押したまま動かす／Shift+ドラッグ: 逆輸入（PBの外へ書き出し）。キャンバスが暗くなる
   - 中ボタンドラッグ: キャンバス移動（慣性あり）／ホイール: ズーム（1コリ10%、なめらかに）
   - 右クリック: アクティブ化のみ／右ドラッグ: ウィンドウ移動／右ダブルクリック: 全画面
   - 島: 上中央の 300×80 にカーソルが来たら出る。操作中は隠れる
   ===================================================================== */
var GLASS_DARK_K = .45, GUMMY_AMBIENT_LIFT = .45;
const MEDIA_BEZEL_RATIO = 25 / 162, LAUNCHER_BEZEL_RATIO = 25 / 96, MEDIA_SHORT = 162;
const SHADOW_K = .35, CONTACT_SHADOW_K = .7, GUMMY_SHADOW_K = .16, GUMMY_CONTACT_K = .45;
const ACCENT = '#0067C0';
const E = {
  inOut: x => x < .5 ? 4 * x * x * x : 1 - Math.pow(-2 * x + 2, 3) / 2,
  out: x => 1 - Math.pow(1 - x, 3),
  outQuart: x => 1 - Math.pow(1 - x, 4),
  outQuint: x => 1 - Math.pow(1 - x, 5),
  inOutSine: x => -(Math.cos(Math.PI * x) - 1) / 2,
  outBack: (x, a = 1.70158) => { const c3 = a + 1; return x >= 1 ? 1 : 1 + c3 * Math.pow(x - 1, 3) + a * Math.pow(x - 1, 2); },
};
const clamp = (x, a = 0, b = 1) => Math.min(b, Math.max(a, x));
const lerp = (a, b, k) => a + (b - a) * k;
const SVG_NOW_PLAY = '<svg viewBox="0 0 32 32"><rect x="10.916" y="9.805" width="3.39" height="12.39" rx="1.432" fill="#fff"/><rect x="17.695" y="9.805" width="3.39" height="12.39" rx="1.432" fill="#fff"/></svg>';
const SVG_NOW_STOP = '<svg viewBox="0 0 32 32"><path d="M12.662,10.5l7.965,4.599c.694.401.694,1.402,0,1.802l-7.965,4.599c-.694.401-1.561-.1-1.561-.901v-9.197c0-.801.867-1.302,1.561-.901Z" fill="#fff"/></svg>';
const SVG_SOUND = `<svg viewBox="0 0 32 32">${SOUND3_PATHS.map(d => `<path d="${d}" fill="#fff"/>`).join('')}</svg>`;
const svgUrl = (svg, w, h) => 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svg.replace(/^<svg /, `<svg xmlns="http://www.w3.org/2000/svg" width="${Math.round(w)}" height="${Math.round(h)}" `));
const loadImg = url => { const im = new Image(); im.src = url; return im.decode().then(() => im).catch(() => im); };
const loadCanvas = url => new Promise((ok, ng) => { const im = new Image(); im.onload = () => { const c = document.createElement('canvas'); c.width = im.naturalWidth; c.height = im.naturalHeight; c.getContext('2d').drawImage(im, 0, 0); ok(c); }; im.onerror = () => ng(new Error('load failed: ' + url)); im.src = url; });
const rasterSvg = (svg, w, h) => loadImg(svgUrl(svg, w, h)).then(im => { const c = document.createElement('canvas'); c.width = Math.round(w); c.height = Math.round(h); c.getContext('2d').drawImage(im, 0, 0, c.width, c.height); return c; });
function labelCanvas(text, rgba) {
  const S = 6, px = 14 * S, c = document.createElement('canvas'), x = c.getContext('2d');
  x.font = `700 ${px}px Arial, "Segoe UI", sans-serif`; const w = Math.ceil(x.measureText(text).width) + 8;
  c.width = w; c.height = Math.ceil(px * 1.35);
  x.font = `700 ${px}px Arial, "Segoe UI", sans-serif`; x.textBaseline = 'top'; x.fillStyle = rgba; x.fillText(text, 4, px * .12);
  return c;
}
// Web リンクの favicon が無いときの代わり（WebLinkIcon::fallbackIcon と同じ: ホスト名の色相の丸＋頭文字）
function webFallbackIcon(host, size = 256) {
  let h = 0; for (const ch of host) h = (h * 31 + ch.charCodeAt(0)) >>> 0;
  const c = document.createElement('canvas'); c.width = c.height = size; const x = c.getContext('2d');
  x.fillStyle = `hsl(${h % 360} ${Math.round(150 / 255 * 100)}% ${Math.round(105 / 255 * 100)}%)`;
  x.beginPath(); x.arc(size / 2, size / 2, size * .44, 0, Math.PI * 2); x.fill();
  x.fillStyle = 'rgba(255,255,255,.92)'; x.font = `700 ${size * .5}px Arial, sans-serif`; x.textAlign = 'center'; x.textBaseline = 'middle';
  x.fillText(host[0].toUpperCase(), size / 2, size / 2 + size * .03);
  return c;
}
function siteThumb() {   // 左のブラウザに出す見た目（架空のギャラリーサイト）
  const c = document.createElement('canvas'); c.width = 640; c.height = 400; const x = c.getContext('2d');
  x.fillStyle = '#12151f'; x.fillRect(0, 0, 640, 400);
  x.fillStyle = '#fff'; x.font = '700 34px sans-serif'; x.fillText('Lumen Gallery', 32, 62);
  x.fillStyle = '#7d86a6'; x.font = '400 16px sans-serif'; x.fillText('Light, color and form', 32, 92);
  const g1 = x.createLinearGradient(32, 120, 300, 370); g1.addColorStop(0, '#2fb9a6'); g1.addColorStop(1, '#0b3a44');
  x.fillStyle = g1; x.beginPath(); x.roundRect(32, 120, 280, 248, 14); x.fill();
  x.fillStyle = '#f06fa8'; x.beginPath(); x.arc(240, 196, 38, 0, 7); x.fill(); x.fillStyle = '#c6fff3'; x.beginPath(); x.arc(130, 290, 56, 0, 7); x.fill();
  const g2 = x.createLinearGradient(330, 120, 608, 240); g2.addColorStop(0, '#5b3fd6'); g2.addColorStop(1, '#e0557c');
  x.fillStyle = g2; x.beginPath(); x.roundRect(328, 120, 280, 118, 14); x.fill();
  x.fillStyle = '#ffb46a'; x.beginPath(); x.roundRect(328, 250, 134, 118, 14); x.fill();
  x.fillStyle = '#26304a'; x.beginPath(); x.roundRect(474, 250, 134, 118, 14); x.fill();
  return c;
}

/* 入れられる素材（中身の短辺は本体の基準 162px） */
const SOURCES = {
  site:    {kind: 'tile', label: 'lumen.gallery', web: true},
  sunset:  {kind: 'image', label: 'sunset.png', ar: 4 / 3, make: () => loadCanvas('assets/art_sunset.jpg')},
  spheres: {kind: 'image', label: 'spheres.jpg', ar: 4 / 3, make: () => loadCanvas('assets/art_spheres.jpg')},
  ocean:   {kind: 'video', label: 'ocean.mp4', ar: 16 / 9, paint: 'vid1', dur: 72},
  orbs:    {kind: 'video', label: 'orbs.mp4', ar: 16 / 9, paint: 'vid2', dur: 48},
  folder:  {kind: 'tile', label: 'Assets', icon: 'folder'},
  notes:   {kind: 'tile', label: 'notes.txt', icon: 'notes'},
  app:     {kind: 'tile', label: 'Studio', icon: 'exe'},
};
const PAINT = {vid1: paintVid1, vid2: paintVid2};

function initDemo() {
  const demoEl = document.getElementById('demo'); if (!demoEl) return;
  const section = demoEl.closest('section');
  const pbEl = demoEl.querySelector('.pb'), host = demoEl.querySelector('.pb-canvas'), pbBody = demoEl.querySelector('.pb-body');
  const uiCv = demoEl.querySelector('.pb-ui'), uiX = uiCv.getContext('2d');
  const islandEl = demoEl.querySelector('.pb-island'), islandCv = islandEl.querySelector('canvas'), islandBlur = islandEl.querySelector('.iblur');
  const recall = demoEl.querySelector('.pb-recall');
  let g = null; try { g = makeGLR(); } catch (e) { g = null; console.error(e); window.__glErr = e; }
  if (!g) { demoEl.classList.add('no-gl'); showDemoError(demoEl, window.__glErr || new Error('WebGL2 が使えません')); return; }
  host.appendChild(g.cv);

  /* ---------- 状態 ---------- */
  let W = 1, H = 1, DPR = 1;
  let style = window.PB_STYLE || 'gummy', styleChangedAt = -10, visible = true;
  const BUB = [], selected = new Set();
  const cam = {cx: 0, cy: 0, z: 1}; let zoomTarget = null, zoomAnchor = null; const inertia = {vx: 0, vy: 0, on: false, sampleT: 0};   // CameraPanInertia（キャンバス単位/秒）
  let active = null;               // アクティブなバブル（青い枠・再生バーの持ち主）
  let focus = null;                // {b, t0, dir, from:{cx,cy,z,w,h}, to:{...}}
  let focusAmt = 0, glassF = 1;    // 全画面の進み具合 / ガラスの濃さ（全画面中は消える）
  let exportHint = 0, exportHintTarget = 0, exportFrom = 0, exportT0 = 0, exportWant = 0;
  let mode = '', press = null, pendingClick = null;
  let rect = null;                 // 範囲選択
  let TEX_STOP, TEX_PLAY;
  let lastT = performance.now() / 1000, mouseLocal = null, lastMoveT = 0;
  const now = () => performance.now() / 1000;
  const baseSize = {w: 0, h: 0};   // 全画面前のウィンドウサイズ（キャンバス固定の挙動で戻す）

  /* ---------- サイズ ---------- */
  function applySize() {
    const r = pbBody.getBoundingClientRect(); W = Math.max(1, r.width); H = Math.max(1, r.height);
    DPR = Math.min(2, devicePixelRatio || 1); g.resize(W, H, DPR); g.cv.style.width = W + 'px'; g.cv.style.height = H + 'px';
    uiCv.width = Math.round(W * DPR); uiCv.height = Math.round(H * DPR); uiCv.style.width = W + 'px'; uiCv.style.height = H + 'px';
  }
  new ResizeObserver(() => { if (!focus) applySize(); kick(); }).observe(pbBody);

  /* ---------- バブル ---------- */
  async function makeBubble(key, x, y) {
    const S = SOURCES[key], b = {key, kind: S.kind, label: S.label, x, y, scale: 1, lift: 1, liftAnim: null, playing: false, vt: 0, playAt: -10, dur: S.dur || 0};
    if (S.kind === 'tile') {
      b.cw0 = b.ch0 = 96;
      const ic = S.web ? webFallbackIcon(S.label) : await loadCanvas(`assets/icon_${S.icon}.png`);
      b.tex = g.texFrom(ic, {mip: true}); b.small = g.makeSmall(ic, 0, 0, ic.width, ic.height); b.thumb = ic;
      const lw = labelCanvas(S.label, 'rgba(245,247,252,.941)'), lg = labelCanvas(S.label, 'rgba(70,70,70,.941)');
      b.labelW = g.texFrom(lw); b.labelG = g.texFrom(lg); b.labelAR = lw.width / lw.height; b.labelPxW = lw.width / 6;
    } else {
      b.ch0 = MEDIA_SHORT; b.cw0 = MEDIA_SHORT * S.ar;
      if (S.kind === 'video') {
        b.paint = PAINT[S.paint]; b.vcv = document.createElement('canvas'); b.vcv.width = Math.round(b.cw0 * 3); b.vcv.height = Math.round(b.ch0 * 3);
        b.paint(b.vcv, .8); b.tex = g.texFrom(b.vcv); b.small = g.makeSmall(b.vcv, 0, 0, b.vcv.width, b.vcv.height); b.smallKey = -1; b.thumb = b.vcv;
      } else {
        const c = await S.make(); b.tex = g.texFrom(c, {mip: true}); b.small = g.makeSmall(c, 0, 0, c.width, c.height); b.thumb = c;
      }
    }
    return b;
  }
  const cw = b => b.cw0 * b.scale, ch = b => b.ch0 * b.scale;
  const bezOf = b => Math.min(cw(b), ch(b)) * (b.kind === 'tile' ? LAUNCHER_BEZEL_RATIO : MEDIA_BEZEL_RATIO);
  const boardOf = b => { const z = bezOf(b); return {x0: b.x - cw(b) / 2 - z, y0: b.y - ch(b) / 2 - z, x1: b.x + cw(b) / 2 + z, y1: b.y + ch(b) / 2 + z}; };
  const toWorld = (sx, sy) => ({x: cam.cx + (sx - W / 2) / cam.z, y: cam.cy + (sy - H / 2) / cam.z});
  const toScreen = (wx, wy) => ({x: W / 2 + (wx - cam.cx) * cam.z, y: H / 2 + (wy - cam.cy) * cam.z});
  function hit(sx, sy) {
    const p = toWorld(sx, sy);
    for (let i = BUB.length - 1; i >= 0; i--) {
      const b = BUB[i], r = boardOf(b), bz = bezOf(b);
      if (p.x >= r.x1 - bz * 1.3 && p.x <= r.x1 + 4 / cam.z && p.y >= r.y1 - bz * 1.3 && p.y <= r.y1 + 4 / cam.z) return {b, handle: true};
      if (p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1) return {b, handle: false};
    }
    return null;
  }
  const raise = b => { BUB.splice(BUB.indexOf(b), 1); BUB.push(b); };
  function setLift(b, to) { b.liftAnim = {from: b.lift, to, t0: now(), dur: .25, ease: to > 1 ? E.outQuart : E.outQuint}; }
  function activate(b) {
    if (active === b) return;
    if (active) { active.lift = 1; active.liftAnim = null; }
    active = b;
    if (b && b.kind === 'video') { bar.popT0 = now(); bar.owner = b; }
  }


  /* ---------- 重なりの押し出し（CanvasWidget::computeOverlapPushOffsets / animateItemOffsets） ---------- */
  const pushAnims = new Map();
  function pushOverlaps(fixedList) {
    const fixed = new Set(fixedList), off = new Map(), rank = new Map(), base = new Map(), UNR = 1e9;
    BUB.forEach(b => { base.set(b, boardOf(b)); off.set(b, {x: 0, y: 0}); rank.set(b, fixed.has(b) ? 0 : UNR); });
    for (let iter = 0; iter < 64; iter++) {
      let any = false;
      for (let i = 0; i < BUB.length; i++) for (let j = i + 1; j < BUB.length; j++) {
        const a = BUB[i], b = BUB[j]; if (fixed.has(a) && fixed.has(b)) continue;
        const oa = off.get(a), ob = off.get(b), ra = base.get(a), rb = base.get(b);
        const ax0 = ra.x0 + oa.x, ax1 = ra.x1 + oa.x, ay0 = ra.y0 + oa.y, ay1 = ra.y1 + oa.y, bx0 = rb.x0 + ob.x, bx1 = rb.x1 + ob.x, by0 = rb.y0 + ob.y, by1 = rb.y1 + ob.y;
        const ox = Math.min(ax1, bx1) - Math.max(ax0, bx0), oy = Math.min(ay1, by1) - Math.max(ay0, by0);
        if (ox <= 0 || oy <= 0) continue; any = true;
        const dx = (bx0 + bx1) / 2 - (ax0 + ax1) / 2, dy = (by0 + by1) / 2 - (ay0 + ay1) / 2;
        const d = ox < oy ? {x: (dx >= 0 ? 1 : -1) * (ox + .5), y: 0} : {x: 0, y: (dy >= 0 ? 1 : -1) * (oy + .5)};
        const rA = rank.get(a), rB = rank.get(b);
        if (rA < rB) { ob.x += d.x; ob.y += d.y; rank.set(b, rA + 1); }
        else if (rB < rA) { oa.x -= d.x; oa.y -= d.y; rank.set(a, rB + 1); }
        else { oa.x -= d.x / 2; oa.y -= d.y / 2; ob.x += d.x / 2; ob.y += d.y / 2; }
      }
      if (!any) break;
    }
    off.forEach((o, b) => { if (fixed.has(b) || (Math.abs(o.x) < .01 && Math.abs(o.y) < .01)) return;
      pushAnims.set(b, {t0: now(), fx: b.x, fy: b.y, tx: b.x + o.x, ty: b.y + o.y}); });
    kick();
  }
  function stepPush(t) {
    let any = false;
    pushAnims.forEach((a, b) => { const k = clamp((t - a.t0) / .42), e = E.outBack(k); b.x = lerp(a.fx, a.tx, e); b.y = lerp(a.fy, a.ty, e); if (k >= 1) pushAnims.delete(b); else any = true; });
    return any;
  }

  /* ---------- 島 ---------- */
  const isl = {showing: false, w: 42, wFrom: 42, wTo: 42, wT0: -10, op: 0, opFrom: 0, opT0: -10, lockUntil: 0,
    btn: [0, 0, 0, 0], btnT0: [-1, -1, -1, -1], bounce: [1, 1, 1, 1], bounceFrom: 1, hoverIdx: -1, hoverT0: 0, leaveIdx: -1, leaveT0: 0, key: ''};
  const islDelay = [0, 140, 70, 0];   // 設定・最小化・最大化・閉じる（設定と閉じるが先、最大化は70ms、最小化は140ms遅れ）
  function showIsland(t) {
    isl.showing = true; isl.lockUntil = t + .3; isl.op = 1;
    isl.wFrom = isl.w; isl.wTo = 240; isl.wT0 = t; isl.btnT0 = islDelay.map(d => t + d / 1000);
  }
  function hideIsland(t) {
    isl.showing = false; isl.lockUntil = t + .3; isl.btnT0 = [-1, -1, -1, -1]; isl.btn = [0, 0, 0, 0]; isl.bounce = [1, 1, 1, 1];
    isl.wFrom = isl.w; isl.wTo = 42; isl.wT0 = t; isl.opFrom = isl.op; isl.opT0 = t + .05;
  }
  const islandRect = () => ({x: W / 2 - 120, y: 15, w: 240, h: 42});
  const islBtnX = [21, 240 - 95, 240 - 58, 240 - 21];
  function islandButtonAt(sx, sy) {
    if (!isl.showing || isl.op < .5) return -1; const r = islandRect();
    for (let i = 0; i < 4; i++) if (Math.hypot(sx - (r.x + islBtnX[i]), sy - (r.y + 21)) <= 13) return i;
    return -1;
  }
  function updateIsland(t) {
    const manip = mode === 'move' || mode === 'pan' || mode === 'rect' || mode === 'resize' || mode === 'window' || mode === 'export';
    if (manip) { if (isl.showing && t >= isl.lockUntil) hideIsland(t); }
    else if (t >= isl.lockUntil && mouseLocal) {
      const inTrigger = Math.abs(mouseLocal.x - W / 2) <= 150 && mouseLocal.y >= 0 && mouseLocal.y <= 80;
      const r = islandRect(), onIsland = mouseLocal.x >= r.x - 30 && mouseLocal.x <= r.x + r.w + 30 && mouseLocal.y >= r.y - 30 && mouseLocal.y <= r.y + r.h + 30;
      if (inTrigger && !isl.showing) showIsland(t);
      else if (!inTrigger && !onIsland && isl.showing) hideIsland(t);
    } else if (!mouseLocal && isl.showing && t >= isl.lockUntil) hideIsland(t);
    // アニメ（幅 500ms OutBack、ボタン 600ms OutBack(振幅2)、消えるときは50ms後に120msでフェード）
    const kw = clamp((t - isl.wT0) / .5); isl.w = lerp(isl.wFrom, isl.wTo, E.outBack(kw));
    if (!isl.showing) { const ko = clamp((t - isl.opT0) / .12); isl.op = t < isl.opT0 ? isl.opFrom : lerp(isl.opFrom, 0, ko); }
    for (let i = 0; i < 4; i++) {
      if (isl.btnT0[i] >= 0) { const k = clamp((t - isl.btnT0[i]) / .6); isl.btn[i] = t < isl.btnT0[i] ? 0 : E.outBack(k, 1.70158 * 2); }
      if (isl.hoverIdx === i) { const k = clamp((t - isl.hoverT0) / .15); isl.bounce[i] = lerp(isl.bounceFrom, 1.15, 1 - (1 - k) * (1 - k)); }
      else if (isl.leaveIdx === i) { const k = clamp((t - isl.leaveT0) / .4); isl.bounce[i] = k < .4 ? 1.15 - k * .25 / .4 : .9 + (k - .4) * .1 / .6; }
    }
    const animating = kw < 1 || (!isl.showing && isl.op > 0) || isl.btnT0.some(t0 => t0 >= 0 && t - t0 < .6) || (isl.leaveIdx >= 0 && t - isl.leaveT0 < .4) || (isl.hoverIdx >= 0 && t - isl.hoverT0 < .15);
    islandEl.style.opacity = isl.op.toFixed(3);
    const key = isl.w.toFixed(2) + '|' + isl.btn.map((v, i) => (v * isl.bounce[i]).toFixed(3)).join(',');
    if (key !== isl.key && isl.op > 0) {
      isl.key = key;
      const d = Math.min(2, devicePixelRatio || 1); if (islandCv.width !== Math.round(280 * d)) { islandCv.width = Math.round(280 * d); islandCv.height = Math.round(82 * d); }
      drawIsland(islandCv, isl.w, isl.btn.map((v, i) => v * isl.bounce[i]));
      const inset = (240 - isl.w) / 2 + 1; islandBlur.style.left = islandBlur.style.right = inset + 'px';
    }
    return animating;
  }

  /* ---------- 再生バー（CanvasVideo::paintPlaybackOverlay） ---------- */
  const bar = {owner: null, fade: 0, popT0: -10, seekHover: 0, countdown: false, muted: false};
  const ICON_IMG = {};
  ['play', 'stop', 'sound'].forEach(k => loadCanvas(`assets/ui_${k}.png`).then(c => ICON_IMG[k] = c).catch(() => {}));
  const acc = hexRgb(ACCENT), accShade = shadeOf(ACCENT, .42);
  const accLight = (() => { const [h, s, v] = rgb2hsv(acc); return hsv2rgb(h, Math.min(1, s * .82), Math.min(1, v * 1.35 + 40 / 255)); })();
  function glassPlate(x, path, a) {   // 島・再生バー共通のガラスの板（アクセント色 α165 ＋ フロスト ＋ 乗算の縁）
    x.save(); x.fillStyle = rgba(acc, 165 * a); x.fill(path); x.clip(path); x.fillStyle = W8(10 * a); x.fill(path); x.restore();
    x.save(); x.globalCompositeOperation = 'multiply'; x.strokeStyle = rgba(accShade, 190 * a); x.lineWidth = 1; x.stroke(path); x.restore();
  }
  function drawBar(t, dt) {
    const x = uiX; x.setTransform(DPR, 0, 0, DPR, 0, 0); x.clearRect(0, 0, W, H);
    const v = active && active.kind === 'video' ? active : null;
    if (v) bar.owner = v;
    const idleHidden = focus && focus.dir > 0 && (t - lastMoveT) > 2 && !(mouseLocal && mouseLocal.y >= H - 76);   // 全画面中は2秒止まると隠れる
    const want = !!v && mode !== 'move' && !idleHidden;
    bar.fade += ((want ? 1 : 0) - bar.fade) * Math.min(1, (want ? 8 : 12) * dt);
    const o = bar.owner; if (!o || bar.fade < .002) return want || idleHidden === false && !!v;
    const a = bar.fade, by = H - 28;
    const tp = t - bar.popT0, popVol = tp < 0 ? 0 : (tp > .32 ? 1 : E.outBack(tp / .32)), popPlay = tp < .07 ? 0 : (tp - .07 > .32 ? 1 : E.outBack((tp - .07) / .32));
    const circle = (cx, r, img, imgSize, imgAlpha = 1) => {
      if (r < .5) return; const p = new Path2D(); p.arc(cx, by, r, 0, Math.PI * 2);
      glassPlate(x, p, a);
      x.save(); x.clip(p); x.globalCompositeOperation = 'lighter';
      const hl = x.createLinearGradient(cx - r, by - r, cx + r, by + r); hl.addColorStop(0, W8(22 * a)); hl.addColorStop(.55, W8(4 * a)); hl.addColorStop(1, W8(0));
      x.fillStyle = hl; x.fillRect(cx - r, by - r, r * 2, r * 2); x.restore();
      if (img) { x.save(); x.globalAlpha = a * imgAlpha; const s = imgSize * r / 16; x.drawImage(img, cx - s / 2, by - s / 2, s, s); x.restore(); }
    };
    circle(28, 16 * popPlay, ICON_IMG[o.playing ? 'play' : 'stop'], 25);
    circle(64, 16 * popVol, ICON_IMG.sound, 18, bar.muted ? .45 : 1);
    const sx = 88, sw = W - sx - 100, sy = by - 2;
    if (sw > 0) {
      const tr = new Path2D(); tr.roundRect(sx, sy, sw, 4, 2);
      x.save(); x.fillStyle = rgba(acc, 165 * .72 * a); x.fill(tr); x.save(); x.clip(tr); x.globalCompositeOperation = 'lighter'; x.fillStyle = W8(16 * a); x.fillRect(sx, sy, sw, 1.4); x.restore();
      x.globalCompositeOperation = 'multiply'; x.strokeStyle = rgba(accShade, 170 * a); x.lineWidth = 1; x.stroke(tr); x.restore();
      const played = sw * ((o.vt % o.dur) / o.dur);
      x.fillStyle = rgba(accLight, 235 * a); x.beginPath(); x.roundRect(sx, sy, Math.max(0, played), 4, 2); x.fill();
      const hov = mouseLocal && mouseLocal.x >= sx && mouseLocal.x <= sx + sw && Math.abs(mouseLocal.y - by) <= 12;
      bar.seekHover += ((hov || mode === 'seek' ? 1 : 0) - bar.seekHover) * Math.min(1, 14 * dt);
      if (bar.seekHover > .01) { x.fillStyle = `rgba(255,255,255,${(a * bar.seekHover).toFixed(3)})`; x.beginPath(); x.arc(sx + played, by, 6 * (.6 + .4 * bar.seekHover), 0, Math.PI * 2); x.fill(); }
    }
    const pad = new Path2D(); pad.roundRect(W - 12 - 80, by - 16, 80, 32, 16); glassPlate(x, pad, a);
    const fmt = s => `${s < 0 ? '-' : ''}${Math.floor(Math.abs(s) / 60)}:${String(Math.floor(Math.abs(s) % 60)).padStart(2, '0')}`;
    const pos = o.vt % o.dur, txt = bar.countdown ? fmt(pos - o.dur) : `${fmt(pos)} / ${fmt(o.dur)}`;
    x.fillStyle = `rgba(255,255,255,${(.94 * a).toFixed(3)})`; x.font = '600 11px "Segoe UI", Arial, sans-serif'; x.textAlign = 'center'; x.textBaseline = 'middle';
    x.fillText(txt, W - 12 - 40, by + .5);
    return Math.abs(bar.fade - (want ? 1 : 0)) > .002 || tp < .4 || (bar.seekHover > .01 && bar.seekHover < .99) || (focus && focus.dir > 0 && !idleHidden);
  }
  function barHit(sx, sy) {
    if (!bar.owner || bar.fade < .5 || active !== bar.owner) return null;
    const by = H - 28;
    if (Math.hypot(sx - 28, sy - by) <= 17) return 'play';
    if (Math.hypot(sx - 64, sy - by) <= 17) return 'vol';
    if (sx >= W - 92 && sx <= W - 12 && Math.abs(sy - by) <= 16) return 'time';
    if (sx >= 88 && sx <= W - 100 && sy >= by - 12 && sy <= by + 16) return 'seek';
    return null;
  }
  const seekTo = sx => { const o = bar.owner; if (!o) return; o.vt = clamp((sx - 88) / (W - 188)) * o.dur; o.needPaint = true; };

  /* ---------- 全画面 ---------- */
  function focusSize(aw, ah) {   // MainWindow::calcFocusSize（今の大きさの中で中身の対比へ。最小 480×270 だが置き場に収める）
    const curW = baseSize.w, curH = baseSize.h, tar = aw / ah, car = curW / curH;
    let nw, nh; if (tar > car) { nw = curW; nh = curW / tar; } else { nh = curH; nw = curH * tar; }
    const minW = Math.min(480, curW), minH = Math.min(270, curH);
    if (nw < minW) { nw = minW; nh = nw / tar; } if (nh < minH) { nh = minH; nw = nh * tar; }
    return {w: Math.min(nw, curW), h: Math.min(nh, curH)};
  }
  // CanvasWidget::animateCamera（画面座標ベースの補間）:
  //   zoom(f) = z0 × (z1/z0)^f（対数補間）, camPos(f) = P − ((1−f)·off0 + f·off1) / zoom(f)
  //   寄る: P = 着地先（バブル中心）, off1 = 0 → バブルが画面中心へまっすぐ引き込まれる
  //   引く: P = 出発点（いま中心にあるバブル）, off0 = 0 → バブルが着地後の位置へまっすぐ送り出される
  function camAnim(tp, tz, dur, anchorTarget, extra) {
    const from = {x: cam.cx, y: cam.cy}, z0 = cam.z, P = anchorTarget ? {x: tp.x, y: tp.y} : from;
    return Object.assign({t0: now(), dur, z0, z1: tz, P, tp,
      off0: {x: (P.x - from.x) * z0, y: (P.y - from.y) * z0}, off1: {x: (P.x - tp.x) * tz, y: (P.y - tp.y) * tz}}, extra);
  }
  function startFocus(b) {
    if (b.kind === 'tile') return;
    activate(b); b.lift = 1; b.liftAnim = null; inertia.on = false; inertia.vx = inertia.vy = 0; zoomTarget = null; selected.clear();
    baseSize.w = W; baseSize.h = H;
    const fs = focusSize(cw(b), ch(b)), z = clamp(Math.min(fs.w / cw(b), fs.h / ch(b)), .1, 10);
    focus = camAnim({x: b.x, y: b.y}, z, .5, true, {b, dir: 1, w0: W, h0: H, w1: fs.w, h1: fs.h, pre: {x: cam.cx, y: cam.cy, z: cam.z}});
  }
  function endFocus() {
    if (!focus || focus.dir < 0) return;
    const pre = focus.pre;
    focus = camAnim({x: pre.x, y: pre.y}, pre.z, .3, false, {b: focus.b, dir: -1, w0: W, h0: H, w1: baseSize.w, h1: baseSize.h, pre});
  }
  function stepFocus(t) {
    if (!focus) return false;
    const k = clamp((t - focus.t0) / focus.dur), f = E.inOut(k);
    if (focus.dir < 0 || !focus.settled) {
      const zf = focus.z0 * Math.pow(focus.z1 / focus.z0, f);
      cam.z = zf; cam.cx = focus.P.x - ((1 - f) * focus.off0.x + f * focus.off1.x) / zf; cam.cy = focus.P.y - ((1 - f) * focus.off0.y + f * focus.off1.y) / zf;
      const w = lerp(focus.w0, focus.w1, f), h = lerp(focus.h0, focus.h1, f);   // ウィンドウも同じ長さ・同じカーブ（InOutCubic）
      pbBody.style.width = w + 'px'; pbBody.style.height = h + 'px';
      if (Math.abs(w - W) > .5 || Math.abs(h - H) > .5) applySize();
      if (k >= 1 && focus.dir > 0) focus.settled = true;
    }
    focusAmt = focus.dir > 0 ? f : 1 - f;
    if (k >= 1 && focus.dir < 0) { focus = null; focusAmt = 0; pbBody.style.width = ''; pbBody.style.height = ''; applySize(); return true; }
    return k < 1;
  }
  function clampFocusCam() {
    if (!focus || focus.dir < 0) return; const b = focus.b, hw = Math.max(0, (cw(b) * cam.z - W) / 2) / cam.z, hh = Math.max(0, (ch(b) * cam.z - H) / 2) / cam.z;
    cam.cx = clamp(cam.cx, b.x - hw, b.x + hw); cam.cy = clamp(cam.cy, b.y - hh, b.y + hh);
  }

  /* ---------- 描画（CanvasWidget::paintGL のバブル部分） ---------- */
  function render(t, dt) {
    let anim = stepFocus(t); if (stepPush(t)) anim = true;
    if (zoomTarget !== null) {   // ホイールズームのイージング（CameraZoomEase）
      const before = toWorld(zoomAnchor.x, zoomAnchor.y);
      cam.z += (zoomTarget - cam.z) * Math.min(1, dt * 16);
      const after = toWorld(zoomAnchor.x, zoomAnchor.y); cam.cx += before.x - after.x; cam.cy += before.y - after.y; clampFocusCam();
      if (Math.abs(zoomTarget - cam.z) < 1e-4) { cam.z = zoomTarget; zoomTarget = null; } anim = true;
    }
    if (inertia.on && mode !== 'pan') {   // 慣性: 1秒で6%まで減速する指数減衰、5未満で停止
      const d = Math.min(dt, .064); cam.cx += inertia.vx * d; cam.cy += inertia.vy * d; clampFocusCam();
      const k = Math.pow(.06, d); inertia.vx *= k; inertia.vy *= k;
      if (Math.hypot(inertia.vx, inertia.vy) < 5) { inertia.on = false; inertia.vx = inertia.vy = 0; } else anim = true;
    }
    const gTarget = focus && focus.dir > 0 ? 0 : 1; glassF += (gTarget - glassF) * Math.min(1, dt / .1); if (Math.abs(glassF - gTarget) > .002) anim = true; else glassF = gTarget;
    // 逆輸入の暗転（2回目を押して140ms／動かす／Shift。200ms OutCubic）
    const want = (mode === 'export' || (mode === 'dpress' && press && (t - press.t >= .14 || press.moved3)) || (shiftDown && BUB.length && pointerInside && !focus)) ? 1 : 0;
    if (want !== exportWant) { exportWant = want; exportFrom = exportHint; exportT0 = t; }
    exportHint = lerp(exportFrom, exportWant, E.out(clamp((t - exportT0) / .2))); if (Math.abs(exportHint - exportWant) > .002) anim = true;
    const gs = 22 * cam.z; host.style.backgroundSize = `${gs}px ${gs}px`; host.style.backgroundPosition = `${W / 2 - cam.cx * cam.z}px ${H / 2 - cam.cy * cam.z}px`;

    g.begin();
    const gummy = style === 'gummy', z = cam.z, vcx = W / 2, vcy = H / 2, pm = g.pm, anchorX = W * .3, anchorY = -H * .25;
    if (exportHint > .002) g.rect(0, 0, W, H, 0, pm(0, 0, 0, 120 * exportHint), pm(0, 0, 0, 0), 0);
    const ph = (t / .9) % 2, pulse = 190 + 65 * E.inOutSine(ph < 1 ? ph : 2 - ph);   // 190↔255 を 900ms で往復
    for (const b of BUB) {
      if (b.liftAnim) { const k = clamp((t - b.liftAnim.t0) / b.liftAnim.dur); b.lift = lerp(b.liftAnim.from, b.liftAnim.to, b.liftAnim.ease(k)); if (k >= 1) b.liftAnim = null; else anim = true; }
      let s = b.lift, wob = 0;
      const d2 = t - styleChangedAt - Math.hypot(b.x - cam.cx, b.y - cam.cy) * .0006;   // スタイル切り替えのぷるっ（サイトの演出）
      if (d2 > 0 && d2 < .7) { s *= 1 - Math.sin(Math.min(d2, .12) / .12 * Math.PI) * .07; wob = Math.sin(d2 * 24) * Math.exp(-d2 * 7) * .05; anim = true; }
      const isF = focus && focus.b === b, gf = isF ? glassF : 1, ccf = isF ? 1 - focusAmt : 1;
      if (b.vcv) {
        if (b.playing) { b.vt += dt; anim = true; }
        if (b.playing || b.needPaint) { b.paint(b.vcv, b.vt + .8); g.texUpdate(b.tex, b.vcv); b.needPaint = false;
          const key = Math.floor(b.vt * 10); if (key !== b.smallKey) { b.smallKey = key; g.makeSmall(b.vcv, 0, 0, b.vcv.width, b.vcv.height, b.small); } }
      }
      const cx = W / 2 + (b.x - cam.cx) * z, cy = H / 2 + (b.y - cam.cy) * z;
      const sw = cw(b) * z * s * (1 + wob), sh = ch(b) * z * s * (1 - wob);
      const launcher = b.kind === 'tile', bez = Math.min(sw, sh) * (launcher ? LAUNCHER_BEZEL_RATIO : MEDIA_BEZEL_RATIO);
      const sr = [cx - sw / 2, cy - sh / 2, sw, sh], board = [sr[0] - bez, sr[1] - bez, sw + bez * 2, sh + bez * 2];
      if (board[0] > W + 80 || board[1] > H + 80 || board[0] + board[2] < -80 || board[1] + board[3] < -80) continue;
      const boardR = launcher ? bez * .8 : bez * 1.25, contentR = Math.min(sw, sh) * MEDIA_BEZEL_RATIO * .25 * ccf;
      const zn = Math.max(z, 1e-6);
      const lightAngle = Math.atan2((vcy + (anchorY - vcy) / zn) - (vcy + (cy - vcy) / zn), (vcx + (anchorX - vcx) / zn) - (vcx + (cx - vcx) / zn));
      const isAct = (b === active || selected.has(b)) && !isF;
      const opts = {board, radius: boardR, bezel: bez, contentRadius: contentR, lightAngle, intensity: (isAct ? 1 : .8) * gf,
        innerMix: launcher ? 0 : 1, launcher, content: launcher ? (gummy ? b.tex : null) : b.tex, small: gummy ? b.small : null};
      if (gf > .001) g.glow(...board, boardR, gummy ? pm(5, 31, 38, 150 * GUMMY_SHADOW_K * gf) : pm(0, 0, 0, 45 * SHADOW_K * gf), Math.max(8, bez * (gummy ? 1.21 : .64)));
      const chrome = a => {   // 枠（アクティブ時は青い枠とグローが脈打つ）
        if (a <= .002) return;
        if (isAct) { g.glow(...board, boardR, pm(120, 180, 255, Math.min(165, pulse * .6) * a), 32); g.rect(...board, boardR, pm(255, 255, 255, 90 * a), pm(140, 195, 255, pulse * a), 4); anim = true; }
        else g.rect(...board, boardR, pm(255, 255, 255, (launcher ? 46 : gummy ? 0 : 46) * a), pm(255, 255, 255, (launcher ? 130 : gummy ? 118 : 130) * a), 1.5);
      };
      const markers = a => { if (a <= .002) return; const ix = board[0] + board[2] - bez * .5, iy = board[1] + board[3] - bez * .5, is = bez * 10 / 25, lw = Math.max(1, bez * 1.5 / 25), c = pm(255, 255, 255, 180 * a);
        g.line(ix - is, iy - is * .3, ix - is * .3, iy - is, lw, c); g.line(ix - is * .6, iy - is * .3, ix - is * .3, iy - is * .6, lw, c); };
      if (!launcher) {
        if (gf > .001) g.glow(...sr, contentR, pm(2, 4, 8, gummy ? 67 * GUMMY_CONTACT_K * gf : 82 * CONTACT_SHADOW_K * gf), bez * .22);
        chrome(ccf); markers(ccf);
        g.image(b.tex, ...sr, contentR, 1);
        if (b.kind === 'video') {
          const r0 = Math.min(sw, sh) * .13; let sc = 1, mark = 1, al = 1, tex = TEX_STOP;
          if (b.playing) { const k = Math.exp(-12 * (t - b.playAt)); sc = k; mark = 1.5 * k; al = k; tex = TEX_PLAY; if (al > .002) anim = true; }
          if (al > .002) { g.circle(cx, cy, r0 * sc, pm(0, 0, 0, 120 * al)); const is = r0 * mark * 1.1; g.image(tex, cx - is / 2, cy - is / 2, is, is, 0, al); }
        }
        if (gf > .001) g.glass(style, opts);
      } else {
        if (gummy && gf > .001) g.glass(style, opts);
        chrome(1);
        const unit = sw / 96, iconS = 60 * unit, icy = board[1] + board[3] / 2;
        g.image(b.tex, cx - iconS / 2, icy - iconS / 2, iconS, iconS, 0, 1);
        const fs = Math.min(1, 66 / b.labelPxW), lh = 14 * 1.35 * unit * fs, lwid = lh * b.labelAR, ly = icy + 34 * unit;
        if (gummy) { g.multiply(true); g.image(b.labelG, cx - lwid / 2, ly, lwid, lh, 0, 1); g.multiply(false); }
        else g.image(b.labelW, cx - lwid / 2, ly, lwid, lh, 0, 1);
        markers(1);
        if (!gummy && gf > .001) g.glass(style, opts);
      }
    }
    if (rect) { const a = toScreen(rect.x0, rect.y0), c = toScreen(rect.x1, rect.y1), x0 = Math.min(a.x, c.x), y0 = Math.min(a.y, c.y), w = Math.abs(c.x - a.x), h = Math.abs(c.y - a.y);
      if (w >= 2 || h >= 2) g.rect(x0, y0, w, h, Math.min(1, w / 2, h / 2), pm(130, 175, 255, 30), pm(130, 175, 255, 230), 1.5); }
    if (drawBar(t, dt)) anim = true;
    if (updateIsland(t)) anim = true;
    return anim;
  }

  /* ---------- 操作 ---------- */
  let shiftDown = false, pointerInside = false, lastMid = null, lastRight = null;
  addEventListener('keydown', e => { if (e.key === 'Shift') { shiftDown = true; kick(); } });
  addEventListener('keyup', e => { if (e.key === 'Shift') { shiftDown = false; kick(); } });
  const local = e => { const r = pbBody.getBoundingClientRect(); return {x: e.clientX - r.left, y: e.clientY - r.top}; };
  pbEl.addEventListener('contextmenu', e => e.preventDefault());
  pbEl.addEventListener('mousedown', e => { if (e.button === 1) e.preventDefault(); });   // 中ボタンの自動スクロールを出さない
  pbEl.addEventListener('auxclick', e => { if (e.button === 1) e.preventDefault(); });
  function schedClick(b) {   // 280ms 待ってダブルクリックでなければシングルクリック（動画は再生/一時停止）
    if (pendingClick) clearTimeout(pendingClick.timer);
    const pc = {b, t: now()}; pc.timer = setTimeout(() => { if (pendingClick === pc) { pendingClick = null; if (b.kind === 'video') { togglePlay(b); kick(); } } }, 280);
    pendingClick = pc;
  }
  const cancelClick = () => { if (pendingClick) { clearTimeout(pendingClick.timer); pendingClick = null; } };
  function togglePlay(b) { b.playing = !b.playing; if (b.playing) b.playAt = now(); }

  pbEl.addEventListener('pointerdown', e => {
    if (pbEl.classList.contains('hidden') || exporting) return;
    const p = local(e), t = now(); try { pbEl.setPointerCapture(e.pointerId); } catch (_) {} mouseLocal = p; lastMoveT = t;
    const ib = islandButtonAt(p.x, p.y);
    if (ib >= 0 && e.button === 0) { islandAction(ib); kick(); return; }
    if (e.button === 1) {   // 中ボタン: キャンバス移動（全画面中は中身の表示位置のパン）
      e.preventDefault(); inertia.on = false; inertia.vx = inertia.vy = 0; zoomTarget = null;
      if (focus && (focus.dir < 0 || !focus.settled)) return;
      const dbl = lastMid && t - lastMid.t < .3 && Math.abs(p.x - lastMid.x) + Math.abs(p.y - lastMid.y) < 6; lastMid = {t, x: p.x, y: p.y};
      if (dbl && focus) { const b = focus.b; zoomAnchor = {x: W / 2, y: H / 2}; zoomTarget = Math.min(W / cw(b), H / ch(b)); cam.cx = b.x; cam.cy = b.y; kick(); return; }
      inertia.on = false; inertia.vx = inertia.vy = 0; inertia.sampleT = performance.now();
      mode = 'pan'; press = {x: p.x, y: p.y, lx: p.x, ly: p.y, t, mt: t}; kick(); return;
    }
    if (e.button === 2) { press = {x: p.x, y: p.y, cx: e.clientX, cy: e.clientY, t, moved: false, ox: winOff.x, oy: winOff.y}; mode = 'rpress'; kick(); return; }
    if (e.button !== 0) return;
    const bh = barHit(p.x, p.y);   // 再生バー（左クリック専用）
    if (bh) {
      const o = bar.owner;
      if (bh === 'play') togglePlay(o); else if (bh === 'vol') bar.muted = !bar.muted; else if (bh === 'time') bar.countdown = !bar.countdown;
      else { mode = 'seek'; press = {x: p.x, y: p.y, t}; seekTo(p.x); }
      kick(); return;
    }
    const h = hit(p.x, p.y);
    if (focus) {   // 全画面中: ダブルクリックで解除。シングルは再生/一時停止だけ。ほかの操作はしない
      if (pendingClick && t - pendingClick.t < .28) { cancelClick(); mode = 'fdpress'; press = {x: p.x, y: p.y, t}; return; }
      schedClick(focus.b); return;
    }
    if (pendingClick && t - pendingClick.t < .28 && h && h.b === pendingClick.b) {   // ダブルクリックの2回目（離したときに確定）
      cancelClick(); activate(h.b); mode = 'dpress'; press = {x: p.x, y: p.y, t, b: h.b}; kick(); return;
    }
    if (h) {
      pushAnims.delete(h.b); raise(h.b);
      if (!(selected.size > 1 && selected.has(h.b))) selected.clear();
      activate(h.b);
      if (h.handle) { mode = 'resize'; press = {x: p.x, y: p.y, t, b: h.b, s0: h.b.scale, w0: toWorld(p.x, p.y)}; }
      else { mode = 'move'; press = {x: p.x, y: p.y, lx: p.x, ly: p.y, t, b: h.b, shift: e.shiftKey}; setLift(h.b, 1.025); }
      schedClick(h.b);
    } else {
      cancelClick(); if (active) setLift(active, 1); active = null;
      if (!e.shiftKey) selected.clear();
      mode = 'rect'; const w = toWorld(p.x, p.y); rect = {x0: w.x, y0: w.y, x1: w.x, y1: w.y}; press = {x: p.x, y: p.y, t};
    }
    kick();
  });

  pbEl.addEventListener('pointermove', e => {
    if (exporting) return;
    const p = local(e), t = now(); mouseLocal = p; lastMoveT = t; pointerInside = true;
    if (!press) {
      const ib = islandButtonAt(p.x, p.y);
      if (ib !== isl.hoverIdx) { if (isl.hoverIdx >= 0) { isl.leaveIdx = isl.hoverIdx; isl.leaveT0 = t; } isl.bounceFrom = ib >= 0 ? isl.bounce[ib] : 1; isl.hoverIdx = ib; isl.hoverT0 = t; }
      pbEl.style.cursor = ib >= 0 || barHit(p.x, p.y) ? 'pointer' : (hit(p.x, p.y) || {}).handle ? 'nwse-resize' : '';
      kick(); return;
    }
    const dx = p.x - (press.lx ?? press.x), dy = p.y - (press.ly ?? press.y); press.lx = p.x; press.ly = p.y;
    const dist = Math.abs(p.x - press.x) + Math.abs(p.y - press.y);
    if (mode === 'pan') {
      const bx = cam.cx, by = cam.cy; cam.cx -= dx / cam.z; cam.cy -= dy / cam.z; clampFocusCam();
      const gap = (performance.now() - inertia.sampleT); inertia.sampleT = performance.now();   // 実際に動いた量から速度（EMA 0.35、200ms以上空いたら使わない）
      if (gap > 0 && gap < 200) { const ivx = (cam.cx - bx) * 1000 / gap, ivy = (cam.cy - by) * 1000 / gap; inertia.vx = inertia.vx * .65 + ivx * .35; inertia.vy = inertia.vy * .65 + ivy * .35; }
    } else if (mode === 'move') {
      if (dist > 6) cancelClick();
      if ((press.shift || e.shiftKey) && dist > 8) { beginExport(press.b, e); return; }   // Shift+ドラッグでも逆輸入
      const targets = selected.size > 1 && selected.has(press.b) ? [...selected] : [press.b];
      targets.forEach(b => { b.x += dx / cam.z; b.y += dy / cam.z; });
    } else if (mode === 'resize') {
      if (dist > 6) cancelClick();
      const b = press.b, w = toWorld(p.x, p.y), d0 = Math.hypot(press.w0.x - b.x, press.w0.y - b.y), d1 = Math.hypot(w.x - b.x, w.y - b.y);
      b.scale = clamp(press.s0 * d1 / Math.max(1, d0), .35, 4);
    } else if (mode === 'rect') {
      const w = toWorld(p.x, p.y); rect.x1 = w.x; rect.y1 = w.y;
      const x0 = Math.min(rect.x0, rect.x1), x1 = Math.max(rect.x0, rect.x1), y0 = Math.min(rect.y0, rect.y1), y1 = Math.max(rect.y0, rect.y1);
      selected.clear(); BUB.forEach(b => { const r = boardOf(b); if (r.x1 >= x0 && r.x0 <= x1 && r.y1 >= y0 && r.y0 <= y1) selected.add(b); });
    } else if (mode === 'dpress') {
      if (dist > 3) press.moved3 = true;
      if (dist > 8) { beginExport(press.b, e); return; }
    } else if (mode === 'seek') seekTo(p.x);
    else if (mode === 'rpress' || mode === 'window') {
      if (Math.hypot(e.clientX - press.cx, e.clientY - press.cy) > 6) { mode = 'window'; press.moved = true; }
      if (mode === 'window') moveWindow(press.ox + e.clientX - press.cx, press.oy + e.clientY - press.cy);
    }
    kick();
  });
  const endPress = () => {
    if (exporting) return;
    const t = now(), m = mode, pr = press; mode = ''; press = null;
    if (m === 'pan') {   // 離す前に30ms以上止まっていたら慣性なし。40未満も始めない
      if (performance.now() - inertia.sampleT > 30) inertia.vx = inertia.vy = 0;
      inertia.on = Math.hypot(inertia.vx, inertia.vy) > 40; if (!inertia.on) inertia.vx = inertia.vy = 0;
    }
    if (m === 'move' && pr) { setLift(pr.b, 1); pushOverlaps(selected.size > 1 && selected.has(pr.b) ? [...selected] : [pr.b]); }
    if (m === 'resize' && pr) pushOverlaps([pr.b]);
    if (m === 'rect') rect = null;
    if (m === 'dpress' && pr) startFocus(pr.b);   // ダブルクリック確定 → 全画面
    if (m === 'fdpress') endFocus();
    if (m === 'rpress' && pr && !pr.moved) {      // 右クリック: アクティブ化だけ／右ダブルクリック: 全画面
      const h = hit(pr.x, pr.y);
      if (lastRight && t - lastRight.t < .3 && (focus || (h && h.b === lastRight.b))) { lastRight = null; if (focus) endFocus(); else startFocus(h.b); }
      else { lastRight = {t, b: h ? h.b : null}; if (h && !focus) { raise(h.b); activate(h.b); } }
    }
    kick();
  };
  pbEl.addEventListener('pointerup', endPress); pbEl.addEventListener('pointercancel', endPress);
  pbEl.addEventListener('pointerleave', () => { if (!press) { mouseLocal = null; pointerInside = false; if (isl.hoverIdx >= 0) { isl.leaveIdx = isl.hoverIdx; isl.leaveT0 = now(); } isl.hoverIdx = -1; kick(); } });
  pbEl.addEventListener('wheel', e => {   // PB の上ではホイールは常に PB のズーム（ページはスクロールしない）
    e.preventDefault(); if (pbEl.classList.contains('hidden') || (focus && (focus.dir < 0 || !focus.settled))) return;
    inertia.on = false; inertia.vx = inertia.vy = 0;
    const p = local(e), notches = clamp(-e.deltaY / (e.deltaMode === 1 ? 3 : 100), -3, 3);
    let tz = (zoomTarget ?? cam.z) * Math.pow(1.1, notches);
    if (focus) { const b = focus.b, mn = Math.min(W / cw(b), H / ch(b)); tz = clamp(tz, mn, mn * 6); } else tz = clamp(tz, .1, 6);
    zoomTarget = tz; zoomAnchor = p; kick();
  }, {passive: false});

  /* ---------- ウィンドウ（右ドラッグで移動・島のボタン） ---------- */
  const winOff = {x: 0, y: 0};
  function moveWindow(x, y) {
    const st = demoEl.getBoundingClientRect(), r = pbEl.getBoundingClientRect(), bx = r.left - winOff.x;
    winOff.x = clamp(x, st.left - bx - r.width * .45, st.right - bx - r.width * .55); winOff.y = clamp(y, -r.height * .25, r.height * .25);
    pbEl.style.translate = `${winOff.x}px ${winOff.y}px`;
  }
  function islandAction(i) {
    if (i === 3 || i === 1) {   // 閉じる / 最小化 → トレイへ。ショートカットで呼び戻す
      if (focus) { focus = null; focusAmt = 0; glassF = 1; pbBody.style.width = ''; pbBody.style.height = ''; }
      hideIsland(now()); pbEl.classList.add('hidden'); recall.hidden = false; mouseLocal = null;
    } else if (i === 2) {      // 最大化 ⇄ 戻す（中身を素早く消して、ウィンドウが変わったらフェードで戻す。島は即座に）
      host.classList.add('fading'); uiCv.classList.add('fading'); islandEl.style.visibility = 'hidden';
      setTimeout(() => {
        demoEl.classList.toggle('maxed'); winOff.x = winOff.y = 0; pbEl.style.translate = '';
        setTimeout(() => { applySize(); host.classList.remove('fading'); uiCv.classList.remove('fading'); islandEl.style.visibility = ''; kick(); }, 220);
      }, 120);
    }
  }
  function summon() { if (!pbEl.classList.contains('hidden')) return; pbEl.classList.remove('hidden'); recall.hidden = true; applySize(); kick(); }
  recall.addEventListener('click', summon);
  addEventListener('keydown', e => { if (e.ctrlKey && e.shiftKey && e.code === 'Space' && visible) { e.preventDefault(); summon(); } });

  /* ---------- 逆輸入（BubbleDragExporter）: 中身だけのプレビュー（最大 160×120）を PB の外へ ---------- */
  let exporting = null;
  const explorer = section.querySelector('.src-explorer'), exList = explorer.querySelector('.src-list');
  function beginExport(b, e) {
    cancelClick(); if (mode === 'move' && press) setLift(press.b, 1);
    mode = 'export'; press = null;
    const src = b.thumb, k = Math.min(160 / src.width, 120 / src.height, 1), pw = Math.round(src.width * k), ph = Math.round(src.height * k);
    const ghost = document.createElement('canvas'); ghost.className = 'export-ghost'; ghost.width = pw * 2; ghost.height = ph * 2; ghost.style.width = pw + 'px'; ghost.style.height = ph + 'px';
    ghost.getContext('2d').drawImage(src, 0, 0, pw * 2, ph * 2); document.body.appendChild(ghost);
    exporting = {b, ghost};
    const inEx = ev => { const r = explorer.getBoundingClientRect(); return ev.clientX > r.left && ev.clientX < r.right && ev.clientY > r.top && ev.clientY < r.bottom; };
    const mv = ev => { ghost.style.left = ev.clientX + 6 + 'px'; ghost.style.top = ev.clientY + 6 + 'px'; explorer.classList.toggle('drop-target', inEx(ev)); };
    mv(e);
    const up = ev => {
      removeEventListener('pointermove', mv); removeEventListener('pointerup', up);
      explorer.classList.remove('drop-target'); ghost.remove();
      if (inEx(ev)) addExplorerItem(b);
      exporting = null; mode = ''; kick();
    };
    addEventListener('pointermove', mv); addEventListener('pointerup', up);
    kick();
  }
  function addExplorerItem(b) {
    const btn = document.createElement('button'); btn.type = 'button'; btn.className = 'item exported' + (b.kind === 'tile' ? ' tile' : ''); btn.dataset.key = b.key;
    const th = document.createElement('span'); th.dataset.thumb = b.key; const c = document.createElement('canvas'); c.width = b.thumb.width; c.height = b.thumb.height; c.getContext('2d').drawImage(b.thumb, 0, 0); th.appendChild(c);
    const sm = document.createElement('small'); sm.textContent = b.label; btn.append(th, sm); exList.appendChild(btn); wireItem(btn);
    explorer.classList.add('got-file'); setTimeout(() => explorer.classList.remove('got-file'), 900);
  }

  /* ---------- 外から素材を入れる（ドロップした場所にそのまま置く） ---------- */
  function wireItem(item) {
    item.addEventListener('pointerdown', e => {
      if (e.button !== 0) return; e.preventDefault();
      const key = item.dataset.key, r0 = item.getBoundingClientRect();
      const ghost = item.cloneNode(true); ghost.classList.add('ghost'); ghost.classList.remove('exported'); ghost.removeAttribute('id');
      const gcs = ghost.querySelectorAll('canvas'); item.querySelectorAll('canvas').forEach((c, i) => { const d = gcs[i]; d.width = c.width; d.height = c.height; d.getContext('2d').drawImage(c, 0, 0); });
      Object.assign(ghost.style, {left: r0.left + 'px', top: r0.top + 'px', width: r0.width + 'px', height: r0.height + 'px'});
      document.body.appendChild(ghost);
      const ox = e.clientX - r0.left, oy = e.clientY - r0.top; let moved = false;
      try { item.setPointerCapture(e.pointerId); } catch (_) {} item.classList.add('lifting');
      const move = ev => { if (Math.hypot(ev.clientX - e.clientX, ev.clientY - e.clientY) > 4) moved = true; ghost.style.left = ev.clientX - ox + 'px'; ghost.style.top = ev.clientY - oy + 'px'; };
      const up = ev => {
        item.removeEventListener('pointermove', move); item.removeEventListener('pointerup', up); item.removeEventListener('pointercancel', up);
        item.classList.remove('lifting'); ghost.remove();
        if (pbEl.classList.contains('hidden')) return;
        const pr = pbBody.getBoundingClientRect(), inside = ev.clientX > pr.left && ev.clientX < pr.right && ev.clientY > pr.top && ev.clientY < pr.bottom;
        if (focus) return;               // 全画面中はドロップできない
        if (inside || !moved) {          // タップだけでもボードの見えている場所に入れる
          const p = inside && moved ? toWorld(ev.clientX - pr.left, ev.clientY - pr.top) : toWorld(W / 2 + (Math.random() - .5) * W * .4, H / 2 + (Math.random() - .5) * H * .3);
          makeBubble(key, p.x, p.y).then(b => { BUB.push(b); pushOverlaps([b]); kick(); });
        }
      };
      item.addEventListener('pointermove', move); item.addEventListener('pointerup', up); item.addEventListener('pointercancel', up);
    });
  }
  section.querySelectorAll('.src .item').forEach(wireItem);

  addEventListener('pbstyle', e => { style = e.detail; styleChangedAt = now(); kick(); });
  section.querySelector('.demo-reset').addEventListener('click', async () => {
    focus = null; focusAmt = 0; glassF = 1; pbBody.style.width = ''; pbBody.style.height = ''; demoEl.classList.remove('maxed'); winOff.x = winOff.y = 0; pbEl.style.translate = '';
    pbEl.classList.remove('hidden'); recall.hidden = true; exList.querySelectorAll('.exported').forEach(x => x.remove());
    active = null; selected.clear(); BUB.length = 0; applySize(); fitCam(); await seed(); kick();
  });

  /* ---------- ループ ---------- */
  let raf = 0;
  function loop() {
    raf = 0; if (!visible) return;
    const t = now(), dt = Math.min(.05, Math.max(0, t - lastT)); lastT = t;
    const a = render(t, dt);
    if (a || press || mode || pendingClick || shiftDown) raf = requestAnimationFrame(loop);
  }
  function kick() { if (!raf && visible) { lastT = Math.max(lastT, now() - .016); raf = requestAnimationFrame(loop); } }
  new IntersectionObserver(es => { visible = es[0].isIntersecting; if (visible) kick(); }).observe(demoEl);

  function fitCam() { cam.cx = 0; cam.cy = 0; cam.z = clamp(Math.min(W / 820, H / 470), .4, 1); }
  async function seed() { for (const [k, x, y] of [['ocean', -60, -10], ['sunset', -330, 30], ['folder', 210, -70], ['site', 210, 95]]) BUB.push(await makeBubble(k, x, y)); }
  (async () => {
    TEX_STOP = g.texFrom(await loadCanvas('assets/ui_stop.png'));
    TEX_PLAY = g.texFrom(await loadCanvas('assets/ui_play.png'));
    applySize(); fitCam(); await seed();
    section.querySelectorAll('.src .item [data-thumb]').forEach(async el => {
      const S = SOURCES[el.dataset.thumb];
      try {
        if (S.web) el.appendChild(siteThumb());
        else if (S.kind === 'tile') { const im = new Image(); im.alt = ''; im.src = `assets/icon_${S.icon}.png`; el.appendChild(im); }
        else if (S.kind === 'video') { const c = document.createElement('canvas'); c.width = 320; c.height = 180; PAINT[S.paint](c, 1.2); el.appendChild(c); }
        else el.appendChild(await S.make());
      } catch (e) { console.error(e); }
    });
    kick();
  })().catch(e => showDemoError(demoEl, e));
}
function showDemoError(demoEl, e) {
  console.error(e);
  const box = document.createElement('p'); box.className = 'demo-error';
  box.textContent = 'デモを読み込めませんでした: ' + (e && e.message || e);
  demoEl.querySelector('.pb-body').appendChild(box);
}
