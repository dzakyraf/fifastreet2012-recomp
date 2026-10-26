#pragma once
// A view of the renderer's register array in the shape the vendored interpreter
// expects. The array is the 0x5003-dword file the command processor keeps.
#include <cstdint>
#include <cstring>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
namespace rex::graphics
{
    class RegisterFile
    {
    public:
        explicit RegisterFile(const uint32_t* values) : values_(values) {}
        const uint32_t& operator[](uint32_t reg) const { return values_[reg]; }
        template <typename T>
        T Get(uint32_t reg) const { T value; std::memcpy(&value, &values_[reg], sizeof(T)); return value; }
        xenos::xe_gpu_vertex_fetch_t GetVertexFetch(uint32_t index) const
        {
            xenos::xe_gpu_vertex_fetch_t fetch;
            std::memcpy(&fetch, &values_[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + (sizeof(fetch) / 4) * index], sizeof(fetch));
            return fetch;
        }
    private:
        const uint32_t* values_;
    };
}
