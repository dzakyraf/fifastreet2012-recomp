SPIR-V of AMD FidelityFX Super Resolution 1 (EASU upscale, RCAS sharpen) and the rectangle vertex shader
that draws them, as built by Xenia's `xb buildshaders` and shipped in the ReXGlue SDK
(`Skate3-Port/third_party/rexglue-sdk/src/ui/shaders/vulkan_spirv/`). Copied unchanged.
FidelityFX FSR: Copyright (c) AMD, MIT licence. Xenia / ReXGlue: BSD-3-Clause.
Interface (upscaler.cpp): set 0, binding 0 a sampled image, binding 1 a sampler; push constants: the
vertex shader's rectangle in NDC {x, y, width, height} at offset 0, then the pass's constants at 16 --
EASU {input/output size ratio[2], 1/input size[2]}, RCAS {output offset int[2], sharpness}.
