const ISLAND_ACCENT = '#0067C0';   // アプリのアクセント色（Windowsのアクセント or 設定値）
const UI_K = 1.5;                  // 論理px → 映像px
function hexRgb(h){const n=parseInt(h.slice(1),16);return [(n>>16)&255,(n>>8)&255,n&255];}
function rgb2hsv([r,g,b]){r/=255;g/=255;b/=255;const mx=Math.max(r,g,b),mn=Math.min(r,g,b),d=mx-mn;let h=0;
  if(d){h=mx===r?((g-b)/d)%6:mx===g?(b-r)/d+2:(r-g)/d+4;h*=60;if(h<0)h+=360;}return [h,mx?d/mx:0,mx];}
function hsv2rgb(h,s,v){const c=v*s,x=c*(1-Math.abs((h/60)%2-1)),m=v-c;let r=0,g=0,b=0;
  if(h<60)[r,g,b]=[c,x,0];else if(h<120)[r,g,b]=[x,c,0];else if(h<180)[r,g,b]=[0,c,x];else if(h<240)[r,g,b]=[0,x,c];else if(h<300)[r,g,b]=[x,0,c];else[r,g,b]=[c,0,x];
  return [Math.round((r+m)*255),Math.round((g+m)*255),Math.round((b+m)*255)];}
const rgba=(c,a)=>`rgba(${c[0]},${c[1]},${c[2]},${(a/255).toFixed(4)})`;
const W8=a=>rgba([255,255,255],a);
function shadeOf(hex,vk){const [h,s,v]=rgb2hsv(hexRgb(hex));return hsv2rgb(h,Math.min(1,s*1.10),v*vk);}
function grad(ctx,x0,y0,x1,y1,stops){const g=ctx.createLinearGradient(x0,y0,x1,y1);stops.forEach(([o,c])=>g.addColorStop(o,c));return g;}
function rgrad(ctx,x,y,r,stops){const g=ctx.createRadialGradient(x,y,0,x,y,r);stops.forEach(([o,c])=>g.addColorStop(o,c));return g;}
function pillPath(l,t,r_,b,rad){const p=new Path2D();p.moveTo(l+rad,t);p.lineTo(r_-rad,t);p.arc(r_-rad,t+rad,rad,-Math.PI/2,0);p.lineTo(r_,b-rad);
  p.arc(r_-rad,b-rad,rad,0,Math.PI/2);p.lineTo(l+rad,b);p.arc(l+rad,b-rad,rad,Math.PI/2,Math.PI);p.lineTo(l,t+rad);p.arc(l+rad,t+rad,rad,Math.PI,Math.PI*1.5);p.closePath();return p;}

function paintIslandBody(ctx, IW, IH, accentHex=ISLAND_ACCENT){
  const L=1, T=1, R=IW-1, B=IH-1;
  const rad=Math.min(IH/2,(B-T)/2,(R-L)/2);
  const path=pillPath(L,T,R,B,rad);
  const acc=hexRgb(accentHex), shade=shadeOf(accentHex,.42);
  // 本体: アクセント色 α165
  ctx.fillStyle=rgba(acc,165); ctx.fill(path);
  const innerH=B-T, lightSpan=Math.min(IH*.95,innerH*.36), s1=Math.min(.34,(IH*.62)/innerH), s2=1-s1;
  const span=IH*.045;
  const bands=(count,style,clip)=>{ctx.save();if(clip){ctx.beginPath();ctx.rect(...clip);ctx.clip();}
    ctx.strokeStyle=style;for(let i=0;i<Math.min(count,12);i++){ctx.lineWidth=span*2*(1-i/12);ctx.stroke(path);}ctx.restore();};
  ctx.save(); ctx.clip(path);
  ctx.fillStyle=W8(10); ctx.fillRect(0,0,IW,IH);           // フロスト
  ctx.globalCompositeOperation='lighter';
  bands(12,grad(ctx,0,T,0,T+lightSpan,[[0,W8(14)],[.45,W8(4)],[1,W8(0)]]),[L,T-2,R-L,lightSpan+4]);
  bands(12,grad(ctx,0,B-lightSpan,0,B,[[0,W8(0)],[.55,W8(3)],[1,W8(9)]]),[L,B-lightSpan-2,R-L,lightSpan+4]);
  const dx=(R-L)*.55, dy=innerH*.55;
  bands(12,grad(ctx,L,T,L+dx,T+dy,[[0,W8(20)],[.55,W8(3)],[1,W8(0)]]));
  bands(12,grad(ctx,R-dx,B-dy,R,B,[[0,W8(0)],[.45,W8(2)],[1,W8(11)]]));
  ctx.globalCompositeOperation='multiply';
  bands(8,grad(ctx,0,T,0,B,[[0,rgba(shade,0)],[s1,rgba(shade,30)],[s2,rgba(shade,30)],[1,rgba(shade,0)]]));
  ctx.restore();
  ctx.save(); ctx.globalCompositeOperation='multiply';
  ctx.strokeStyle=grad(ctx,0,T,0,B,[[0,rgba(shade,120)],[s1,rgba(shade,230)],[s2,rgba(shade,230)],[1,rgba(shade,190)]]);
  ctx.lineWidth=1; ctx.stroke(path); ctx.restore();
}
function drawIsland(cv, IWp=240, buttons=true){
  const ctx=cv.getContext('2d');
  const s=cv.width/420*UI_K;           // canvas px per logical px
  ctx.setTransform(1,0,0,1,0,0); ctx.clearRect(0,0,cv.width,cv.height);
  ctx.setTransform(s,0,0,s,30*cv.width/420+(240-IWp)/2*s,30*cv.width/420);
  const IW=IWp, IH=42;
  paintIslandBody(ctx, IW, IH);
  // ボタン（中心X: 21 / w-95 / w-58 / w-21）
  if(buttons) [['#4A90E2','+',21],['#28C940','-',IW-95],['#FFBD2E','square',IW-58],['#FF5F57','x',IW-21]].forEach(([c,ic,x],i)=>{
    const sc=Array.isArray(buttons)?buttons[i]:1; if(sc<.01) return;
    ctx.save(); ctx.translate(x,IH/2); ctx.scale(sc,sc); ctx.translate(-x,-IH/2); drawIslandButton(ctx,c,ic,x,IH/2); ctx.restore(); });
}
function drawIslandButton(ctx,hex,icon,cx,cy){
  const r=12, base=hexRgb(hex);
  const shade=shadeOf(hex,.52);
  const [h,sa,v]=rgb2hsv(base); const tint=hsv2rgb(h,Math.min(1,sa*1.04),Math.min(1,v*1.20+10/255));
  // 落ち影
  const sr=r*1.28, sy=cy+r*.16;
  ctx.fillStyle=rgrad(ctx,cx,sy,sr,[[0,'rgba(0,0,0,.259)'],[.6,'rgba(0,0,0,.11)'],[1,'rgba(0,0,0,0)']]);
  ctx.beginPath();ctx.arc(cx,sy,sr,0,Math.PI*2);ctx.fill();
  const box=[cx-r,cy-r,r*2,r*2];
  ctx.save();ctx.beginPath();ctx.arc(cx,cy,r,0,Math.PI*2);ctx.clip();
  ctx.fillStyle=W8(10);ctx.fillRect(...box);
  ctx.fillStyle=rgba(tint,192);ctx.fillRect(...box);
  ctx.globalCompositeOperation='lighter';
  ctx.fillStyle=grad(ctx,cx-r,cy-r,cx+r,cy+r,[[0,W8(13)],[.55,W8(3)],[1,W8(0)]]);ctx.fillRect(...box);
  ctx.fillStyle=rgrad(ctx,cx,cy,r,[[0,W8(0)],[.38,W8(0)],[.68,W8(4)],[.86,W8(12)],[.96,W8(22)],[1,W8(18)]]);ctx.fillRect(...box);
  ctx.fillStyle=rgrad(ctx,cx,cy-r*1.02,r*.95,[[0,W8(22)],[.45,W8(7)],[1,W8(0)]]);ctx.fillRect(...box);
  ctx.fillStyle=rgrad(ctx,cx,cy+r*1.06,r*.80,[[0,W8(16)],[.45,W8(5)],[1,W8(0)]]);ctx.fillRect(...box);
  ctx.globalCompositeOperation='multiply';
  for(const side of [-1,1]){ctx.fillStyle=rgrad(ctx,cx+side*r*.92,cy,r*.95,[[0,rgba(shade,70)],[.45,rgba(shade,26)],[1,rgba(shade,0)]]);ctx.fillRect(...box);}
  ctx.restore();
  ctx.save();ctx.globalCompositeOperation='multiply';ctx.lineWidth=.9;
  ctx.strokeStyle=grad(ctx,0,cy-r,0,cy+r,[[0,rgba(shade,120)],[.5,rgba(shade,230)],[1,rgba(shade,190)]]);
  ctx.beginPath();ctx.arc(cx,cy,r-.35,0,Math.PI*2);ctx.stroke();
  ctx.globalCompositeOperation='lighter';
  ctx.strokeStyle=grad(ctx,0,cy-r,0,cy+r,[[0,W8(30)],[.32,W8(11)],[.68,W8(10)],[1,W8(24)]]);
  ctx.beginPath();ctx.arc(cx,cy,r-.85,0,Math.PI*2);ctx.stroke();
  ctx.restore();
  // アイコン（白）。閉じる/全画面/最小化は image/svg の 32×32 を 30px で描画
  ctx.save();ctx.fillStyle='#fff';ctx.strokeStyle='#fff';
  if(icon==='+'){ctx.lineWidth=2;ctx.lineCap='round';ctx.beginPath();ctx.moveTo(cx-4,cy);ctx.lineTo(cx+4,cy);ctx.moveTo(cx,cy-4);ctx.lineTo(cx,cy+4);ctx.stroke();}
  else{
    ctx.translate(cx-15,cy-15);ctx.scale(30/32,30/32);
    if(icon==='-'){ctx.beginPath();ctx.roundRect(16-10.351/2,16-2.227/2,10.351,2.227,.79);ctx.fill();}
    if(icon==='square'){const o=new Path2D('M10.645,12.705v6.589c0,1.138.922,2.06,2.06,2.06h6.589c1.138,0,2.06-.922,2.06-2.06v-6.589c0-1.138-.922-2.06-2.06-2.06h-6.589c-1.138,0-2.06.922-2.06,2.06ZM19.192,19.741h-6.385c-.303,0-.548-.245-.548-.548v-6.385c0-.303.245-.548.548-.548h6.385c.303,0,.548.245.548.548v6.385c0,.303-.245.548-.548.548Z');ctx.fill(o);}
    if(icon==='x'){for(const a of [45,-45]){ctx.save();ctx.translate(16,16);ctx.rotate(a*Math.PI/180);ctx.beginPath();ctx.roundRect(-2.227/2,-12.331/2,2.227,12.331,1.114);ctx.fill();ctx.restore();}}
  }
  ctx.restore();
}

