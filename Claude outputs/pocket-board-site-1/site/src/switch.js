/* ガラスのスタイル切り替え（本体の島・再生バーと同じガラスの台。つまみはドラッグでも動かせる） */
function initStyleSwitch() {
  const sw = document.getElementById('style-switch'); if (!sw) return;
  const plate = sw.querySelector('canvas'), thumb = sw.querySelector('.sw-thumb'), btns = [...sw.querySelectorAll('button')];
  const order = btns.map(b => b.dataset.style);
  let idx = Math.max(0, order.indexOf(window.PB_STYLE || 'gummy')), pos = idx, drag = null;
  const PAD = 4;
  function geom() { const r = sw.getBoundingClientRect(); return {w: r.width, h: r.height, seg: (r.width - PAD * 2) / order.length}; }
  function drawPlate() {
    const {w, h} = geom(), d = Math.min(2, devicePixelRatio || 1);
    plate.width = Math.round(w * d); plate.height = Math.round(h * d);
    const x = plate.getContext('2d'); x.setTransform(d, 0, 0, d, 0, 0); x.clearRect(0, 0, w, h);
    paintIslandBody(x, w, h, '#6f7076');   // 本体の既定のアクセント色（グレー）
  }
  function place(animate) {
    const {seg} = geom();
    thumb.style.transition = animate ? 'transform .42s cubic-bezier(.3,1.45,.5,1)' : 'none';
    thumb.style.width = seg + 'px'; thumb.style.transform = `translateX(${PAD + pos * seg}px)`;
    btns.forEach((b, i) => { const on = Math.abs(pos - i) < .5; b.setAttribute('aria-checked', on); b.tabIndex = on ? 0 : -1; });
  }
  function choose(i, fromDrag) {
    i = Math.max(0, Math.min(order.length - 1, i));
    if (i !== idx || fromDrag === 'init') {
      idx = i; window.PB_STYLE = order[i];
      if (fromDrag !== 'init') dispatchEvent(new CustomEvent('pbstyle', {detail: order[i]}));
    }
  }
  // 押したところへは離したときに移動（クリック）。つかんで動かすとその場で追従して、離すと一番近い所へ収まる
  sw.addEventListener('pointerdown', e => {
    if (e.button !== 0) return; e.preventDefault(); sw.setPointerCapture(e.pointerId);
    const {seg} = geom(), r = sw.getBoundingClientRect(), px = (e.clientX - r.left - PAD) / seg - .5;
    drag = {startX: e.clientX, startPos: pos, px, onThumb: Math.abs(px - pos) < .55, moved: false};
  });
  sw.addEventListener('pointermove', e => {
    if (!drag) return; const {seg} = geom();
    if (!drag.moved && Math.abs(e.clientX - drag.startX) < 4) return;
    if (!drag.moved) { drag.moved = true; sw.classList.add('dragging'); if (!drag.onThumb) drag.startPos = Math.max(0, Math.min(order.length - 1, drag.px)); }
    pos = Math.max(-.1, Math.min(order.length - 1 + .1, drag.startPos + (e.clientX - drag.startX) / seg));
    place(false); choose(Math.round(Math.max(0, Math.min(order.length - 1, pos))), true);
  });
  const end = () => {
    if (!drag) return; const d = drag; drag = null; sw.classList.remove('dragging');
    if (!d.moved) choose(Math.round(Math.max(0, Math.min(order.length - 1, d.px))));
    pos = idx; place(true);
  };
  sw.addEventListener('pointerup', end); sw.addEventListener('pointercancel', end);
  sw.addEventListener('keydown', e => {
    if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') { e.preventDefault(); choose(idx + (e.key === 'ArrowRight' ? 1 : -1)); pos = idx; place(true); btns[idx].focus(); }
  });
  new ResizeObserver(() => { drawPlate(); place(false); }).observe(sw);
  choose(idx, 'init'); drawPlate(); place(false);
}
