/* =====================================================================
   バブル描画（WebGL2）— PocketBoard 本体の描画を移植
   - ガラス: GlassHighlightEffect.cpp の GLASS_FRAG をそのまま使用（標準 / CARVED_GLASS / GUMMY_GLASS）
   - 影・枠: Renderer.cpp の GLOW_FRAG と CanvasWidget の描画順・値
   - 品質: Quality 相当（コンテンツ色の反射・グミのにじみ）。屈折プリパスは掴んでいる1個だけの演出なので無し
   ===================================================================== */

const makeGLR = () => {
  let RES = 1, VW = 1, VH = 1;
  const cv = document.createElement('canvas');
  function resize(w, h, r) { VW = w; VH = h; RES = r; cv.width = Math.max(1, Math.round(w * r)); cv.height = Math.max(1, Math.round(h * r)); }
  const gl = cv.getContext('webgl2', {premultipliedAlpha: true, alpha: true, antialias: true, preserveDrawingBuffer: true});
  if (!gl) return null;

  const VERT = `#version 300 es
in vec2 aPos; uniform vec2 uView; out vec2 vPos;
void main(){ vPos = aPos; gl_Position = vec4(aPos.x / uView.x * 2.0 - 1.0, 1.0 - aPos.y / uView.y * 2.0, 0.0, 1.0); }`;
  // 形状: 0=角丸矩形の塗り＋線（Renderer::drawRect）, 1=グロー（GLOW_FRAG）, 2=円, 3=線分（drawLine）
  const SHAPE = `#version 300 es
precision highp float; in vec2 vPos; out vec4 o;
uniform int uMode; uniform vec2 uCenter; uniform vec2 uHalf; uniform float uRadius;
uniform vec4 uFill; uniform vec4 uStroke; uniform float uLw; uniform float uGlowW; uniform vec2 uA; uniform vec2 uB;
float rr(vec2 p){ vec2 q = abs(p - uCenter) - uHalf + vec2(uRadius); return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uRadius; }
void main(){
  if (uMode == 0) {
    float d = rr(vPos); float aa = max(fwidth(d), 0.5);
    float fillA = 1.0 - smoothstep(-aa, aa, d);
    float strokeA = (1.0 - smoothstep(-aa, aa, abs(d + uLw * 0.5) - uLw * 0.5));
    vec4 c = uFill * fillA; c = uStroke * strokeA + c * (1.0 - uStroke.a * strokeA); o = c;
  } else if (uMode == 1) {
    float d = rr(vPos);
    float outer = 1.0 - clamp(d / uGlowW, 0.0, 1.0); outer *= outer;
    float inner = smoothstep(-2.0, 0.0, d);
    float a = uFill.a * outer * inner; o = vec4(uFill.rgb * a, a);
  } else if (uMode == 2) {
    float d = length(vPos - uCenter) - uRadius; float aa = max(fwidth(d), 0.5);
    o = uFill * (1.0 - smoothstep(-aa, aa, d));
  } else {
    vec2 pa = vPos - uA, ba = uB - uA; float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    float d = length(pa - ba * h) - uLw * 0.5; float aa = max(fwidth(d), 0.5);
    o = uFill * (1.0 - smoothstep(-aa, aa, d));
  }
}`;
  const TEX = `#version 300 es
precision highp float; in vec2 vPos; out vec4 o;
uniform sampler2D uTex; uniform vec4 uRect; uniform float uRadius; uniform float uAlpha; uniform vec4 uUv;
void main(){
  vec2 c = uRect.xy + uRect.zw * 0.5; vec2 q = abs(vPos - c) - uRect.zw * 0.5 + vec2(uRadius);
  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uRadius; float aa = max(fwidth(d), 0.5);
  float m = 1.0 - smoothstep(-aa, aa, d);
  vec2 uv = uUv.xy + (vPos - uRect.xy) / uRect.zw * uUv.zw;
  o = texture(uTex, uv) * (m * uAlpha);
}`;
  function compile(type, src) {
    const s = gl.createShader(type); gl.shaderSource(s, src); gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) { const log = gl.getShaderInfoLog(s); console.error('shader error', log); throw new Error(log); }
    return s;
  }
  function program(fs) {
    const p = gl.createProgram(); gl.attachShader(p, compile(gl.VERTEX_SHADER, VERT)); gl.attachShader(p, compile(gl.FRAGMENT_SHADER, fs));
    gl.bindAttribLocation(p, 0, 'aPos'); gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
    const u = {}; const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < n; i++) { const info = gl.getActiveUniform(p, i); u[info.name] = gl.getUniformLocation(p, info.name); }
    return {p, u};
  }
  const glassSrc = def => `#version 300 es\n${def ? `#define ${def}\n` : ''}` + GLASS_FRAG_SRC;
  const P = {
    shape: program(SHAPE), tex: program(TEX),

  };
  // ガラスのシェーダーは大きいので、使うスタイルの分だけ必要になった時にコンパイルする
  const GLASS_DEF = {std: '', carved: 'CARVED_GLASS', gummy: 'GUMMY_GLASS'};
  for (const k in GLASS_DEF) { let cache = null; Object.defineProperty(P, k, {get: () => cache || (cache = program(glassSrc(GLASS_DEF[k])))}); }
  const vbo = gl.createBuffer(), vao = gl.createVertexArray();
  gl.bindVertexArray(vao); gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
  gl.bufferData(gl.ARRAY_BUFFER, 8 * 4, gl.DYNAMIC_DRAW);
  gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
  const quadBuf = new Float32Array(8);
  function quad(x0, y0, x1, y1) { quadBuf.set([x0, y0, x1, y0, x0, y1, x1, y1]); gl.bufferSubData(gl.ARRAY_BUFFER, 0, quadBuf); gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4); }

  function texFrom(src, {mip = false} = {}) {
    const t = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, t);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, src);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    if (mip) { gl.generateMipmap(gl.TEXTURE_2D); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR); }
    else gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    return t;
  }
  function texUpdate(t, src, mip = false) {
    gl.bindTexture(gl.TEXTURE_2D, t); gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, src);
    if (mip) gl.generateMipmap(gl.TEXTURE_2D);
  }
  const dummy = (() => { const c = document.createElement('canvas'); c.width = c.height = 1; return texFrom(c); })();
  // グミ: 中身を 64×64 に縮めてミップを作った小テクスチャ（GlassHighlightEffect::renderGummySmall 相当）
  const smallCv = document.createElement('canvas'); smallCv.width = smallCv.height = 64;
  const sctx = smallCv.getContext('2d');
  function makeSmall(src, sx, sy, sw, sh, tex) {
    sctx.clearRect(0, 0, 64, 64); sctx.imageSmoothingEnabled = true; sctx.imageSmoothingQuality = 'high';
    sctx.drawImage(src, sx, sy, sw, sh, 0, 0, 64, 64);
    if (tex) { texUpdate(tex, smallCv, true); return tex; }
    return texFrom(smallCv, {mip: true});
  }

  const view = () => { gl.viewport(0, 0, cv.width, cv.height); };
  function begin() {
    view(); gl.clearColor(0, 0, 0, 0); gl.clear(gl.COLOR_BUFFER_BIT);
    gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.bindVertexArray(vao); gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
  }
  const pm = (r, g, b, a) => [r / 255 * a / 255, g / 255 * a / 255, b / 255 * a / 255, a / 255];
  function useShape() { gl.useProgram(P.shape.p); gl.uniform2f(P.shape.u.uView, VW, VH); return P.shape.u; }
  function rect(x, y, w, h, radius, fill, stroke, lw) {
    const u = useShape(); gl.uniform1i(u.uMode, 0);
    gl.uniform2f(u.uCenter, x + w / 2, y + h / 2); gl.uniform2f(u.uHalf, w / 2, h / 2); gl.uniform1f(u.uRadius, Math.min(radius, w / 2, h / 2));
    gl.uniform4fv(u.uFill, fill); gl.uniform4fv(u.uStroke, stroke); gl.uniform1f(u.uLw, lw);
    quad(x - 2, y - 2, x + w + 2, y + h + 2);
  }
  function glow(x, y, w, h, radius, color, gw) {
    if (color[3] <= 0 || gw <= 0) return;
    const u = useShape(); gl.uniform1i(u.uMode, 1);
    gl.uniform2f(u.uCenter, x + w / 2, y + h / 2); gl.uniform2f(u.uHalf, w / 2, h / 2); gl.uniform1f(u.uRadius, Math.min(radius, w / 2, h / 2));
    gl.uniform4fv(u.uFill, color); gl.uniform1f(u.uGlowW, gw);
    quad(x - gw, y - gw, x + w + gw, y + h + gw);
  }
  function circle(cx, cy, r, fill) {
    const u = useShape(); gl.uniform1i(u.uMode, 2); gl.uniform2f(u.uCenter, cx, cy); gl.uniform1f(u.uRadius, r); gl.uniform4fv(u.uFill, fill);
    quad(cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2);
  }
  function line(ax, ay, bx, by, w, fill) {
    const u = useShape(); gl.uniform1i(u.uMode, 3); gl.uniform2f(u.uA, ax, ay); gl.uniform2f(u.uB, bx, by); gl.uniform1f(u.uLw, w); gl.uniform4fv(u.uFill, fill);
    quad(Math.min(ax, bx) - w - 2, Math.min(ay, by) - w - 2, Math.max(ax, bx) + w + 2, Math.max(ay, by) + w + 2);
  }
  function image(tex, x, y, w, h, radius = 0, alpha = 1, uv = [0, 0, 1, 1]) {
    const u = P.tex.u; gl.useProgram(P.tex.p); gl.uniform2f(u.uView, VW, VH);
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, tex); gl.uniform1i(u.uTex, 0);
    gl.uniform4f(u.uRect, x, y, w, h); gl.uniform1f(u.uRadius, Math.min(radius, w / 2, h / 2)); gl.uniform1f(u.uAlpha, alpha); gl.uniform4fv(u.uUv, uv);
    quad(x - 1, y - 1, x + w + 1, y + h + 1);
  }
  function multiply(on) { if (on) gl.blendFunc(gl.DST_COLOR, gl.ONE_MINUS_SRC_ALPHA); else gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA); }

  // GlassHighlightEffect::draw 相当
  function glass(style, o) {
    const pr = P[style === 'standard' ? 'std' : style], u = pr.u; gl.useProgram(pr.p); gl.uniform2f(u.uView, VW, VH);
    const set1 = (n, v) => { if (u[n]) gl.uniform1f(u[n], v); };
    const set2 = (n, a, b) => { if (u[n]) gl.uniform2f(u[n], a, b); };
    const [bx, by, bw, bh] = o.board;
    const hw = bw / 2, hh = bh / 2;
    set2('uCenter', bx + hw, by + hh); set2('uHalfSize', hw, hh);
    set1('uRadius', Math.min(Math.max(0, o.radius), hw, hh));
    set1('uBezelPx', Math.max(4, o.bezel)); set1('uContentRadius', Math.max(0, o.contentRadius));
    set1('uNormalScalePx', Math.max(4, o.bezel)); set1('uLightAngle', o.lightAngle); set1('uIntensity', o.intensity);
    set1('uInnerMix', o.innerMix);
    gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, dummy);
    gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, o.content || dummy);
    gl.activeTexture(gl.TEXTURE3); gl.bindTexture(gl.TEXTURE_2D, o.small || dummy);
    gl.activeTexture(gl.TEXTURE4); gl.bindTexture(gl.TEXTURE_2D, dummy);
    if (u.uCapture) gl.uniform1i(u.uCapture, 1);
    if (u.uContent) gl.uniform1i(u.uContent, 2);
    if (u.uGummySmall) gl.uniform1i(u.uGummySmall, 3);
    if (u.uGummyCaptureSmall) gl.uniform1i(u.uGummyCaptureSmall, 4);
    if (u.uContentUvRect) gl.uniform4fv(u.uContentUvRect, o.uv || [0, 0, 1, 1]);
    set1('uContentReflection', o.content ? 1 : 0);
    set2('uCaptureTextureSize', 1, 1); set2('uCaptureOrigin', 0, 0); set2('uCaptureSourceSize', 1, 1); set2('uCaptureDataSize', 1, 1);
    set1('uDpr', RES); set1('uRefractMix', 0); set1('uRefraction', 0); set1('uEdgeBoost', 0); set1('uChromatic', .15);
    set1('uRefractionPrepass', 0); set1('uDebugNormal', 0);
    set1('uSurfaceProfile', o.launcher ? 1 : 0); set1('uNormalInfluence', o.launcher ? 2.2 : 1);
    if (u.uTint) gl.uniform4f(u.uTint, 1, 1, 1, 1);
    set1('uCaptureTint', 0); set1('uGummySmallValid', o.small ? 1 : 0);
    set1('uFuseK', 0); set1('uPrepassHoleAmount', 0);
    set1('uDarkK', typeof GLASS_DARK_K === 'number' ? GLASS_DARK_K : 1); set1('uAmbientLift', typeof GUMMY_AMBIENT_LIFT === 'number' ? GUMMY_AMBIENT_LIFT : 0);
    const pad = Math.max(2, o.bezel * .95);
    quad(bx - pad, by - pad, bx + bw + pad, by + bh + pad);
    gl.activeTexture(gl.TEXTURE0);
  }
  return {cv, gl, resize, begin, rect, glow, circle, line, image, glass, multiply, texFrom, texUpdate, makeSmall, pm};
};
