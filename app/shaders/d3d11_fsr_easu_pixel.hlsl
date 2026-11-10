// GFN fork: AMD FidelityFX Super Resolution 1.0 - EASU (Edge Adaptive Spatial Upsampling)
//
// Port of the FP32 path of ffx_fsr1.h to plain HLSL (SM 5.0).
// Original: Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Licensed under the MIT license (see LICENSE-FSR in this folder).
//
// Input: RGB texture at stream resolution (t0)
// Output: RGB at output resolution, one invocation per output pixel.

Texture2D<float4> inputTexture : register(t0);
SamplerState theSampler : register(s0);

cbuffer EASU_CONST_BUF : register(b1)
{
    float4 con0;
    float4 con1;
    float4 con2;
    float4 con3;
};

struct ShaderInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

// Filtering for a given tap for the scalar.
void FsrEasuTap(inout float3 aC,   // Accumulated color, with negative lobe.
                inout float aW,    // Accumulated weight.
                float2 off,        // Pixel offset from resolve position to tap.
                float2 dir,        // Gradient direction.
                float2 len,        // Length.
                float lob,         // Negative lobe strength.
                float clp,         // Clipping point.
                float3 c)          // Tap color.
{
    // Rotate offset by direction.
    float2 v;
    v.x = (off.x * ( dir.x)) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    // Anisotropy.
    v *= len;
    // Compute distance^2.
    float d2 = v.x * v.x + v.y * v.y;
    // Limit to the window as at corner, 2 taps can easily be outside.
    d2 = min(d2, clp);
    // Approximation of lanczos2 without sin() or rcp(), or sqrt() to get x.
    //  (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (1/4 * x^2 - 1)^2
    //  |_______________________________________|   |_______________|
    //                   base                             window
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    // Do weighted average.
    aC += c * w;
    aW += w;
}

// Accumulate direction and length.
void FsrEasuSet(inout float2 dir,
                inout float len,
                float2 pp,
                bool biS, bool biT, bool biU, bool biV,
                float lA, float lB, float lC, float lD, float lE)
{
    // Compute bilinear weight, branches factor out as predicates are compiler time immediates.
    //  s t
    //  u v
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w =        pp.x  * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) *        pp.y ;
    if (biV) w =        pp.x  *        pp.y ;

    // Direction is the '+' diff.
    //    a
    //  b c d
    //    e
    // Then takes magnitude from abs average of both sides of 'c'.
    // Length converts gradient reversal to 0, smoothly to non-reversal at 1, shaped, then adding horz and vert terms.
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = 1.0 / max(lenX, 1.0 / 32768.0);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;

    // Repeat for the y axis.
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = 1.0 / max(lenY, 1.0 / 32768.0);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

float4 main(ShaderInput input) : SV_TARGET
{
    // Integer output pixel position
    float2 ip = floor(input.pos.xy);

    // Get position of 'f'.
    float2 pp = ip * con0.xy + con0.zw;
    float2 fp = floor(pp);
    pp -= fp;

    // 12-tap kernel.
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    // Gather 4 ordering.
    //  a b
    //  r g
    // For packed FP16, need either {rg} or {ab} so using the following setup for gather in all versions,
    //    a b    <- unused (z)
    //    r g
    //  a b a b
    //  r g r g
    //    a b
    //    r g    <- unused (z)
    // Allowing dead-code removal to remove the 'z's.
    float2 p0 = fp * con1.xy + con1.zw;
    // These are from p0 to avoid pulling two constants on pre-Navi hardware.
    float2 p1 = p0 + con2.xy;
    float2 p2 = p0 + con2.zw;
    float2 p3 = p0 + con3.xy;

    float4 bczzR = inputTexture.GatherRed(theSampler, p0);
    float4 bczzG = inputTexture.GatherGreen(theSampler, p0);
    float4 bczzB = inputTexture.GatherBlue(theSampler, p0);
    float4 ijfeR = inputTexture.GatherRed(theSampler, p1);
    float4 ijfeG = inputTexture.GatherGreen(theSampler, p1);
    float4 ijfeB = inputTexture.GatherBlue(theSampler, p1);
    float4 klhgR = inputTexture.GatherRed(theSampler, p2);
    float4 klhgG = inputTexture.GatherGreen(theSampler, p2);
    float4 klhgB = inputTexture.GatherBlue(theSampler, p2);
    float4 zzonR = inputTexture.GatherRed(theSampler, p3);
    float4 zzonG = inputTexture.GatherGreen(theSampler, p3);
    float4 zzonB = inputTexture.GatherBlue(theSampler, p3);

    // Simplest multi-channel approximate luma possible (luma times 2, in 2 FMA/MAD).
    float4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    float4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    float4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    float4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);

    // Rename.
    float bL = bczzL.x;
    float cL = bczzL.y;
    float iL = ijfeL.x;
    float jL = ijfeL.y;
    float fL = ijfeL.z;
    float eL = ijfeL.w;
    float kL = klhgL.x;
    float lL = klhgL.y;
    float hL = klhgL.z;
    float gL = klhgL.w;
    float oL = zzonL.z;
    float nL = zzonL.w;

    // Accumulate for bilinear interpolation.
    float2 dir = float2(0.0, 0.0);
    float len = 0.0;
    FsrEasuSet(dir, len, pp, true,  false, false, false, bL, eL, fL, gL, jL);
    FsrEasuSet(dir, len, pp, false, true,  false, false, cL, fL, gL, hL, kL);
    FsrEasuSet(dir, len, pp, false, false, true,  false, fL, iL, jL, kL, nL);
    FsrEasuSet(dir, len, pp, false, false, false, true,  gL, jL, kL, lL, oL);

    // Normalize with approximation, and cleanup close to zero.
    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = rsqrt(max(dirR, 1.0 / 32768.0));
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;

    // Transform from {0 to 2} to {0 to 1} range, and shape with square.
    len = len * 0.5;
    len *= len;

    // Stretch kernel {1.0 vert|horz, to sqrt(2.0) on diagonal}.
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(max(abs(dir.x), abs(dir.y)), 1.0 / 32768.0);

    // Anisotropic length after rotation,
    //  x := 1.0 lerp to 'stretch' on edges
    //  y := 1.0 lerp to 2x on edges
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);

    // Based on the amount of 'edge',
    // the window shifts from +/-{sqrt(2.0) to slightly beyond 2.0}.
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;

    // Set distance^2 clipping point to the end of the adjustable window.
    float clp = 1.0 / lob;

    // Accumulation mixed with min/max of 4 nearest.
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    float3 bC = float3(bczzR.x, bczzG.x, bczzB.x);
    float3 cC = float3(bczzR.y, bczzG.y, bczzB.y);
    float3 iC = float3(ijfeR.x, ijfeG.x, ijfeB.x);
    float3 jC = float3(ijfeR.y, ijfeG.y, ijfeB.y);
    float3 fC = float3(ijfeR.z, ijfeG.z, ijfeB.z);
    float3 eC = float3(ijfeR.w, ijfeG.w, ijfeB.w);
    float3 kC = float3(klhgR.x, klhgG.x, klhgB.x);
    float3 lC = float3(klhgR.y, klhgG.y, klhgB.y);
    float3 hC = float3(klhgR.z, klhgG.z, klhgB.z);
    float3 gC = float3(klhgR.w, klhgG.w, klhgB.w);
    float3 oC = float3(zzonR.z, zzonG.z, zzonB.z);
    float3 nC = float3(zzonR.w, zzonG.w, zzonB.w);

    float3 min4 = min(min(fC, gC), min(jC, kC));
    float3 max4 = max(max(fC, gC), max(jC, kC));

    // Accumulation.
    float3 aC = float3(0.0, 0.0, 0.0);
    float aW = 0.0;
    FsrEasuTap(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, bC); // b
    FsrEasuTap(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, cC); // c
    FsrEasuTap(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, iC); // i
    FsrEasuTap(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, jC); // j
    FsrEasuTap(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, fC); // f
    FsrEasuTap(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, eC); // e
    FsrEasuTap(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, kC); // k
    FsrEasuTap(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, lC); // l
    FsrEasuTap(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, hC); // h
    FsrEasuTap(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, gC); // g
    FsrEasuTap(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, oC); // o
    FsrEasuTap(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, nC); // n

    // Normalize and dering.
    float3 pix = min(max4, max(min4, aC / aW));

    return float4(pix, 1.0);
}
