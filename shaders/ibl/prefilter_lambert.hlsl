// Lambertian (irradiance) prefilter of one cube face (plan F3 Task 5): glTF-Sample-Renderer v1.1.0
// ibl_filtering.frag with u_distribution = cLambertian, as iblSampler.cubeMapToLambertian (ibl_sampler.js) runs it:
// roughness 0, mip 0, u_sampleCount = lambertianSampleCount, u_width = textureSize.
// Port and constants: ibl_filtering.hlsli; CPU twin: engine/renderer/IblCpu.cpp.
#define IBL_FILTER_CUBE 1
#include "ibl_filtering.hlsli"

float4 PSMain(FullscreenOut input) : SV_Target0
{
    return filterMain(input.position, cLambertian);
}
