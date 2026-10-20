const rand = i => { const s = Math.sin(i * 12.9898 + 78.233) * 43758.5453; return s - Math.floor(s); };
function paintVid1(c, tt) {
  // 明るい南国の海（青〜エメラルド）。雲が流れ、波が寄せ、水面がきらめく
  const x = c.getContext('2d'), w = c.width, h = c.height, hy = h * .5;
  const sky = x.createLinearGradient(0, 0, 0, hy);
  sky.addColorStop(0, '#3fa9f5'); sky.addColorStop(.75, '#9fdcff'); sky.addColorStop(1, '#d9f4ff');
  x.fillStyle = sky; x.fillRect(0, 0, w, hy);
  // 太陽の光
  const sx0 = w * .74, sy0 = h * .16;
  const sun = x.createRadialGradient(sx0, sy0, 0, sx0, sy0, h * .55);
  sun.addColorStop(0, 'rgba(255,255,240,.95)'); sun.addColorStop(.12, 'rgba(255,253,225,.6)'); sun.addColorStop(1, 'rgba(255,255,255,0)');
  x.fillStyle = sun; x.fillRect(0, 0, w, hy);
  // 雲
  const cloud = (cx, cy, s) => { x.fillStyle = 'rgba(255,255,255,.92)'; x.beginPath();
    x.ellipse(cx, cy, s * 1.6, s * .55, 0, 0, Math.PI * 2); x.ellipse(cx - s * .7, cy + s * .05, s * .8, s * .45, 0, 0, Math.PI * 2);
    x.ellipse(cx + s * .2, cy - s * .35, s * .75, s * .55, 0, 0, Math.PI * 2); x.ellipse(cx + s * .95, cy + s * .05, s * .7, s * .4, 0, 0, Math.PI * 2); x.fill(); };
  for (let i = 0; i < 4; i++) { const s = h * (.05 + rand(i + 20) * .04); const px = ((rand(i + 30) * 1.4 + tt * .018 * (1 + i * .3)) % 1.4 - .2) * w;
    cloud(px, h * (.1 + rand(i + 40) * .22), s); }
  // 遠くの島
  x.fillStyle = '#2f9e7a'; x.beginPath(); x.moveTo(w * .08, hy); x.quadraticCurveTo(w * .2, hy - h * .09, w * .34, hy); x.fill();
  x.fillStyle = '#4cbf8f'; x.beginPath(); x.moveTo(w * .18, hy); x.quadraticCurveTo(w * .26, hy - h * .05, w * .38, hy); x.fill();
  // 海
  const sea = x.createLinearGradient(0, hy, 0, h);
  sea.addColorStop(0, '#1fb8d6'); sea.addColorStop(.45, '#13c2b0'); sea.addColorStop(1, '#5fe3c4');
  x.fillStyle = sea; x.fillRect(0, hy, w, h - hy);
  // 波の線（奥ほど細かく）
  x.lineCap = 'round';
  for (let k = 0; k < 14; k++) {
    const z = ((k + tt * .9) % 14) / 14, yy = hy + (h - hy) * z * z;
    x.strokeStyle = `rgba(255,255,255,${(.18 + z * .45).toFixed(3)})`; x.lineWidth = Math.max(1, w / 900 + z * w / 260);
    x.beginPath();
    for (let xx = 0; xx <= w; xx += w / 60) { const yv = yy + Math.sin(xx / w * (18 - z * 12) + tt * 1.6 + k) * h * .008 * (1 + z * 3); xx === 0 ? x.moveTo(xx, yv) : x.lineTo(xx, yv); }
    x.stroke();
  }
  // 水面のきらめき（太陽の下）
  for (let i = 0; i < 60; i++) {
    const px = sx0 + (rand(i) - .5) * w * .35 * (.4 + rand(i + 5)), py = hy + rand(i + 9) * (h - hy) * .55;
    const a = Math.pow(Math.max(0, Math.sin(tt * (3 + rand(i + 3) * 4) + i)), 6);
    if (a > .02) { x.fillStyle = `rgba(255,255,255,${(a * .9).toFixed(3)})`; x.fillRect(px, py, w / 140, w / 700 + 1); }
  }
  // 手前の砂浜
  const sand = x.createLinearGradient(0, h * .86, 0, h);
  sand.addColorStop(0, '#fff3cf'); sand.addColorStop(1, '#ffe2a3');
  x.fillStyle = sand; x.beginPath(); x.moveTo(0, h);
  for (let xx = 0; xx <= w; xx += w / 40) x.lineTo(xx, h * .9 + Math.sin(xx / w * 5 + tt * .8) * h * .012 + (xx / w) * h * .03);
  x.lineTo(w, h); x.closePath(); x.fill();
  x.strokeStyle = 'rgba(255,255,255,.85)'; x.lineWidth = Math.max(1.5, w / 300); x.beginPath();
  for (let xx = 0; xx <= w; xx += w / 40) { const yv = h * .9 + Math.sin(xx / w * 5 + tt * .8) * h * .012 + (xx / w) * h * .03; xx === 0 ? x.moveTo(xx, yv) : x.lineTo(xx, yv); }
  x.stroke();
}
function paintVid2(c, tt) {
  const x = c.getContext('2d'), w = c.width, h = c.height;
  const g = x.createRadialGradient(w * .5, h * .5, 0, w * .5, h * .5, w * .7);
  g.addColorStop(0, '#16224a'); g.addColorStop(1, '#070a18');
  x.globalCompositeOperation = 'source-over'; x.fillStyle = g; x.fillRect(0, 0, w, h);
  x.globalCompositeOperation = 'lighter';
  const cols = [[80, 220, 255], [255, 90, 190], [255, 210, 90]];
  for (let k = 0; k < 3; k++) {
    const px = w * .5 + Math.cos(tt * .9 + k * 2.1) * w * .26, py = h * .5 + Math.sin(tt * 1.2 + k * 1.7) * h * .26, rr = h * .42;
    const og = x.createRadialGradient(px, py, 0, px, py, rr);
    const [r, gg, b] = cols[k];
    og.addColorStop(0, `rgba(${r},${gg},${b},.95)`); og.addColorStop(.35, `rgba(${r},${gg},${b},.35)`); og.addColorStop(1, `rgba(${r},${gg},${b},0)`);
    x.fillStyle = og; x.fillRect(0, 0, w, h);
  }
  x.globalCompositeOperation = 'source-over';
}
