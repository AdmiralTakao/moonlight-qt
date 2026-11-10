// GFN fork: AMD FidelityFX Super Resolution 1.0 - RCAS (Robust Contrast Adaptive Sharpening)
//
// Port of the FP32 path of ffx_fsr1.h to plain HLSL (SM 5.0), with FSR_RCAS_DENOISE
// enabled because compressed video contains noise we don't want to amplify.
// Original: Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Licensed under the MIT license (see LICENSE-FSR in this folder).
//
// Draws the (already upscaled) image 1:1 into the video rectangle of the back buffer.

Texture2D<float4> inputTexture : register(t0);

cbuffer RCAS_CONST_BUF : register(b1)
{
    float4 rcasParams; // x = sharpening amount (0 = off, 1 = maximum)
    float4 rcasRect;   // xy = top-left of the video rectangle in render target pixels, zw = input texture size
};

struct ShaderInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

// This is set at the limit of providing unnatural results for sharpening.
#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

float3 RcasLoad(int2 p)
{
    p = clamp(p, int2(0, 0), int2(rcasRect.zw) - int2(1, 1));
    return inputTexture.Load(int3(p, 0)).rgb;
}

float4 main(ShaderInput input) : SV_TARGET
{
    int2 sp = int2(floor(input.pos.xy - rcasRect.xy));

    // Algorithm uses minimal 3x3 pixel neighborhood.
    //    b
    //  d e f
    //    h
    float3 b = RcasLoad(sp + int2( 0, -1));
    float3 d = RcasLoad(sp + int2(-1,  0));
    float3 e = RcasLoad(sp);
    float3 f = RcasLoad(sp + int2( 1,  0));
    float3 h = RcasLoad(sp + int2( 0,  1));

    // Early out if sharpening is disabled (exact copy)
    if (rcasParams.x <= 0.0) {
        return float4(e, 1.0);
    }

    // Luma times 2.
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
    float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
    float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hL = h.b * 0.5 + (h.r * 0.5 + h.g);

    // Noise detection.
    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    float rangeL = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);
    nz = saturate(abs(nz) / max(rangeL, 1.0 / 32768.0));
    nz = -0.5 * nz + 1.0;

    // Min and max of ring.
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));

    // Immediate constants for peak range.
    float2 peakC = float2(1.0, -1.0 * 4.0);

    // Limiters, these need to be high precision RCPs.
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1.0 / 32768.0);
    float3 hitMax = (peakC.x - max(mx4, e)) / min(4.0 * mn4 + peakC.y, -1.0 / 32768.0);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-FSR_RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * rcasParams.x;

    // Apply noise removal.
    lobe *= nz;

    // Resolve.
    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    float3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;

    return float4(pix, 1.0);
}
