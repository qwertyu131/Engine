// GGX prefilter of one cube face / mip (plan F3 Task 5): glTF-Sample-Renderer v1.1.0 ibl_filtering.frag with
// u_distribution = cGGX, as iblSampler.cubeMapToGGX (ibl_sampler.js) runs it per level: roughness = level /
// (mipmapLevels - 1), u_sampleCount = ggxSampleCount, u_width = textureSize (the mip-0 edge at every level).
// Port and constants: ibl_filtering.hlsli; CPU twin: engine/renderer/IblCpu.cpp.
#define IBL_FILTER_CUBE 1
#include "ibl_filtering.hlsli"

float4 PSMain(FullscreenOut input) : SV_Target0
{
    return filterMain(input.position, cGGX);
}
