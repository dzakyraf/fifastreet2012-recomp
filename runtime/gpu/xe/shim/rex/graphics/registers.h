#pragma once
// The register numbers the interpreter reads shader constants from.
#define XE_GPU_REG_SHADER_CONSTANT_000_X 0x4000
#define XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 0x4800
#define XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 0x4900
#define XE_GPU_REG_SHADER_CONSTANT_LOOP_00 0x4908
#define XE_GPU_REG_SQ_VS_CONST 0x2307
#define XE_GPU_REG_SQ_PS_CONST 0x2308
#include <cstdint>
namespace rex::graphics::reg
{
    union SQ_VS_CONST
    {
        uint32_t value;
        struct
        {
            uint32_t base : 9;
            uint32_t _pad_9 : 3;
            uint32_t size : 9;      // vec4 count minus one
            uint32_t _pad_21 : 11;
        };
    };
    using SQ_PS_CONST = SQ_VS_CONST;
}
