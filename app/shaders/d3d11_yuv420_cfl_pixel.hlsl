// GFN fork: "Artificial 4:4:4"
//
// YUV 4:2:0 -> RGB conversion with luma-guided chroma reconstruction
// (Chroma-from-Luma). Regular 4:2:0 rendering bilinearly upsamples the
// half-resolution chroma planes, which makes colored edges and text look
// blurry. Here we fit a local linear model chroma = a * luma + b on the
// 2x2 chroma neighborhood and use the full-resolution luma to predict
// sharper chroma. The prediction is only used where luma actually explains
// the chroma (high correlation and a real luma edge), otherwise we fall
// back to normal bilinear chroma.

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
    float4 cflParams;  // x = reconstruction strength (0..1)
};

float4 main(ShaderInput input) : SV_TARGET
{
    // Clamp the chrominance texcoords to avoid sampling the row of texels adjacent to the alignment padding
    float2 chromaTex = min(input.tex + chromaOffset, chromaTexMax);

    float lumaVal = luminancePlane.Sample(theSampler, input.tex);
    float2 chromaBilinear = chrominancePlane.Sample(theSampler, chromaTex);

    // Find the 2x2 chroma texel neighborhood that bilinear filtering uses
    float2 chromaSize = planeSizes.zw;
    float2 invChromaSize = 1.0 / chromaSize;
    float2 chromaPos = chromaTex * chromaSize - 0.5;
    float2 chromaBase = floor(chromaPos);
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

    // Local linear regression of chroma against luma
    const float4 quarter = float4(0.25, 0.25, 0.25, 0.25);
    float yMean = dot(y4, quarter);
    float2 cMean = float2(dot(u4, quarter), dot(v4, quarter));
    float4 dY = y4 - yMean;
    float4 dU = u4 - cMean.x;
    float4 dV = v4 - cMean.y;

    float varY = dot(dY, dY);
    float2 cov = float2(dot(dY, dU), dot(dY, dV));
    float2 varC = float2(dot(dU, dU), dot(dV, dV));

    float2 slope = cov / max(varY, 1e-6);
    float2 chromaCfl = cMean + slope * (lumaVal - yMean);

    // Never overshoot the local chroma range (avoids halos/ringing)
    float2 cMin = float2(min(min(u4.x, u4.y), min(u4.z, u4.w)),
                         min(min(v4.x, v4.y), min(v4.z, v4.w)));
    float2 cMax = float2(max(max(u4.x, u4.y), max(u4.z, u4.w)),
                         max(max(v4.x, v4.y), max(v4.z, v4.w)));
    chromaCfl = clamp(chromaCfl, cMin, cMax);

    // Confidence: r^2 of the fit times "is there a real luma edge here?"
    float2 r2 = saturate((cov * cov) / max(varY * varC, 1e-10));
    float edge = saturate(varY * 400.0);
    float2 alpha = r2 * edge * cflParams.x;

    float2 chroma = lerp(chromaBilinear, chromaCfl, alpha);

    float3 yuv = float3(lumaVal, chroma);

    // Subtract the YUV offset for limited vs full range
    yuv -= offsets;

    // Multiply by the conversion matrix for this colorspace
    yuv = mul(yuv, cscMatrix);

    return float4(yuv, 1.0);
}
