/* 紹介画像を画像バブルとして描く（デモと同じガラス。スタイルのスイッチにも追従） */
function initFigures() {
  const figs = [...document.querySelectorAll('.bubble-fig')]; if (!figs.length) return;
  let g = null; try { g = makeGLR(); } catch (e) { g = null; console.error(e); }
  if (!g) { figs.forEach(f => f.classList.add('no-gl')); return; }
  let style = window.PB_STYLE || 'gummy', styleAt = -10;
  const now = () => performance.now() / 1000;
  const items = figs.map(el => {
    const cv = document.createElement('canvas'); cv.className = 'fig-cv'; el.appendChild(cv);
    const it = {el, cv, ctx: cv.getContext('2d'), img: null, tex: null, small: null, s: 1, sTarget: 1, w: 0, h: 0, M: 0, visible: false};
    const im = new Image(); im.onload = () => { it.img = im; it.tex = g.texFrom(im, {mip: true}); it.small = g.makeSmall(im, 0, 0, im.width, im.height); layout(it); paint(it); }; im.src = el.dataset.src;
    el.addEventListener('pointerenter', () => { it.sTarget = 1.012; kick(); });   // ごく控えめに大きく
    el.addEventListener('pointerleave', () => { it.sTarget = 1; kick(); });
    return it;
  });
  // 中身の矩形は要素いっぱい（16:9）。枠・影ははみ出して描くので、キャンバスを外側へ広げる
  function layout(it) {
    const r = it.el.getBoundingClientRect(); if (!r.width) return;
    const cw = r.width, ch = r.height, bez = Math.min(cw, ch) * MEDIA_BEZEL_RATIO, M = Math.ceil(bez * 2.6);
    it.cw = cw; it.ch = ch; it.bez = bez; it.M = M; it.w = cw + M * 2; it.h = ch + M * 2;
    const d = Math.min(2, devicePixelRatio || 1); it.d = d;
    it.cv.width = Math.round(it.w * d); it.cv.height = Math.round(it.h * d);
    Object.assign(it.cv.style, {left: -M + 'px', top: -M + 'px', width: it.w + 'px', height: it.h + 'px'});
  }
  function paint(it, t = now()) {
    if (!it.tex || !it.w) return false;
    g.resize(it.w, it.h, it.d); g.begin();
    const gummy = style === 'gummy', pm = g.pm;
    let s = it.s, wob = 0, anim = false;
    const d2 = t - styleAt;   // スタイル切り替えのぷるっ（デモと同じ演出）
    if (d2 > 0 && d2 < .7) { s *= 1 - Math.sin(Math.min(d2, .12) / .12 * Math.PI) * .03; wob = Math.sin(d2 * 24) * Math.exp(-d2 * 7) * .02; anim = true; }
    const cx = it.w / 2, cy = it.h / 2, sw = it.cw * s * (1 + wob), sh = it.ch * s * (1 - wob);
    const bez = Math.min(sw, sh) * MEDIA_BEZEL_RATIO, sr = [cx - sw / 2, cy - sh / 2, sw, sh], board = [sr[0] - bez, sr[1] - bez, sw + bez * 2, sh + bez * 2];
    const boardR = bez * 1.25, contentR = Math.min(sw, sh) * MEDIA_BEZEL_RATIO * .25;
    const lightAngle = Math.atan2(-it.h * .25 - cy, it.w * .3 - cx);
    g.glow(...board, boardR, gummy ? pm(5, 31, 38, 150 * GUMMY_SHADOW_K) : pm(0, 0, 0, 45 * SHADOW_K), Math.max(8, bez * (gummy ? 1.21 : .64)));
    g.glow(...sr, contentR, pm(2, 4, 8, gummy ? 67 * GUMMY_CONTACT_K : 82 * CONTACT_SHADOW_K), bez * .22);
    g.rect(...board, boardR, pm(255, 255, 255, gummy ? 0 : 46), pm(255, 255, 255, gummy ? 118 : 130), 1.5);
    const ix = board[0] + board[2] - bez * .5, iy = board[1] + board[3] - bez * .5, is = bez * 10 / 25, lw = Math.max(1, bez * 1.5 / 25), c = pm(255, 255, 255, 180);
    g.line(ix - is, iy - is * .3, ix - is * .3, iy - is, lw, c); g.line(ix - is * .6, iy - is * .3, ix - is * .3, iy - is * .6, lw, c);
    g.image(it.tex, ...sr, contentR, 1);
    g.glass(style, {board, radius: boardR, bezel: bez, contentRadius: contentR, lightAngle, intensity: .8, innerMix: 1, launcher: false, content: it.tex, small: gummy ? it.small : null});
    it.ctx.setTransform(1, 0, 0, 1, 0, 0); it.ctx.clearRect(0, 0, it.cv.width, it.cv.height); it.ctx.drawImage(g.cv, 0, 0); it.el.classList.add('gl');
    return anim;
  }
  let raf = 0, lastT = now();
  function loop() {
    raf = 0; const t = now(), dt = Math.min(.05, t - lastT); lastT = t; let more = false;
    for (const it of items) {
      if (!it.visible) continue;
      const ds = it.sTarget - it.s, moving = Math.abs(ds) > 1e-4;
      if (moving) it.s += ds * Math.min(1, dt * 9); else it.s = it.sTarget;   // なめらかに
      if (moving || it.dirty || (t - styleAt) < .75) { if (paint(it, t) || moving) more = true; it.dirty = false; }
    }
    if (more || (now() - styleAt) < .75) raf = requestAnimationFrame(loop);
  }
  function kick() { if (!raf) { lastT = now() - .016; raf = requestAnimationFrame(loop); } }
  const io = new IntersectionObserver(es => es.forEach(e => { const it = items.find(i => i.el === e.target); it.visible = e.isIntersecting; if (it.visible) { it.dirty = true; kick(); } }), {rootMargin: '200px'});
  items.forEach(it => io.observe(it.el));
  items.forEach(it => new ResizeObserver(() => { layout(it); it.dirty = true; kick(); }).observe(it.el));
  addEventListener('pbstyle', e => { style = e.detail; styleAt = now(); items.forEach(it => it.dirty = true); kick(); });
}
