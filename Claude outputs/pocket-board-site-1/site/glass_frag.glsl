precision highp float;
precision highp sampler2D;
vec4 fragColor;
vec4 fragCover;
out vec4 outColor;
uniform float uDarkK;
uniform float uAmbientLift;

in vec2 vPos;
// dual-source blending(GL3.3コア)。fragColorが加算される色、fragCoverの
// アルファが「dstをどれだけ退かせるか」。通常のブレンドだと置き換え量と
// アルファ書き込み量が連動してしまい、半透明のボード上で屈折の
// 「サンプルによる置き換え」が恒等にならず白浮きする(2026-07-19に実機で
// 確認)。ブレンド式は draw() 側で GL_ONE / GL_ONE_MINUS_SRC1_ALPHA。
uniform vec2      uCenter;
uniform vec2      uHalfSize;
uniform float     uRadius;
uniform float     uBezelPx;     // 枠（board縁〜コンテンツ縁）の厚み(px)
uniform float     uContentRadius; // 実際のコンテンツ角丸半径(px)
uniform float     uNormalScalePx; // 屈折法線の深さ基準(px)
uniform float     uLightAngle;  // ラジアン。バブル中心→光源の方向
uniform float     uIntensity;   // 0〜1
uniform float     uInnerMix;    // 内側レイヤー(innerGlow/innerLine)の強さ0〜1。
                                // ランチャー等の小さな正方形バブルでは内側の線が
                                // 「四角の中の四角」に見えて不自然なため0にする
uniform sampler2D uContent;     // 現在表示中の動画/画像。Quality色反射用
uniform vec4      uContentUvRect; // xy=左上、zw=幅/高さ(Rendererと同じUV系)
uniform float     uContentReflection; // 0=無効、1=Qualityのコンテンツ色反射
uniform sampler2D uCapture;     // 小領域キャプチャ(ネイティブ解像度、premultiplied)
uniform vec2      uCaptureTextureSize; // キャプチャテクスチャの確保容量
uniform vec2      uCaptureOrigin;      // 元FBO内の小領域左下(物理px)
uniform vec2      uCaptureSourceSize;  // 元FBO上での小領域サイズ(物理px)
uniform vec2      uCaptureDataSize;    // キャプチャ側へ書き込んだサイズ
uniform float     uDpr;         // 論理px→物理pxの倍率
uniform float     uRefractMix;  // キャプチャ疑似屈折の強さ0〜1(品質=Qualityのみ>0)
uniform float     uRefraction;  // 固定法線から算出した変位へ掛ける強度
uniform float     uEdgeBoost;   // 最終変位の外周成分だけを追加増幅
uniform float     uChromatic;   // R/Bサンプル距離の±差
uniform float     uRefractionPrepass; // >0.5: 全面屈折だけを描く背景プリパス
uniform float     uDebugNormal; // Shift+Dデバッグ。0:通常 1:法線表示 2:屈折レイヤー単体表示
uniform float     uSurfaceProfile; // 0=動画/画像、1=ランチャー専用の外周集中型
uniform float     uNormalInfluence; // プロファイル別の法線変位倍率
uniform vec4      uTint;
uniform sampler2D uGummySmall;        // 2026-10-11 グミ: 中身を64×64へ縮めた小テクスチャ（ミップ付き・premultiplied）
uniform float     uGummySmallValid;   // グミ: uGummySmall が使えるとき1
uniform sampler2D uGummyCaptureSmall; // グミ: 背景キャプチャを64×64へ縮めた小テクスチャ（ミップ付き）
uniform float     uCaptureTint; // 2026-10-11 グミ: 掴んでいる間、背景キャプチャの色をゲルへ混ぜる強さ(0〜1)        // uTint.aは全体強度の追加乗数として使う
// 2026-09-22: バブル融合（GlassHighlightEffect::Fusion）。uFuseK=0 で無効。
uniform float     uFuseK;             // smin の幅（px）
uniform vec2      uFuseCenter;        // 相手の板の中心
uniform vec2      uFuseHalfSize;      // 相手の板の半サイズ
uniform float     uFuseRadius;        // 相手の板の角丸半径
uniform float     uFuseLightAngle;    // 相手のライト角度（首の部分で補間する）
uniform float     uFuseBezelPx;       // 相手のベゼル幅
uniform float     uFuseContentRadius; // 相手のコンテンツ角丸半径
// 2026-09-23: 屈折プリパスの穴（Fusion::prepassHole）。uPrepassHoleAmount=0 で無効。
uniform vec2      uPrepassHoleCenter;
uniform vec2      uPrepassHoleHalf;
uniform float     uPrepassHoleAmount;

vec2 captureUv(vec2 physicalPos) {
    vec2 local = clamp(
        (physicalPos - uCaptureOrigin) / max(uCaptureSourceSize, vec2(1.0)),
        0.0, 1.0);
    // 書き込み済み範囲の最初〜最後のテクセル中心へ写像する。確保容量を
    // 丸めて余白がある場合でも、未書き込み部分を線形補間で拾わない。
    vec2 texel = local * max(uCaptureDataSize - vec2(1.0), vec2(0.0))
               + vec2(0.5);
    return texel / max(uCaptureTextureSize, vec2(1.0));
}

// ランチャーの角丸率に応じた屈折法線の深さプロファイル。
// 小さいバブル（角丸率が高い）は広く柔らかく、大きいバブル
// （角丸率が低い）は狭く締まる。
float launcherBendProfile(float depth, float cornerRatio) {
    float roundness = smoothstep(0.12, 0.42, cornerRatio);
    float broadWeight = mix(0.16, 0.30, roundness);
    float sharpDecay = mix(5.2, 3.9, roundness);
    float broadDecay = mix(2.3, 1.55, roundness);
    return (1.0 - broadWeight) * exp(-sharpDecay * depth)
         + broadWeight * exp(-broadDecay * depth);
}

#ifdef CARVED_GLASS
// ---- 削り出しの断面（2026-09-16）------------------------------------------
// 面取りの帯の位置と幅。ライティング・反射・屈折・法線表示で同じものを使う。
const float kCarveOuterRatio = 0.18;  // 外周の面取りの幅（ベゼル幅に対する割合）
const float kCarveInnerRatio = 0.14;  // 内側の面取りの幅
const float kCarveSoft       = 0.30;  // 折れ目の丸み（面取りの幅に対する割合）
// 屈折用の面の傾き（標準の bend と同じ尺度。標準は外周で1.0から内側へ減衰）。
//   外周の面取り … 外へ傾いた平らな斜面（一定の傾き）
//   ベゼルの面   … 2026-09-16: 面取りだけではゆがみがほとんど感じられなかったため、
//                  ベゼルにも外向きの傾きを持たせた。外周側の折れ目で最も強く
//                  （kCarveTiltFlat）、内側の面取りへ向かって0まで弱まる。
//                  kCarveTiltFlatCurve で弱まり方のカーブを変えられる
//                  （1=直線、大きいほど外周寄りに集中）。
//   内側の面取り … コンテンツへ落ちる斜面（傾きの向きが逆＝負の値）
//   コンテンツ   … 傾き0（屈折しない）
const float kCarveTiltOuter     = 0.85;
// 2026-09-16: 0.55 / 1.4 では面取りの折れ目が埋もれて削り出しらしさが消えたため、
// ほんのり乗る程度へ弱めた（面取りの 0.85 との段差がはっきり残る）。
// さらに調整: 外周の折れ目のすぐ内側だけ強め（0.45）、カーブを急にして（3.0）
// すぐ弱まるようにした。ベゼルの中ほど（u=0.5）での強さは 0.20/2.0 のときと
// ほぼ同じ（0.05→0.056）なので、減衰の感じは変えずに外周付近だけが強くなる。
const float kCarveTiltFlat      = 0.45;
const float kCarveTiltFlatCurve = 3.0;
const float kCarveTiltInner     = 0.60;

// d: 板の外周からの距離（内側が負） / dContent: コンテンツの縁からの距離
// px: 1物理ピクセルぶんの論理px（fwidthで求めて渡す）
void carveBands(float d, float dContent, float bez, float sideMask, float px,
                out float bandO, out float bandI, out float wO, out float wI)
{
    // 寄りすぎ・引きすぎでも破綻しないよう、1画素ぶんだけは下限にする
    wO = max(bez * kCarveOuterRatio, px);
    wI = max(bez * kCarveInnerRatio, px);
    float softO = max(wO * kCarveSoft, px);
    float softI = max(wI * kCarveSoft, px);
    // 帯の中にいるか。折れ目の前後 soft の範囲だけなめらかに切り替える。
    bandO = 1.0 - smoothstep(wO - 0.5 * softO, wO + 0.5 * softO, -d);
    bandI = (1.0 - smoothstep(wI - 0.5 * softI, wI + 0.5 * softI, dContent)) * sideMask;
}

// 面取りの断面から求めた屈折の傾き（符号付き。正=外向き、負=内向き）
// sideMask: コンテンツの外（ベゼル側）なら1
float carveBend(float d, float bez, float px, float bandO, float bandI,
                float sideMask, float innerMix)
{
    float wO = max(bez * kCarveOuterRatio, px);
    float wI = max(bez * kCarveInnerRatio, px);
    // ベゼルの面での位置。外周側の折れ目で1、内側の面取りの折れ目で0。
    float flatSpan = max(bez - wO - wI, px);
    float u = clamp((bez - wI - (-d)) / flatSpan, 0.0, 1.0);
    float flatTilt = kCarveTiltFlat * pow(u, kCarveTiltFlatCurve);
    float flatMask = (1.0 - bandO) * (1.0 - bandI) * sideMask;
    return bandO * kCarveTiltOuter
         + flatMask * flatTilt
         - bandI * kCarveTiltInner * innerMix;
}
#endif

#ifdef GUMMY_GLASS
// ---- グミの断面と粒（2026-10-11）-------------------------------------------
// 調整値は Claude outputs/gummy-glass-lab.html で決めて、この範囲を丸ごと置き換える。
// 2026-10-11: ラボで決めた値（ランチャーは後で別途調整予定）。
// @@GUMMY_PARAMS_BEGIN
const float kGummyDome        = 1.6;  // 外周の膨らみの幅（ベゼル幅に対する割合）
const float kGummyDomePow     = 3.7;  // 膨らみのカーブ（大きいほど縁で急に立ち上がる）
const float kGummyPuff        = 0.71;  // 膨らみの高さ（ベゼル幅に対する割合）
const float kGummyWell        = 0.45;  // 内側のくぼみの幅（ベゼル幅に対する割合）
const float kGummyWellDepth   = 0.38;  // くぼみの深さ（0で平ら）
const float kGummyMilk        = 0.95;  // 乳白の濃さ
const float kGummyEdgeClear   = 1.0;  // 縁の濃さ（膨らみの頂上に対する比。小さいほど縁が透ける）
const float kGummyAmbient     = 0.5;  // 陰になった面の明るさ
const float kGummyBleed       = 0.79;  // コンテンツの色のにじみ（Qualityのみ）
const float kGummyBleedDepth  = 1.02;  // にじみの色を拾う深さ（ベゼル幅に対する割合）
const float kGummyBleedFar    = 0.7;   // 外周側を画面全体の平均色へ寄せる量
const float kGummyBleedSat    = 1.4;   // にじむ色・根本の光の彩度（1=中身のまま）
const float kGummyRootGlow    = 0.6;   // 根本（中身の縁のすぐ外）に当たる中身の光の強さ
const float kGummyRootWidth   = 0.18;  // 根本の光の幅（ベゼル幅に対する割合）
const float kGummyIconBleed   = 0.9;   // ランチャーのアイコンの色のにじみ（動画・画像は kGummyBleed）
const float kGummyUnderTint  = 1.0;   // 掴んでいる間、下にあるものの色をゲルへ混ぜる強さ
const float kGummyElev        = 0.83;  // 光の高さ（ラジアン。小さいほど横から当たる）
const float kGummyWrap        = 0.51;  // 光の回り込み
const float kGummySpec        = 0.7;  // つやの強さ
const float kGummySpecPow     = 7.0;  // つやの鋭さ（小さいほど広く鈍い）
const float kGummyRim         = 0.46;  // 外周の光の線を残す量（標準=1）
const float kGummyRimSoft     = 2.2;  // 外周の光の線の太さ（標準=1）
const float kGummyGlassLayers = 0.0;  // ガラスのにじみ・内側の線を残す量（標準=1）
const float kGummySSS         = 1.33;  // 透過光（光の反対側の縁にたまる光）
const float kGummySSSWidth    = 0.5;  // 透過光の幅（ベゼル幅に対する割合）
const float kGummySSSSat      = 0.5;  // 透過光の彩度ブースト
const float kGummySideDark    = 0.56;  // 側面の沈み（標準=1）
const float kGummyGrain       = 0.16;  // 粒の強さ
const float kGummyGrainScale  = 0.01;  // 粒の大きさ（ベゼル幅に対する割合）
const float kGummySparkle     = 0.31;  // つやに乗る粒のきらめき
const vec3  kGummyTint        = vec3(0.930, 0.970, 0.980);  // ゲルの色
// 中身・背景は64×64へ縮めた小テクスチャから読む。ミップの段は 0=64×64、1段ごとに半分、6=1×1
const float kGummyBleedLevel   = 1.5;  // にじみを読む段（根本側。小さいほど細部が残る）
const float kGummyBleedSpread  = 1.5;  // 外周へ行くほど段を上げる量（外周ほどぼける）
const float kGummyRootLevel    = 1.0;  // 根本の光を読む段
const float kGummyUnderLevel   = 2.5;  // 掴んでいる間、下にあるものの色を読む段
const float kGummyUnderGainMax = 2.0;  // 下にあるものの色をゲルの明るさへ持ち上げる倍率の上限
// @@GUMMY_PARAMS_END

float gummyHash(vec2 p)
{
    p = fract(p * vec2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return fract((p.x + p.y) * p.x);
}

// 粒（値ノイズ）。セルごとのハッシュをなめらかに補間する。
float gummyGrainNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = gummyHash(i);
    float b = gummyHash(i + vec2(1.0, 0.0));
    float c = gummyHash(i + vec2(0.0, 1.0));
    float e = gummyHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, e, f.x), f.y);
}

// グミの断面。外周からふっくら盛り上がるドームと、コンテンツの縁へ落ちる
// くぼみを掛け合わせる。
// 戻り値 x: 高さ（0=外周〜1=頂上） / y: 傾き（sin。正=外向きに下がる、負=内向き）
// y は標準の bend と同じ尺度なので、法線表示・屈折にそのまま使える。
vec2 gummyProfile(float d, float dContent, float bez, float sideMask, float innerMix)
{
    float height = kGummyPuff * bez;
    float wO = max(bez * kGummyDome, 1.0);
    float u = clamp(-d / wO, 0.0, 1.0);
    float hO = 1.0 - pow(1.0 - u, kGummyDomePow);
    float sO = kGummyDomePow * pow(1.0 - u, kGummyDomePow - 1.0) * height / wO;

    float wI = max(bez * kGummyWell, 1.0);
    float v = clamp(dContent / wI, 0.0, 1.0);
    float wellMask = sideMask * innerMix * step(0.001, kGummyWell);
    float hI = 1.0 - kGummyWellDepth * (1.0 - v) * (1.0 - v);
    float sI = 2.0 * kGummyWellDepth * (1.0 - v) * height / wI;

    float h = hO * mix(1.0, hI, wellMask);
    float s = sO - sI * wellMask;
    return vec2(h, s / sqrt(1.0 + s * s));
}
#endif

void glassMain() {
    // 角丸矩形のSDF（d<0: 内側、d=0: 縁、d>0: 外側）
    vec2 rel = vPos - uCenter;
    vec2 q = abs(rel) - uHalfSize + vec2(uRadius);
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uRadius;

    // ---- 2026-09-22: バブル融合 --------------------------------------------
    // 相手の板との距離を smin で合成し、液体のように首でつながった1つの
    // ガラス板として扱う（どちらか片方の draw() が全体を1回で描く）。
    // fuseH は自分側の重み（1=自分だけ、0=相手だけ）。法線・ライト・
    // コンテンツ境界の補間に使う。
    float fuseH = 1.0;
    vec2  relOther = vec2(0.0);
    if (uFuseK > 0.0) {
        relOther = vPos - uFuseCenter;
        vec2 qo = abs(relOther) - uFuseHalfSize + vec2(uFuseRadius);
        float dOther = length(max(qo, 0.0)) + min(max(qo.x, qo.y), 0.0) - uFuseRadius;
        fuseH = clamp(0.5 + 0.5 * (dOther - d) / uFuseK, 0.0, 1.0);
        d = mix(dOther, d, fuseH) - uFuseK * fuseH * (1.0 - fuseH);
    }

    float bez = max(uBezelPx, 4.0);
#ifdef CARVED_GLASS
    // 1物理ピクセルぶんの論理px。分岐の中で fwidth を取らないよう先に求める。
    float carvePx = max(fwidth(d), 1e-3);
#endif
#ifdef GUMMY_GLASS
    float gumPx = max(fwidth(d), 1e-3);
#endif
    // 通常レイヤーはガラス面の内側だけだが、強いハイライトの専用
    // ブルームだけは外へ約1ベゼルぶん許可する。
    float insideMask = 1.0 - smoothstep(-1.5, 2.0, d);
    float strongBloomExtent = bez * 0.95;
    if (insideMask <= 0.0 && d >= strongBloomExtent) {
        fragColor = vec4(0.0); fragCover = vec4(0.0); return;
    }
    float cornerRatio =
        clamp(uRadius / max(min(uHalfSize.x, uHalfSize.y), 1.0),
              0.0, 1.0);

    // ---- デバッグ: 実効サーフェス法線の表示モード(Shift+D) ----
    // 2026-07-19: ノイズ法線マップ全廃に伴い、屈折に実際に使っている
    // 「レンズ状に湾曲した成形ガラス板」の解析的な3D法線を可視化する。
    // RGB=法線xyz、(0.5,0.5,1.0)の紫が平坦(ガラス中央)、縁ほど傾く。
    if (uDebugNormal > 0.5 && uDebugNormal < 1.5) {
        vec2 hsD = max(uHalfSize, vec2(1.0));
        vec2 kkD = vec2(log(0.5)) / log(clamp((hsD - vec2(uRadius)) / hsD, vec2(0.05), vec2(0.99)));
        vec2 gD = sign(rel) * pow(clamp(abs(rel) / hsD, 0.0, 1.0), kkD);
        float gLD = length(gD);
        vec2 nrmD = gLD > 1e-4 ? gD / gLD : vec2(0.0, -1.0);
        float normalScaleD = max(uNormalScalePx, 4.0);
        // 屈折と同じ、ソフトマックス化した滑らかな距離+クッション形プロファイル
        // (縁で最大→中央でなめらかに0。対角の切れ目が出ない)
        // 屈折側と同一: 分岐なしソフトマックス場+指数減衰プロファイル(全域C∞)
        float smWD = normalScaleD * 0.5;
        float dSoftD = 0.5 * ((q.x + q.y) + sqrt((q.x - q.y) * (q.x - q.y) + smWD * smWD)) - uRadius;
        float rr = -dSoftD / normalScaleD;
        // ランチャーは鋭い外周成分に弱く広い裾を重ね、法線をぼかした
        // ように影響範囲を広げる。追加テクスチャサンプルは使わない。
        float rrPositiveD = max(rr, 0.0);
        float mediaBendD = exp(-2.6 * rrPositiveD);
        float launcherBendD =
            launcherBendProfile(rrPositiveD, cornerRatio);
        float bendD =
            mix(mediaBendD, launcherBendD, uSurfaceProfile);
#ifdef CARVED_GLASS
        // 削り出し: 屈折と同じ面取りの断面で法線を表示する（下の屈折処理と同じ式）。
        {
            vec2 contentHalfD = max(uHalfSize - vec2(bez), vec2(2.0));
            float contentRadiusD = min(uContentRadius, min(contentHalfD.x, contentHalfD.y));
            vec2 qContentD = abs(rel) - contentHalfD + vec2(contentRadiusD);
            float dContentD = length(max(qContentD, 0.0))
                            + min(max(qContentD.x, qContentD.y), 0.0) - contentRadiusD;
            float sideMaskD = smoothstep(-carvePx, 0.0, dContentD);
            float bOD, bID, wOD, wID;
            carveBands(d, dContentD, bez, sideMaskD, carvePx, bOD, bID, wOD, wID);
            bendD = carveBend(d, bez, carvePx, bOD, bID, sideMaskD, uInnerMix);
        }
#endif
#ifdef GUMMY_GLASS
        // グミ: 屈折と同じ断面で法線を表示する
        {
            vec2 contentHalfD = max(uHalfSize - vec2(bez), vec2(2.0));
            float contentRadiusD = min(uContentRadius, min(contentHalfD.x, contentHalfD.y));
            vec2 qContentD = abs(rel) - contentHalfD + vec2(contentRadiusD);
            float dContentD = length(max(qContentD, 0.0))
                            + min(max(qContentD.x, qContentD.y), 0.0) - contentRadiusD;
            float sideMaskD = smoothstep(-gumPx, 0.0, dContentD);
            bendD = gummyProfile(d, dContentD, bez, sideMaskD, uInnerMix).y;
        }
#endif
        float effectiveBendD =
            clamp(bendD * uNormalInfluence, -0.999, 0.999);
        vec3 n3 = vec3(
            nrmD * effectiveBendD,
            sqrt(max(0.0, 1.0 - effectiveBendD * effectiveBendD)));
        // 法線マップの慣例(yは上向き正)に合わせscreen座標のyを反転して表示
        vec3 colD = vec3(n3.x, -n3.y, n3.z) * 0.5 + 0.5;
        fragColor = vec4(colD * insideMask, insideMask);
        fragCover = vec4(insideMask);
        return;
    }

    // ---- 縁の外向き法線（超楕円近似・全周なめらか版） ----
    // 2026-07-19: 当初はSDF勾配そのもの（辺領域=軸方向/角領域=放射方向の
    // if分岐）を使っていたが、分岐境界（角から内側へ伸びる対角線＝SDFの
    // 中軸線）で法線が不連続になり、ベゼルが角丸半径より厚い本アプリでは
    // 「額縁の留め継ぎ」のような筋が角に見えてしまった（実機で確認）。
    // そこで超楕円 (|x/w|^k + |y/h|^k = 1) の法線で近似する: 辺の中央では
    // 軸方向、角丸ゾーンに入るとなめらかに対角へ回転し、不連続が一切ない。
    // 指数kは「角丸の始まる位置で寄与が0.5になる」よう軸ごとに自動算出。
    vec2 hs = max(uHalfSize, vec2(1.0));
    vec2 kk = vec2(log(0.5)) / log(clamp((hs - vec2(uRadius)) / hs, vec2(0.05), vec2(0.99)));
    vec2 g = sign(rel) * pow(clamp(abs(rel) / hs, 0.0, 1.0), kk);
    float gLen = length(g);
    vec2 nrm = gLen > 1e-4 ? g / gLen : vec2(0.0, -1.0);

    vec2 L = vec2(cos(uLightAngle), sin(uLightAngle));
    // 2026-09-22: 融合中は相手の形の法線・ライトと重み fuseH で混ぜる。
    // 首の部分では2つの角の法線が混ざって、首の向きに沿った法線になる。
    if (uFuseK > 0.0 && fuseH < 0.999) {
        vec2 hsO = max(uFuseHalfSize, vec2(1.0));
        vec2 kkO = vec2(log(0.5)) / log(clamp((hsO - vec2(uFuseRadius)) / hsO,
                                              vec2(0.05), vec2(0.99)));
        vec2 gO = sign(relOther) * pow(clamp(abs(relOther) / hsO, 0.0, 1.0), kkO);
        float gLO = length(gO);
        vec2 nrmO = gLO > 1e-4 ? gO / gLO : nrm;
        vec2 nm = mix(nrmO, nrm, fuseH);
        if (length(nm) > 1e-4)
            nrm = normalize(nm);
        vec2 LO = vec2(cos(uFuseLightAngle), sin(uFuseLightAngle));
        vec2 lm = mix(LO, L, fuseH);
        if (length(lm) > 1e-4)
            L = normalize(lm);
    }
    float facing = dot(nrm, L); // 1: 光源を向く縁 / 0: 側面 / -1: 真裏
    float litSharp = pow(clamp(facing, 0.0, 1.0), 20.0);
    float strongBloomSigma = max(bez * 0.30, 2.5);
    float strongBloomDistance = max(d, 0.0);
    float strongOuterBloom =
        exp(-(strongBloomDistance * strongBloomDistance)
            / (2.0 * strongBloomSigma * strongBloomSigma))
        * smoothstep(-1.0, 1.5, d)
        * litSharp * 0.30;

    // 外側は専用ブルームだけを出し、コンテンツ反射や屈折の重い処理へ
    // 進ませない。デバッグ表示と屈折プリパスでは従来どおり外へ出さない。
    if (insideMask <= 0.0) {
        if (uRefractionPrepass > 0.5 || uDebugNormal > 0.5) {
            fragColor = vec4(0.0); fragCover = vec4(0.0); return;
        }
        float bloomK = uIntensity * strongOuterBloom * uTint.a;
        fragColor = vec4(vec3(bloomK), bloomK);
        fragCover = vec4(bloomK);
        return;
    }

    // ---- 深さ方向の層（すべて縁からの距離dのガウス） ----
    // 2026-07-19: 当初は太さ・位置が固定pxだったため、カメラを引く（ズーム
    // アウトする）とバブルに対して相対的に太く見え、ズーム具合でハイライトの
    // 入り方が変わってしまった（ユーザー指摘）。すべてベゼル厚uBezelPx
    // 基準の相対値に変更し、リサイズやズームに依存しない比率にする。
    // 係数は変更前の標準的な見た目
    // （bez≈26px時: リムσ2.4/にじみσ6.0/内側線σ1.8）と一致するよう逆算。
    // 極小バブルでサブピクセル化して消えないよう最小pxでクランプ。
    // コンテンツ境界のSDF。内周ハイライト・鏡像・片側マスクで共用し、
    // 各レイヤーの根元が同じ位置から始まるようにする。
    vec2 contentHalf = max(uHalfSize - vec2(bez), vec2(2.0));
    float contentRadius =
        min(uContentRadius, min(contentHalf.x, contentHalf.y));
    vec2 qContent = abs(rel) - contentHalf + vec2(contentRadius);
    float dContent = length(max(qContent, 0.0))
                   + min(max(qContent.x, qContent.y), 0.0)
                   - contentRadius;
    // 2026-09-22: 融合中は相手のコンテンツ境界も内側の線の対象にする
    // （2枚の動画はそれぞれの角丸のまま。首の部分はガラスだけになる）。
    if (uFuseK > 0.0) {
        vec2 contentHalfO = max(uFuseHalfSize - vec2(max(uFuseBezelPx, 4.0)), vec2(2.0));
        float crO = min(uFuseContentRadius, min(contentHalfO.x, contentHalfO.y));
        vec2 qcO = abs(relOther) - contentHalfO + vec2(crO);
        float dcO = length(max(qcO, 0.0)) + min(max(qcO.x, qcO.y), 0.0) - crO;
        dContent = min(dContent, dcO);
    }
    float contentEdgeAa = max(fwidth(dContent), 0.75);
    float bezelSideMask =
        smoothstep(-contentEdgeAa, 0.0, dContent);

    // (a) 外周リム: 縁ぴったりの鋭く強い光の線＋周囲の柔らかいにじみ
    // 2026-09-11: 光量(ピーク値)はガウスの性質上sigmaに依存せず常に1.0で
    // 変わらないため、線を細くしたいという要望に対してsigma比率だけを
    // 0.092→0.06へ下げた(光の強さはそのまま、太さだけ約2/3に)。
    float rimSigma = max(bez * 0.06, 1.0);
    float rimT = d + rimSigma * 0.75;
    float rimLine = exp(-(rimT * rimT) / (2.0 * rimSigma * rimSigma));
    // ---- 疑似色収差（chromatic aberration） ----
    // 本物の分散（波長ごとの屈折率差）は背景屈折なしでは再現できないため、
    // リムのガウスをチャンネルごとに深さ方向へ±caだけずらしてサンプルする。
    // 青は外側へ・赤は内側へ寄るので、エッジの外側が青紫、内側がオレンジに
    // うっすら色づく。caMixで強さを調整（0=無効、1=フルずらし）。
    float ca = clamp(rimSigma * 0.5, 0.5, 2.0);
    const float caMix = 0.65;
    float tR = rimT + ca;
    float tB = rimT - ca;
    float rimR = exp(-(tR * tR) / (2.0 * rimSigma * rimSigma));
    float rimB = exp(-(tB * tB) / (2.0 * rimSigma * rimSigma));
    vec3 rimRGB = mix(vec3(rimLine), vec3(rimR, rimLine, rimB), caMix);
    float bloomSigma = max(bez * 0.18, 2.5);
    float bloomT = d + bloomSigma * 0.5;
    float rimBloom = exp(-(bloomT * bloomT) / (2.0 * bloomSigma * bloomSigma));
    // (b) ベゼル内部を満たす厚みのあるグロー（ガラスの「厚み」の表現）
    float glowT = d + bez * 0.55;
    float glowSigma = bez * 0.45;
    float innerGlow = exp(-(glowT * glowT) / (2.0 * glowSigma * glowSigma));
    // (c) 内側境界: コンテンツとの実際の境目に走る細い線。
    // コンテンツ側へ漏らさず、ガラス側だけへ広げる。
    float lineSigma = max(bez * 0.07, 1.0);
    float innerLine =
        exp(-(dContent * dContent) / (2.0 * lineSigma * lineSigma))
        * bezelSideMask;

    // ---- 法線ベースの照明項 ----
    float litBroad = smoothstep(-0.05, 1.0, facing);      // 弱い回り込みも控えめに
    float counter  = pow(clamp(-facing, 0.0, 1.0), 2.5);  // 反対側の弱い反射

#ifdef CARVED_GLASS
    // ---- 削り出し: 強い光をカクっとさせる（2026-09-16）--------------------
    // 標準の強い光は「縁からガウスでにじむ線」と「光源の向きへ pow で
    // なめらかに絞る」の組み合わせで、なめらかな形状向き。削り出しでは、
    //   ・深さ方向 … 光を外周の面取りの面いっぱいに平らに乗せ、折れ目で
    //                ほぼ段差で切る（色収差も折れ目の位置をずらして出す）
    //   ・周方向   … 光源の向きに対して、ある角度を境にパッと点灯する
    // にして、面ごとに光を拾っている見え方にする。
    // 調整はこの定数から:
    //   kCarveRimSoft          … 光の帯の折れ目のなじみ（面取りの幅に対する割合）
    //   kCarveLitStart / End   … 周方向の点灯の切り替え（facing の範囲。1に近いほど光源の正面だけ）
    //   kCarveRimDepth         … 光の帯の深さ（外周の面取りの幅に対する割合）
    //
    // 2026-09-16 調整: 前面（ベゼルの平らな面のにじみ・厚みのグロー・内側の線）は
    // 標準と同じに戻し、カクっとさせるのは外周の面取りの強い光だけにした。
    // 点灯する角度の範囲も狭め（0.55〜0.72 → 0.82〜0.95）、光源の正面の辺に限定。
    float litSharpRim = litSharp;
    {
        const float kCarveRimSoft  = 0.10;
        // さらに調整: 光る範囲が広いと眩しいので、角度を 0.82〜0.95 → 0.90〜0.985 に、
        // 光の帯の深さを面取りの幅の 60% に絞った（面取りの外寄りだけが光る）。
        const float kCarveLitStart = 0.90;
        const float kCarveLitEnd   = 0.985;
        const float kCarveRimDepth = 0.60;

        float wRimO = max(bez * kCarveOuterRatio * kCarveRimDepth, carvePx);
        float softR = max(wRimO * kCarveRimSoft, carvePx);
        float caR = clamp(wRimO * 0.12, 0.0, 2.0);
        float tRim = -d;
        rimLine = 1.0 - smoothstep(wRimO - 0.5 * softR, wRimO + 0.5 * softR, tRim);
        rimR = 1.0 - smoothstep(wRimO + caR - 0.5 * softR, wRimO + caR + 0.5 * softR, tRim);
        rimB = 1.0 - smoothstep(wRimO - caR - 0.5 * softR, wRimO - caR + 0.5 * softR, tRim);
        rimRGB = mix(vec3(rimLine), vec3(rimR, rimLine, rimB), caMix);

        litSharpRim = smoothstep(kCarveLitStart, kCarveLitEnd, facing);
    }
#elif defined(GUMMY_GLASS)
    // グミ: 外周の鋭い光の線を太く柔らかくし、強さを kGummyRim まで落とす。
    // 点灯する角度の絞りも緩め（pow 20→6）、辺全体がぼんやり光るようにする。
    float litSharpRim = pow(clamp(facing, 0.0, 1.0), 6.0);
    {
        float rsG = rimSigma * kGummyRimSoft;
        float rtG = d + rsG * 0.75;
        float caG = clamp(rsG * 0.5, 0.5, 3.0);
        rimLine = exp(-(rtG * rtG) / (2.0 * rsG * rsG));
        rimR = exp(-((rtG + caG) * (rtG + caG)) / (2.0 * rsG * rsG));
        rimB = exp(-((rtG - caG) * (rtG - caG)) / (2.0 * rsG * rsG));
        rimRGB = mix(vec3(rimLine), vec3(rimR, rimLine, rimB), caMix) * kGummyRim;
    }
#else
    float litSharpRim = litSharp;
#endif

    // リムには小さな定数項を足し、側面でもガラスの切り口がうっすら光るようにする。
    // リムのみ色収差でRGB別、他レイヤーは白のままスカラー加算。
    float rimW  = 1.18 * litSharpRim + 0.20 * litBroad + 0.62 * counter + 0.05;
    float restW = rimBloom  * (0.48 * litSharp + 0.20 * counter)
              + (innerGlow * (0.38 * litSharp + 0.08 * counter)
               + innerLine * (0.48 * litSharp + 0.24 * counter)) * uInnerMix;
#ifdef GUMMY_GLASS
    // ガラスらしいにじみ・厚みのグロー・内側の線は控えめにする
    restW *= kGummyGlassLayers;
#endif
    vec3 lum = rimRGB * rimW + vec3(restW);

    // ---- ガラススタイル「削り出し」（2026-09-16）------------------------
    // CARVED_GLASS を定義してコンパイルしたシェーダーだけに入る（標準の
    // シェーダーには1命令も増えない。GlassHighlightEffect::initShader 参照）。
    //
    // 線を足すのではなく「面取りの面」を作る。板の外周のすぐ内側と、
    // コンテンツの縁のすぐ外側に、傾いた細い帯（面取り）があるとみなし、
    // 帯の向きと光の向きで明暗を決める。平らなベゼルとの境目はぼかさず
    // くっきり切り替えるので、「削り出した角」の段差に見える。
    //   外周の面取り … 外へ向かって下がる面。光源側が明るく、反対側が暗い。
    //   内側の面取り … コンテンツへ向かって落ちる面。向きが逆なので明暗も逆。
    // 明暗の向きは既存の縁の光と同じ facing（縁の法線×光の向き）を使うので、
    // カメラが動けば一緒に回る。
    //
    // 調整はこの定数から:
    //   kCarveOuterRatio / kCarveInnerRatio … 外周・内側の面取りの幅（ベゼル幅に対する割合）
    //   kCarveSoft / kCarveEdgeRatio         … 折れ目の丸み・稜線の太さ（面取りの幅に対する割合）
    //   kCarveHi / kCarveShade      … 面取りの明るい側・暗い側の強さ
    //   kCarveEdge                  … 面取りと平らな面の境目に乗る細い光
    float carveDark = 0.0;
    // 面取りの帯の被覆率（反射・透過・屈折でも使う。標準では0のまま）
    float carveBandO = 0.0;
    float carveBandI = 0.0;
#ifdef CARVED_GLASS
    {
        // 2026-09-16 改訂:
        //  - 幅・境目のなじみ・稜線の太さを、すべてベゼル幅に比例させた。
        //    以前は px の下限/上限と「1物理ピクセルの境目」を使っていたため、
        //    カメラで寄ると面取りだけが相対的に細く・硬くなっていた。
        //    ベゼル幅はズームに比例するので、これで寄っても引いても同じ形に見える。
        //  - 境目を完全な段差にせず、角だけわずかに丸める（kCarveSoft）。
        //    全体がぼやけるのではなく、折れ目の前後だけがなじむ。
        //  （幅・丸みの定数と帯の計算は carveBands() に移した）
        const float kCarveEdgeRatio  = 0.10;  // 稜線の光の太さ（面取りの幅に対する割合）
        const float kCarveHi         = 0.34;
        // 削り出しは面がはっきり折れているので、なめらかな標準より影を強めにする。
        const float kCarveShade      = 0.46;
        const float kCarveEdge       = 0.26;

        float pxO = carvePx;
        float pxI = carvePx;
        float bandO, bandI, wO, wI;
        carveBands(d, dContent, bez, bezelSideMask, carvePx, bandO, bandI, wO, wI);
        float tO = -d;                                        // 外周から内側への距離

        // 面の向きと光。外周の面取りは外向き（facing）、内側は内向き（-facing）。
        float sO = facing;
        float sI = -facing;
        // 面ごとに光を拾う見え方にするため、明暗の切り替えも角度で急にする
        float hiO = smoothstep(0.35, 0.55, sO);
        float shO = smoothstep(0.35, 0.55, -sO);
        float hiI = smoothstep(0.35, 0.55, sI);
        float shI = smoothstep(0.35, 0.55, -sI);

        // 折れ目に乗る光（太さも面取りの幅に比例）
        float edgeHalfO = max(wO * kCarveEdgeRatio, 0.5 * pxO);
        float edgeHalfI = max(wI * kCarveEdgeRatio, 0.5 * pxI);
        float edgeO = 1.0 - smoothstep(edgeHalfO * 0.5, edgeHalfO + pxO, abs(tO - wO));
        float edgeI = 1.0 - smoothstep(edgeHalfI * 0.5, edgeHalfI + pxI, abs(dContent - wI));

        lum += vec3(bandO * hiO * kCarveHi
                  + bandI * hiI * kCarveHi * 0.8 * uInnerMix
                  + edgeO * kCarveEdge * (0.35 + 0.65 * litBroad)
                  + edgeI * kCarveEdge * 0.8 * (0.35 + 0.65 * max(counter, litBroad * 0.5))
                    * uInnerMix);
        carveDark = bandO * shO * kCarveShade
                  + bandI * shI * kCarveShade * 0.8 * uInnerMix;
        carveBandO = bandO * insideMask;
        carveBandI = bandI;
    }
#endif
#ifdef GUMMY_GLASS
    // ---- ガラススタイル「グミ」（2026-10-11）--------------------------------
    // 厚みのある半透明のゲルとして描く。ガラスの「縁の線」で見せるのではなく、
    //   ・断面 … 外周からふっくら盛り上がるドーム＋コンテンツの縁へ落ちるくぼみ
    //   ・体   … 乳白で半透明。厚いところほど濃く、縁は少し透ける。粒でざらつく
    //   ・光   … 3Dの法線で広く柔らかい拡散と鈍いつや。光の反対側の縁に、
    //            ゲルの中を通ってきた色付きの光（透過光）がたまる
    // 体の色は kGummyTint。Quality ではコンテンツの色をにじませる。
    // 体（gumBody）は合成のときハイライトの下に敷く。
    vec4 gumBody = vec4(0.0);
    {
        vec2 prof = gummyProfile(d, dContent, bez, bezelSideMask, uInnerMix);
        float gH = prof.x;
        float gBend = prof.y;
        vec3 n3 = vec3(nrm * gBend, sqrt(max(0.0, 1.0 - gBend * gBend)));
        vec3 L3 = normalize(vec3(L * cos(kGummyElev), sin(kGummyElev)));
        float dif = clamp((dot(n3, L3) + kGummyWrap) / (1.0 + kGummyWrap), 0.0, 1.0);
        vec3 H3 = normalize(L3 + vec3(0.0, 0.0, 1.0));
        float spec = pow(max(dot(n3, H3), 0.0), kGummySpecPow);

        // 粒はバブルの局所座標・ベゼル比で作る（動かしてもズームしても同じ見え方）
        float grainSize = max(bez * kGummyGrainScale, gumPx);
        float grain = gummyGrainNoise(rel / grainSize) * 2.0 - 1.0;

        // ゲルは動画・画像ではコンテンツの外（ベゼル側）だけ。
        // ランチャー（innerMix=0）はくぼみを作らず、板全体を1つのゲルで覆う。
        // ランチャーはこのゲルをアイコンより先に描く（CanvasWidget 側）ので、
        // アイコンはゲルの上に乗る（draw() でグミのランチャーはリングの中抜きもやめている）。
        float gel = mix(1.0, bezelSideMask, uInnerMix);
        vec3 bodyCol = kGummyTint;
        vec3 rootCol = vec3(0.0);   // 根本（中身の縁）に当たる中身の光の色
        // グミ: 中身を64×64へ縮めてミップを作った小テクスチャ
        // （GlassHighlightEffect::updateGummySmall、10Hz）を読むだけ。縮めた時点で
        // 十分ぼけているので筋も出ない。にじみ・根本の光・平均色の3回だけで済む
        // （元のグミは毎画素22回）。外周ほどぼかすのはミップの段を上げて表現する。
        if (uContentReflection > 0.001 && uGummySmallValid > 0.5 && gel > 0.001) {
            float tG = clamp(max(dContent, 0.0) / bez, 0.0, 1.0);
            float depthIn = max(dContent, 0.0) + bez * kGummyBleedDepth;
            vec2 c01 = clamp((rel - nrm * depthIn) / (2.0 * contentHalf) + vec2(0.5), 0.0, 1.0);
            vec4 sb = textureLod(uGummySmall, c01, kGummyBleedLevel + kGummyBleedSpread * tG);
            vec4 sr = textureLod(uGummySmall, c01, kGummyRootLevel);
            vec4 avgG = textureLod(uGummySmall, vec2(0.5), 6.0);   // 1×1 の段＝全体の平均
            vec4 sc = mix(sb, avgG, kGummyBleedFar * smoothstep(0.0, 1.0, tG));
            // 2026-10-11: ランチャー（innerMix=0）は場所ごとの色を拾うとアイコンの
            // ぼけた複製がゲルに浮いて見えたため、アイコン全体の平均色1色で染める。
            sc = mix(avgG, sc, uInnerMix);
            vec3 scCol = sc.rgb / max(sc.a, 1e-3);
            // 2026-10-11: にじむ色は少し鮮やかにする（kGummyBleedSat。1=元の彩度）
            float scL = dot(scCol, vec3(0.2126, 0.7152, 0.0722));
            scCol = clamp(mix(vec3(scL), scCol, kGummyBleedSat), 0.0, 1.0);
            vec3 bleed = mix(scCol, vec3(1.0), 0.18);
            float bleedK = mix(kGummyIconBleed, kGummyBleed, uInnerMix);
            bodyCol = mix(bodyCol, bleed,
                          bleedK * uContentReflection * smoothstep(0.0, 0.35, sc.a));
            rootCol = (sr.rgb / max(sr.a, 1e-3)) * smoothstep(0.0, 0.35, sr.a) * uContentReflection;
            float rootL = dot(rootCol, vec3(0.2126, 0.7152, 0.0722));
            rootCol = clamp(mix(vec3(rootL), rootCol, kGummyBleedSat), 0.0, 1.0);
        }

        // 掴んでいる間（Qualityで背景キャプチャがあるとき）は、下にあるもの
        // （他のバブル・キャンバス）の色をゲルへ混ぜる。屈折像は見せず、大きく
        // ぼかした色味だけを移す。明るさはゲルのままにし、灰色（キャンバス等）は混ぜない。
        if (uCaptureTint > 0.001 && gel > 0.001) {
            // グミ: 背景キャプチャも64×64へ縮めてあるので1回読むだけ
            vec2 clU = clamp((gl_FragCoord.xy - uCaptureOrigin)
                             / max(uCaptureSourceSize, vec2(1.0)), 0.0, 1.0);
            vec4 su = textureLod(uGummyCaptureSmall, clU, kGummyUnderLevel);
            vec3 uc = su.rgb / max(su.a, 1e-3);
            float ucMax = max(uc.r, max(uc.g, uc.b));
            float ucMin = min(uc.r, min(uc.g, uc.b));
            float ucSat = (ucMax - ucMin) / max(ucMax, 1e-3);
            vec3 lumW = vec3(0.2126, 0.7152, 0.0722);
            // 2026-10-11: 以前は明るさを無制限にゲルへ合わせていたため、再生中の動画の
            // 暗い部分が何倍にも持ち上がって原色に化け、毎フレームちらついた。
            // 倍率に上限をつけ、暗い色ほど混ぜる量も減らす（縮小側でも時間方向になめらかにしている）。
            float ucL = dot(uc, lumW);
            float gainU = min(dot(bodyCol, lumW) / max(ucL, 0.05), kGummyUnderGainMax);
            vec3 ucBright = clamp(uc * gainU, 0.0, 1.0);
            bodyCol = mix(bodyCol, ucBright,
                          kGummyUnderTint * uCaptureTint * su.a
                          * smoothstep(0.02, 0.25, ucSat) * smoothstep(0.03, 0.2, ucL));
        }

        float milkA = kGummyMilk * mix(kGummyEdgeClear, 1.0, gH);
        milkA = clamp(milkA * (1.0 + kGummyGrain * grain), 0.0, 1.0) * gel;
        vec3 milkRgb = bodyCol * mix(mix(kGummyAmbient, 1.0, uAmbientLift), 1.0, dif);   // 映像用: 陰の面を持ち上げる（本家=0）
        gumBody = vec4(milkRgb * milkA, milkA);

        // つや: 粒でまだらにして、砂糖をまぶしたような鈍い光り方にする
        float sheen = max(spec * kGummySpec * (1.0 + kGummySparkle * grain), 0.0) * gel;
        // 透過光: 光の反対側の外周付近へ、ゲルを通ってきた色付きの光がたまる
        float back = smoothstep(0.0, 1.0, -facing);
        float sssBand = exp(-max(-d, 0.0) / max(bez * kGummySSSWidth, 1.0));
        float lumaB = dot(bodyCol, vec3(0.2126, 0.7152, 0.0722));
        vec3 sssCol = clamp(mix(vec3(lumaB), bodyCol, 1.0 + kGummySSSSat), 0.0, 1.0);
        lum += vec3(sheen) + sssCol * (kGummySSS * back * sssBand * gel);
        // 根本の光: 中身の縁のすぐ外側に、中身の光が強く当たっている帯。
        // 鏡像ではなく、上でぼかした中身の色をそのまま光として足す。
        float rootBand = exp(-max(dContent, 0.0) / max(bez * kGummyRootWidth, 1.0))
                       * bezelSideMask * uInnerMix;
        lum += rootCol * (kGummyRootGlow * rootBand);
    }
#endif
    vec3 contentReflectionRgb = vec3(0.0);
    float contentReflectionCoverage = 0.0;

    // ---- コンテンツ由来の色反射（Qualityのみ） ----
    // ガラス内周を鏡面として、ベゼル上の点を内側境界の法線方向へ折り返す。
    // 上下辺は縦反転、左右辺は横反転し、角では上で求めた連続法線nrmに
    // 沿って放射状に回り込む。4辺を別々に貼る方式ではないため継ぎ目がない。
    if (uContentReflection > 0.001) {
        // 内周の急な曲面を通る反射として、根元では法線方向の像を強く圧縮し、
        // 外側へ離れるほど通常の平面鏡（等倍の折り返し）へ戻す。
        float reflectionDepth = max(dContent, 0.0);
        // 内周反射を外側から単純に切り落とすのではなく、境界からの
        // プロファイル座標そのものを少し圧縮する。反射像の進み方、濃度、
        // 色拡散がまとめて狭い帯へ収まり、元のグラデーションを維持する。
        float reflectionProfileDepth = reflectionDepth * 1.15;
        float rootDistortion =
            exp(-reflectionProfileDepth / max(bez * 0.18, 1.0));
        float reflectedDepth =
            reflectionProfileDepth * mix(1.0, 4.6, rootDistortion);
        // p=b+d*n から境界を越え、コンテンツ側のreflectedDepthへ写す。
        // AA画素を避けるオフセットはベゼル比率ではなく1〜2pxに制限し、
        // 大きいバブルでも鏡像の根元が境界から離れないようにする。
        float boundaryInset =
            clamp(contentEdgeAa * 1.25, 0.75, 2.0);
        vec2 reflectedRel =
            rel - nrm
                * (reflectionDepth + reflectedDepth + boundaryInset);
        vec2 content01 =
            clamp(reflectedRel / (2.0 * contentHalf) + vec2(0.5), 0.0, 1.0);
        vec2 contentUv =
            uContentUvRect.xy + content01 * uContentUvRect.zw;
        // 外周の透過光は鏡像ではない。ガラス上の点を同じ側の
        // コンテンツ境界へ戻し、少し内側を読む非反転UVを使う。
        vec2 transmittedRel =
            rel - nrm * (reflectionDepth + boundaryInset);
        vec2 transmitted01 =
            clamp(transmittedRel / (2.0 * contentHalf) + vec2(0.5), 0.0, 1.0);
        vec2 transmittedUv =
            uContentUvRect.xy + transmitted01 * uContentUvRect.zw;

        // コンテンツ色を広いグローへ混ぜない。鏡像は内周境界、透過色は
        // 外周境界を中心とした細い帯だけに限定する。
        float innerReflectionSigma = max(bez * 0.130, 1.0);
        float outerTransmissionSigma = max(bez * 0.120, 1.5);
        // 反射帯は内周境界のガラス側だけに存在させる。以前はガウスが
        // 境界の両側へ広がり、コンテンツ本体へ反射サンプルが漏れていた。
        float innerReflectionMask =
            exp(-(reflectionProfileDepth * reflectionProfileDepth)
                / (2.0 * innerReflectionSigma * innerReflectionSigma))
            * bezelSideMask;
        // コンテンツに接する根元だけは、近距離から強い光を受ける細い
        // コア反射を重ねる。広い反射領域を白飛びさせず、境界で最大になる。
        float innerReflectionCoreSigma = max(bez * 0.035, 0.75);
        float innerReflectionCore =
            exp(-(reflectionProfileDepth * reflectionProfileDepth)
                / (2.0 * innerReflectionCoreSigma
                   * innerReflectionCoreSigma))
            * bezelSideMask;
        float outerTransmissionMask =
            exp(-(d * d)
                / (2.0 * outerTransmissionSigma * outerTransmissionSigma));
#ifdef CARVED_GLASS
        // 2026-09-16 削り出し: 反射と透過をぼかさず、面取りの帯の中だけに
        // 閉じ込める。鏡像は内側の面取り、ガラス越しの色は外周の面取りに乗り、
        // 平らな面との境目でくっきり切れる（標準は境界からガウスでぼわっと減衰）。
        innerReflectionMask = carveBandI;
        innerReflectionCore = carveBandI * 0.35;
        outerTransmissionMask = carveBandO;
#endif
#ifdef GUMMY_GLASS
        // グミ: 鏡面的な映り込みは持たない。中身の色はぼかしたにじみ（上の体の色）だけで出す
        innerReflectionMask = 0.0;
        innerReflectionCore = 0.0;
        outerTransmissionMask = 0.0;
#endif
        float directW =
            innerReflectionMask * 0.45 + innerReflectionCore * 0.525;
        float throughW = outerTransmissionMask * 0.40;
        // 2026-09-22: 融合中、コンテンツ色の反射は自分の動画の側だけに出す
        // （相手の動画の縁に、自分の動画の色が映り込まないように）。
        if (uFuseK > 0.0) {
            float selfSide = smoothstep(0.35, 0.65, fuseH);
            directW *= selfSide;
            throughW *= selfSide;
        }

        // 帯から離れた大部分の画素ではテクスチャを読まない。内周は1回、
        // 外周の透過色が見える帯だけ追加の4回サンプルを行う。
        if (directW + throughW > 0.001) {
            vec2 uvMin = uContentUvRect.xy;
            vec2 uvMax = uContentUvRect.xy + uContentUvRect.zw;
            vec4 directSample = vec4(0.0);
            vec3 reflectedRgb = vec3(0.0);
            if (directW > 0.001) {
                directSample = texture(uContent, contentUv);
                reflectedRgb = directSample.rgb;
                // コンテンツ反射にも分散を加える。根元を最大にしつつ、
                // 密集した境界部分だけに偏らないよう反射帯の外側にも
                // 45%以上を残す。標準サイズで約2.5px、巨大バブルでは
                // 8pxを上限にする。
                float dispersionFalloff =
                    exp(-reflectionProfileDepth / max(bez * 0.42, 1.0));
                float dispersionProfile =
                    mix(0.45, 1.0, dispersionFalloff);
                float dispersionPx =
                    clamp(bez * 0.10, 2.0, 8.0) * dispersionProfile;
                vec2 dispersionUv =
                    nrm * dispersionPx / (2.0 * contentHalf)
                    * uContentUvRect.zw;
                vec4 redSample = texture(
                    uContent,
                    clamp(contentUv + dispersionUv, uvMin, uvMax));
                vec4 blueSample = texture(
                    uContent,
                    clamp(contentUv - dispersionUv, uvMin, uvMax));
                reflectedRgb = vec3(
                    redSample.r, directSample.g, blueSample.b);
            }
            vec3 throughColor = vec3(0.0);

            if (throughW > 0.001) {
                // 中央＋上下左右の5点平均で像を崩し、コンテンツの色だけを残す。
                vec2 blurStep = uContentUvRect.zw / 18.0;
                vec4 throughSample = texture(uContent, transmittedUv);
                vec4 blurredSample =
                    (throughSample * 2.0
                     + texture(uContent, clamp(transmittedUv + vec2( blurStep.x, 0.0), uvMin, uvMax))
                     + texture(uContent, clamp(transmittedUv + vec2(-blurStep.x, 0.0), uvMin, uvMax))
                     + texture(uContent, clamp(transmittedUv + vec2(0.0,  blurStep.y), uvMin, uvMax))
                     + texture(uContent, clamp(transmittedUv + vec2(0.0, -blurStep.y), uvMin, uvMax))) / 6.0;
                throughColor = blurredSample.rgb * blurredSample.a;
                float throughLuma =
                    dot(throughColor, vec3(0.2126, 0.7152, 0.0722));
                // ガラスを通った光として低彩度にする。
                throughColor =
                    mix(vec3(throughLuma), throughColor, 0.62);
            }

            // 鏡像は背後の透過成分を反射率ぶん置き換える独立レイヤー。
            // ガラス表面の白いハイライトは別の光路なので減衰させず、
            // 最終合成でこの反射像より上にそのまま残す。
            float directCoverage =
                clamp(directW * uContentReflection, 0.0, 1.0)
                * directSample.a;
            contentReflectionRgb =
                reflectedRgb * directCoverage;
            lum += throughColor * throughW * uContentReflection;
            contentReflectionCoverage =
                max(contentReflectionCoverage, directCoverage);
        }
    }

    // ---- 左右側面（facing≈0）は暗く沈ませてコントラストを作る ----
    // 2026-07-19: 「暗いところをもっと暗く」の指示でコントラスト増強。
    // マスクを広げ（明・暗の縁の近くまで暗みが及ぶ）、アルファも引き上げ。
    float darkMask = 1.0 - smoothstep(0.08, 0.75, abs(facing));
    float rimDark = rimLine * 0.50 * darkMask;
    float bellyDark = innerGlow * 0.38 * uInnerMix * darkMask;
#ifdef CARVED_GLASS
    // 削り出しは側面の沈みも少し強める（標準はなめらかな形なので控えめ）。
    rimDark *= 1.35;
    bellyDark *= 1.35;
#endif
#ifdef GUMMY_GLASS
    // グミは柔らかい形なので、側面の沈みは弱くする
    rimDark *= kGummySideDark;
    bellyDark *= kGummySideDark;
#endif
    // 映像用の補正: 暗く沈む部分の濃さ（本家=1）
    rimDark *= uDarkK; bellyDark *= uDarkK; carveDark *= uDarkK;
    float aDark = rimDark + bellyDark + carveDark;

    // ---- 合成（premultiplied alpha） ----
    // 色収差でRGB別になったためaLightはベクトル。アルファには最大成分を使う
    // （平均だとフリンジ部分が背景を削りすぎて濁る）。
    // (2026-07-19: 法線マップの±8%むら(grain)はノイズ全廃に伴い削除)
    vec3 aLightRGB = clamp(lum, 0.0, 1.0);
    float aLight = max(aLightRGB.r, max(aLightRGB.g, aLightRGB.b));
    // 2026-07-19: 「単純な黒感がある」との指摘で、暗部にわずかに青みを
    // 足した（ガラスの影が環境光の青を拾っているイメージ）。
    vec3 rimDarkColor = vec3(0.04, 0.07, 0.13);
    vec3 bellyDarkColor = rimDarkColor;
    // グミ: 暗部に混ぜる画面平均も、小テクスチャの1×1段を1回読むだけ
    if (uContentReflection > 0.001 && aDark > 0.001 && uGummySmallValid > 0.5) {
        vec4 avL = textureLod(uGummySmall, vec2(0.5), 6.0);
        vec3 averageRgb = avL.rgb / max(avL.a, 1e-3);
        rimDarkColor = mix(rimDarkColor, averageRgb * 0.32, 0.72);
        bellyDarkColor = mix(bellyDarkColor, averageRgb * 0.40, 0.90);
    }
    // 反射像は透過背景より前、解析的な白ハイライトはその上に加算する。
    // 黒い反射でもcoverageは残るため背景だけが暗くなり、白ハイライトは
    // contentReflectionRgbに左右されず同じ明るさで存在し続ける。
    vec3 lightRgb = clamp(
        aLightRGB + contentReflectionRgb
        + rimDarkColor * (rimDark + carveDark) + bellyDarkColor * bellyDark,
        0.0, 1.0);
    float lightA = clamp(aLight + aDark, 0.0, 1.0);
#ifdef GUMMY_GLASS
    // ゲルの体を一番下に敷く（反射・ハイライトはその上に乗る）
    lightRgb = clamp(lightRgb + gumBody.rgb * (1.0 - lightA), 0.0, 1.0);
    lightA = clamp(lightA + gumBody.a * (1.0 - lightA), 0.0, 1.0);
#endif
    lightA += (1.0 - lightA) * contentReflectionCoverage;
    lightA = max(lightA,
                 max(lightRgb.r, max(lightRgb.g, lightRgb.b)));

    // ---- キャプチャ疑似屈折（品質=Qualityのみ。設計md 4章） ----
    // 自FBOキャプチャを縁の法線に沿って内側へずらしてサンプルし、
    // ライティングの下敷きとして合成する。R/BはuChromaticぶん距離を変えて
    // 本物の分散による色収差を出す。キャプチャの透過部(alpha=0)では
    // 屈折の寄与が自動的に消え、従来のライティングのみに落ちる。
    vec3 outRgb = lightRgb;
    float outA  = lightA;
    float cover = lightA; // dstをどれだけ退かせるか(dual-sourceの第2出力)
    if (uRefractionPrepass > 0.5 && uRefractMix <= 0.001) {
        fragColor = vec4(0.0);
        fragCover = vec4(0.0);
        return;
    }
    if (uRefractMix > 0.001) {
        // 2026-07-19(3): 「自コンテンツが映りすぎ」「角の内側の溝」
        // 「カメラ移動でゆらゆら」の3点対策でサンプル方向を再設計(実機確認)。
        //   - 厚いガラス板の縁は本来「背景(バブルの外)」を屈折して見せる。
        //     外周端(rt=0)で外側0.6bez → 内側境界(rt=1)で内側0.15bez
        //     (自コンテンツが少しだけ映る)への単調な線形遷移にする。
        //   - 旧実装(全帯で内側サンプル)は角で内向き写像が折り畳まれて
        //     「溝」状の像の折り返しが出ていた。単調遷移なら折り返しが無い。
        //   - 拡大率も約7倍→約1.7倍に下がり、カメラ移動時のサブピクセル
        //     揺れ(ゆらゆら)が大幅に減る。
        //   - さらに法線マップxyの「ガラス面のゆらぎ」をバブル面全体に
        //     薄く乗せる(このためQuality時はリングではなく全面クワッドで
        //     描画する。draw()のジオメトリ分岐参照)。
        // 2026-07-19(6): SDFのmax(q.x,q.y)は対角の中軸線で勾配が不連続
        // (x辺支配⇔y辺支配の切り替わり)なため、これを距離に使うと法線場に
        // 角から対角へ伸びる切れ目が出る(Shift+Dで確認)。湾曲量bendの計算
        // だけ、maxをソフトマックスへ置き換えた滑らかな距離dSoftを使う
        // (縁のライティング帯は正確なdのままなので見た目の位置は変わらない)。
        // 2026-07-19(7): 「湾曲をよりエッジ側へ寄せたい」(ユーザー指示、
        // 参考イメージあり)。平滑幅2bezだと角の湾曲が内側へ膨らみすぎる
        // ため1bezへ縮小。湾曲の届く深さも2.5bez→1.3bezへ絞り、
        // 中央の平坦領域を大きく取る。
        // 2026-07-19(8): min()/length()の分岐が残っていると、クランプ点の
        // 等値線上に微小な不連続の輪が出る(コントラストを上げると見える)。
        // 分岐なしの純粋なソフトマックス場 dSoft = smax(q.x,q.y) - r に変更。
        // 全域C∞で、境界(dSoft=0)は正確なSDFの縁とほぼ一致する。
        float normalScale = max(uNormalScalePx, 4.0);
        float smW = normalScale * 0.5;
        float dSoft = 0.5 * ((q.x + q.y) + sqrt((q.x - q.y) * (q.x - q.y) + smW * smW)) - uRadius;
        float rtRaw = -dSoft / normalScale; // 0=外周端、内側ほど増加
        // 2026-07-19(5): 「内側境界で外向き→内向きへ反転」する区分プロファイル
        // は、境界で微分が不連続になり法線場に四角い裂け目(稜線)が出ていた
        // (Shift+Dで確認)。方向反転を廃止し、球の法線マップのような
        // 「縁で最大→中央へなめらかに0」の単一のクッション形湾曲に一本化:
        //   tilt = 0.6bez × (1 - smoothstep(0, 2.5, rtRaw))、方向は常に外側。
        // smoothstepは両端で微分0なので全域C1連続=稜線が出ない。
        // 終端を持たない指数減衰プロファイル(全域C∞)。有限区間のプロファイル
        // ((1-t)²等)は終端の高次不連続がマッハバンドとして知覚されるため、
        // 「どこにも境界が無い」固定曲線にする。確定パラメーターは
        // この法線プロファイル自体を変更せず、算出後の変位だけへ適用する。
        // ランチャーは小さな正方形なので、動画と同じ減衰だと屈折曲率が
        // 面の中央近くまで届きすぎる。専用プロファイルでは外周の最大傾斜を
        // 保ったまま中央方向へ速く減衰させ、外周リム中心の見た目に揃える。
        float rtPositive = max(rtRaw, 0.0);
        float mediaBend = exp(-2.6 * rtPositive);
        float launcherBend =
            launcherBendProfile(rtPositive, cornerRatio);
        float bend =
            mix(mediaBend, launcherBend, uSurfaceProfile);
#ifdef CARVED_GLASS
        // 削り出し: 面取りの断面から作った法線でゆがませる（法線表示と同じ式）。
        // 外周の面取りは外向き、内側の面取りは内向きに一定の傾きで、平らな
        // ベゼルとコンテンツは傾き0。以降のずれ量の計算・キャプチャの読み方・
        // フェードは標準と完全に同じ（refractionField 以降は共通）。
        bend = carveBend(d, bez, carvePx, carveBandO, carveBandI, bezelSideMask, uInnerMix);
#endif
        // 削り出しでは内側の面取りが負の傾きになるので、増幅は大きさで掛ける
        // （標準は bend>=0 なので abs を付けても結果は変わらない）。
        float edgeGain = 1.0 + uEdgeBoost * abs(bend);
        // exe/lnkで確定した向きに、動画・画像も統一する。
        const float refractionDirection = -1.0;
        vec2 refractionField =
            nrm * bend * edgeGain * uNormalInfluence
            * refractionDirection;
        // refractionFieldはscreen座標(y下向き)、サンプル位置はGL座標
        // (y上向き)なのでyだけ反転する。
        vec2 ofsPx =
            vec2(refractionField.x, -refractionField.y)
            * (normalScale * uRefraction) * uDpr;
        vec2 base = gl_FragCoord.xy;
        // 確定ブラー値は0なので、同一位置だったGの3タップを1回へ統合。
        // G/R/Bの合計3サンプル。テクスチャの線形補間が穏やかな
        // ローパスを兼ねるため、追加ブラーなしでもジャギーは出にくい。
        vec2 uvG = captureUv(base + ofsPx);
        vec2 uvR = captureUv(base + ofsPx * (1.0 + uChromatic));
        vec2 uvB = captureUv(base + ofsPx * (1.0 - uChromatic));
        vec4 sG = texture(uCapture, uvG);
        vec4 sR = texture(uCapture, uvR);
        vec4 sB = texture(uCapture, uvB);
        vec4 refr = vec4(sR.r, sG.g, sB.b, sG.a);  // premultiplied
        // 背景プリパス: itemを描く前のdstを、屈折サンプルでバブル全面に
        // 置き換える。中央は変位が小さければ同一位置サンプルとなり厳密に
        // 恒等となる。refraction/edgeBoostは法線を変えず、算出後の変位量
        // だけを調整する。item本体はこの後に描くため、全面置換しても
        // アイコンや動画を消さない。
        if (uRefractionPrepass > 0.5) {
            float passMask = uRefractMix * uIntensity * insideMask * uTint.a;
            if (uPrepassHoleAmount > 0.0) {
                // 相手の中身の内側（縁から8pxでなめらかに）では屈折を出さない
                vec2 hd = uPrepassHoleHalf - abs(vPos - uPrepassHoleCenter);
                float inHole = smoothstep(0.0, 8.0, min(hd.x, hd.y));
                passMask *= 1.0 - uPrepassHoleAmount * inHole;
            }
            if (uDebugNormal > 1.5) {
                vec3 debugRefr =
                    refr.rgb + vec3(0.08, 0.11, 0.16) * (1.0 - refr.a);
                fragColor = vec4(debugRefr * passMask, passMask);
            } else {
                fragColor = vec4(refr.rgb * passMask, refr.a * passMask);
            }
            fragCover = vec4(passMask);
            return;
        }
    }
    // デバッグ2で屈折が無効(Medium等)の場合は何も出さない
    if (uDebugNormal > 1.5) { fragColor = vec4(0.0); fragCover = vec4(0.0); return; }

    float surfaceK = uIntensity * insideMask * uTint.a;
    float bloomK = uIntensity * strongOuterBloom * uTint.a;
    fragColor = vec4(
        clamp(outRgb * surfaceK + vec3(bloomK), 0.0, 1.0),
        clamp(outA * surfaceK + bloomK, 0.0, 1.0));
    fragCover = vec4(clamp(cover * surfaceK + bloomK, 0.0, 1.0));
}

void main() {
    glassMain();
    // dual-source blending (ONE, ONE_MINUS_SRC1_ALPHA) の再現:
    // rgb=加算色、alpha=dstを退かせる量 → (ONE, ONE_MINUS_SRC_ALPHA) で同じ式になる
    outColor = vec4(fragColor.rgb, fragCover.a);
}
