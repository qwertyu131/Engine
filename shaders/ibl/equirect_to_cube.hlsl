// Panorama -> source cube with its mip chain (plan F3 Task 5): glTF-Sample-Renderer v1.1.0
// panorama_to_cubemap.frag (iblSampler.panoramaToCubeMap, ibl_sampler.js) followed by gl.generateMipmap on the cube.
//   mip 0:  texel = texture(u_panorama, dirToUV(normalize(uvToXYZ(face, texCoord * 2 - 1)))), the panorama a LINEAR,
//           MIRRORED_REPEAT, single-level RGB32F texture (loadTextureHDR);
//   mip m:  generateMipmap's 2 x 2 box filter, applied m times = the mean of the 2^m x 2^m mip-0 texels the texel
//           covers; computed here from the panorama directly (this pass never samples the cube it renders into).
// CPU twin: engine/renderer/IblCpu.cpp panoramaToCube.
#include "ibl_filtering.hlsli"

Texture2D<float4> u_panorama : register(t1, space0);
SamplerState u_panoramaSampler : register(s2, space0);

// filterInts.x = face, filterInts.y = block (2^mip), filterInts.z = mip-0 edge.
float4 PSMain(FullscreenOut input) : SV_Target0
{
    const int face = filterInts.x;
    const int block = filterInts.y;
    const float size0 = float(filterInts.z);
    const int2 texel = int2(input.position.xy);
    float3 sum = float3(0.0, 0.0, 0.0);
    for (int j = 0; j < block; ++j)
    {
        for (int i = 0; i < block; ++i)
        {
            const float2 texCoord = (float2(texel * block + int2(i, j)) + 0.5) / size0;
            const float2 texCoordNew = texCoord * 2.0 - 1.0;
            const float3 scan = uvToXYZ(face, texCoordNew);
            const float3 direction = normalize(scan);
            const float2 src = dirToUV(direction);
            sum += u_panorama.SampleLevel(u_panoramaSampler, src, 0.0).rgb;
        }
    }
    return float4(sum / float(block * block), 1.0);
}
