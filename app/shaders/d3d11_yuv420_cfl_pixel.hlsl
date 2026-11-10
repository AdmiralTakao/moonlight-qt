// GFN fork: "Artificial 4:4:4"
//
// YUV 4:2:0 -> RGB conversion with luma-guided chroma upsampling
// (joint bilateral upsampling). Regular 4:2:0 rendering bilinearly upsamples
// the half-resolution chroma planes, which makes colors bleed across edges
// and text look blurry. Here every chroma sample of the bilinear 2x2
// footprint is additionally weighted by how similar its luma is to the luma
// of the output pixel, so colors stop at luma edges.
//
// The weights are continuous across chroma texel boundaries (samples leaving
// the footprint always have a bilinear weight of 0), so this never produces
// block or grid artifacts.

Texture2D<float> luminancePlane : register(t0);
Texture2D<float2> chrominancePlane : register(t1);
SamplerState theSampler : register(s0);

struct ShaderInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

cbuffer CSC_CONST_BUF : register(b0)
{
    float3x3 cscMatrix;
    float3 offsets;
    float2 chromaOffset;
    float2 chromaTexMax;
    float4 planeSizes; // xy = luma texture size, zw = chroma texture size (texels)
    float4 cflParams;  // x = strength (0..1), y = luma similarity factor 1/(2*sigma^2)
};

float4 main(ShaderInput input) : SV_TARGET
{
    // Clamp the chrominance texcoords to avoid sampling the row of texels adjacent to the alignment padding
    float2 chromaTex = min(input.tex + chromaOffset, chromaTexMax);

    float lumaVal = luminancePlane.Sample(theSampler, input.tex);
    float2 chromaBilinear = chrominancePlane.Sample(theSampler, chromaTex);

    // Find the 2x2 chroma texel footprint that bilinear filtering uses
    float2 chromaSize = planeSizes.zw;
    float2 invChromaSize = 1.0 / chromaSize;
    float2 chromaPos = chromaTex * chromaSize - 0.5;
    float2 chromaBase = floor(chromaPos);
    float2 f = chromaPos - chromaBase;
    float2 gatherUV = (chromaBase + 1.0) * invChromaSize;

    // Gather component order: x = (0,1), y = (1,1), z = (1,0), w = (0,0)
    float4 u4 = chrominancePlane.GatherRed(theSampler, gatherUV);
    float4 v4 = chrominancePlane.GatherGreen(theSampler, gatherUV);

    // Luma at the chroma sample sites. Bilinear sampling of the luma plane at the
    // chroma site averages the luma pixels the chroma sample was derived from.
    float2 site00 = (chromaBase + 0.5) * invChromaSize - chromaOffset;
    float2 dx = float2(invChromaSize.x, 0.0);
    float2 dy = float2(0.0, invChromaSize.y);
    float4 y4;
    y4.x = luminancePlane.SampleLevel(theSampler, site00 + dy, 0);
    y4.y = luminancePlane.SampleLevel(theSampler, site00 + dx + dy, 0);
    y4.z = luminancePlane.SampleLevel(theSampler, site00 + dx, 0);
    y4.w = luminancePlane.SampleLevel(theSampler, site00, 0);

    // Spatial (bilinear) weights in gather order
    float4 wSpatial = float4((1.0 - f.x) * f.y,
                             f.x * f.y,
                             f.x * (1.0 - f.y),
                             (1.0 - f.x) * (1.0 - f.y));

    // Range weights: prefer chroma samples whose luma matches this pixel
    float4 dY = y4 - lumaVal;
    float4 wRange = exp(-(dY * dY) * cflParams.y);

    float4 w = wSpatial * wRange;
    const float4 ones = float4(1.0, 1.0, 1.0, 1.0);
    float wSum = dot(w, ones);
    float2 chromaJbu = float2(dot(w, u4), dot(w, v4)) / max(wSum, 1e-5);

    // If no sample in the footprint matches (thin features), keep bilinear chroma
    float alpha = saturate(wSum * 20.0) * cflParams.x;
    float2 chroma = lerp(chromaBilinear, chromaJbu, alpha);

    float3 yuv = float3(lumaVal, chroma);

    // Subtract the YUV offset for limited vs full range
    yuv -= offsets;

    // Multiply by the conversion matrix for this colorspace
    yuv = mul(yuv, cscMatrix);

    return float4(yuv, 1.0);
}
