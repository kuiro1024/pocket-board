/* 背景の虹色のもやを、ゆっくり流れるように動かす（背景画像をノイズで歪ませて描く） */
function startFlow(cv, imgUrl) {
  const gl = cv.getContext('webgl', {alpha: false, antialias: false, premultipliedAlpha: false});
  if (!gl) return false;
  const VS = 'attribute vec2 p;varying vec2 v;void main(){v=p*.5+.5;gl_Position=vec4(p,0.,1.);}';
  const FS = `precision mediump float;varying vec2 v;uniform sampler2D T;uniform float t;uniform vec2 R;uniform vec2 I;
  vec3 h3(vec3 p){p=vec3(dot(p,vec3(127.1,311.7,74.7)),dot(p,vec3(269.5,183.3,246.1)),dot(p,vec3(113.5,271.9,124.6)));return -1.+2.*fract(sin(p)*43758.5453);}
  float n3(vec3 p){vec3 i=floor(p),f=fract(p),u=f*f*(3.-2.*f);
    return mix(mix(mix(dot(h3(i),f),dot(h3(i+vec3(1,0,0)),f-vec3(1,0,0)),u.x),mix(dot(h3(i+vec3(0,1,0)),f-vec3(0,1,0)),dot(h3(i+vec3(1,1,0)),f-vec3(1,1,0)),u.x),u.y),
               mix(mix(dot(h3(i+vec3(0,0,1)),f-vec3(0,0,1)),dot(h3(i+vec3(1,0,1)),f-vec3(1,0,1)),u.x),mix(dot(h3(i+vec3(0,1,1)),f-vec3(0,1,1)),dot(h3(i+vec3(1,1,1)),f-vec3(1,1,1)),u.x),u.y),u.z);}
  void main(){
    // cover 配置
    float ra=R.x/R.y, ia=I.x/I.y; vec2 uv=v; uv.y=1.-uv.y;
    if(ra>ia){uv.y=(uv.y-.5)*ia/ra+.5;}else{uv.x=(uv.x-.5)*ra/ia+.5;}
    vec2 q=uv*vec2(ra,1.)*1.3; float s=t*.32;
    vec2 w=vec2(n3(vec3(q,s)),n3(vec3(q+5.2,s+1.7)));
    vec2 w2=vec2(n3(vec3(q*1.7+w*1.2+3.1,s*1.3)),n3(vec3(q*1.7+w*1.2+8.3,s*1.3+2.)));
    vec2 d=(w*.2+w2*.12) + vec2(sin(t*.23),cos(t*.19))*.07;
    uv=(uv-.5)*(.7+.05*sin(t*.15))+.5+d;                        // 端が見えないよう少し拡大
    vec2 u2=uv+vec2(sin(s*1.3),cos(s*1.1))*.04;
    uv=1.-abs(1.-mod(uv,2.)); u2=1.-abs(1.-mod(u2,2.));   // 端は鏡映しで折り返す
    vec3 c=texture2D(T,uv).rgb;
    vec3 c2=texture2D(T,u2).rgb;
    vec3 col=mix(c,c2,.5);
    // ゆっくり漂う光のかたまり（背景の色を拾って重ねる）で、流れを目で追えるようにする
    vec2 p=v*vec2(ra,1.);
    for(int i=0;i<4;i++){
      float fi=float(i);
      vec2 ctr=vec2(.5*ra,.5)+vec2(sin(t*(.13+fi*.037)+fi*1.7)*.42*ra, cos(t*(.11+fi*.029)+fi*2.3)*.38);
      vec3 bc=texture2D(T,fract(vec2(.2+fi*.21,.3+fi*.17)+vec2(sin(t*.05+fi),cos(t*.04+fi))*.15)).rgb;
      bc=clamp(mix(vec3(dot(bc,vec3(.333))),bc,1.6),0.,1.);
      float r=.30+.08*sin(t*.2+fi);
      float a=exp(-dot(p-ctr,p-ctr)/(r*r))*.42;
      col=1.-(1.-col)*(1.-bc*a);
    }
    gl_FragColor=vec4(col,1.);
  }`;
  const sh = (ty, s) => { const o = gl.createShader(ty); gl.shaderSource(o, s); gl.compileShader(o); return o; };
  const pr = gl.createProgram(); gl.attachShader(pr, sh(gl.VERTEX_SHADER, VS)); gl.attachShader(pr, sh(gl.FRAGMENT_SHADER, FS)); gl.linkProgram(pr);
  if (!gl.getProgramParameter(pr, gl.LINK_STATUS)) return false;
  gl.useProgram(pr);
  const b = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, b); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
  const loc = gl.getAttribLocation(pr, 'p'); gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
  const uT = gl.getUniformLocation(pr, 't'), uR = gl.getUniformLocation(pr, 'R'), uI = gl.getUniformLocation(pr, 'I');
  const tex = gl.createTexture(); let iw = 1, ih = 1, ready = false;
  const img = new Image(); img.onload = () => {
    gl.bindTexture(gl.TEXTURE_2D, tex); gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB, gl.RGB, gl.UNSIGNED_BYTE, img);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    iw = img.width; ih = img.height; ready = true; frame(performance.now());
  };
  img.src = imgUrl;
  const still = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const SCALE = .35;   // もや自体がぼけているので低解像度で十分
  function fit() { const w = Math.max(1, Math.round(innerWidth * SCALE)), h = Math.max(1, Math.round(innerHeight * SCALE)); if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; } }
  const t0 = performance.now();
  function frame(now) {
    if (!ready) return;
    fit(); gl.viewport(0, 0, cv.width, cv.height);
    gl.uniform1f(uT, still ? 0 : (now - t0) / 1000); gl.uniform2f(uR, cv.width, cv.height); gl.uniform2f(uI, iw, ih);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    if (!still) requestAnimationFrame(frame);
  }
  addEventListener('resize', () => { if (still) frame(performance.now()); });
  return true;
}
