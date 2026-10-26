#include "memexport.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/register_file.h>

namespace
{
    using namespace rex::graphics;

    uint32_t Bits(uint32_t value, uint32_t offset, uint32_t width) { return (value >> offset) & ((1u << width) - 1); }

    // The title's endianness for the exported element, applied to its packed
    // four dwords the way a texture fetch undoes it.
    void EndianSwap(uint32_t (&v)[4], uint32_t endian)
    {
        auto swap16in32 = [](uint32_t x) { return (x >> 16) | (x << 16); };
        auto swap8in16 = [](uint32_t x) { return ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu); };
        for (uint32_t& x : v)
        {
            switch (endian)
            {
            case 1: x = swap8in16(x); break;
            case 2: case 4: case 5: x = __builtin_bswap32(x); break;
            case 3: x = swap16in32(x); break;
            default: break;
            }
        }
        if (endian == 4) { std::swap(v[0], v[1]); std::swap(v[2], v[3]); }
        else if (endian == 5) { std::swap(v[0], v[3]); std::swap(v[1], v[2]); }
    }

    // One element's float4 turned into the format's bits: widths in order from
    // the lowest, as Xenia's pack_8_16_32.
    uint32_t Pack(const float (&e)[4], const uint32_t* widths, uint32_t count, bool isSigned, bool isNorm)
    {
        uint32_t packed = 0, offset = 0;
        for (uint32_t c = 0; c < count; c++)
        {
            float v = e[c];
            if (std::isnan(v)) v = 0.0f;
            const uint32_t w = widths[c];
            int64_t integer;
            if (isSigned)
            {
                if (isNorm) v = std::min(std::max(v, -1.0f), 1.0f) * float((1u << (w - 1)) - 1);
                integer = int32_t(v + (v < 0.0f ? -0.5f : 0.5f));
            }
            else
            {
                if (isNorm) v = std::min(std::max(v, 0.0f), 1.0f) * float((1u << w) - 1);
                integer = int64_t(uint32_t(v + 0.5f));
            }
            packed |= (uint32_t(integer) & ((w >= 32) ? ~0u : ((1u << w) - 1))) << offset;
            offset += w;
        }
        return packed;
    }

    uint32_t Half(float v)
    {
        // Round to nearest half; the Xenos' extended range is not modelled.
        uint32_t f; std::memcpy(&f, &v, 4);
        const uint32_t sign = (f >> 16) & 0x8000;
        int32_t exponent = int32_t((f >> 23) & 0xFF) - 127 + 15;
        uint32_t mantissa = f & 0x7FFFFF;
        if (((f >> 23) & 0xFF) == 0xFF) return sign | 0x7BFF;      // inf/nan clamp
        if (exponent >= 31) return sign | 0x7BFF;
        if (exponent <= 0)
        {
            if (exponent < -10) return sign;
            mantissa |= 0x800000;
            const uint32_t shift = uint32_t(14 - exponent);
            return sign | ((mantissa + (1u << (shift - 1))) >> shift);
        }
        const uint32_t rounded = (uint32_t(exponent) << 10 | (mantissa >> 13)) + ((mantissa >> 12) & 1);
        return sign | rounded;
    }

    struct Sink : ShaderInterpreter::ExportSink
    {
        float eA[4] = {};
        float eM[5][4] = {};
        uint32_t written = 0;
        bool addressWritten = false;
        void Reset() { std::memset(eA, 0, sizeof eA); std::memset(eM, 0, sizeof eM); written = 0; addressWritten = false; }
        void Export(ucode::ExportRegister reg, const float* value, uint32_t mask) override
        {
            const uint32_t r = uint32_t(reg);
            float* target = nullptr;
            if (r == uint32_t(ucode::ExportRegister::kExportAddress)) { target = eA; addressWritten = true; }
            else if (r >= uint32_t(ucode::ExportRegister::kExportData0) && r <= uint32_t(ucode::ExportRegister::kExportData4))
            {
                target = eM[r - uint32_t(ucode::ExportRegister::kExportData0)];
                written |= 1u << (r - uint32_t(ucode::ExportRegister::kExportData0));
            }
            if (!target) return;
            for (uint32_t c = 0; c < 4; c++)
                if (mask & (1u << c)) target[c] = value[c];
        }
    };

    // Stores what one invocation exported; the bounds checks keep a bad
    // address from leaving the bank.
    bool Store(const Sink& sink, const xe_memexport::Job& job)
    {
        if (!sink.addressWritten || !sink.written) return false;
        uint32_t a[4];
        std::memcpy(a, sink.eA, sizeof a);
        if ((a[0] >> 30) != 1 || (a[1] >> 23) != 0x96 || (a[2] >> 23) != 0x96 || (a[3] >> 23) != 0x96) return false;

        const uint32_t formatInfo = a[2] & 0x7FFFFF;
        const uint32_t eM0Index = a[1] & 0x7FFFFF;
        const int32_t remaining = std::clamp(int32_t(a[3] & 0x7FFFFF) - int32_t(eM0Index), 0, 5);
        const uint32_t toStore = sink.written & ((1u << remaining) - 1);
        if (!toStore) return false;

        const bool swapRedBlue = formatInfo & (1u << 19);
        const bool isSigned = formatInfo & (1u << 16);
        const bool isNorm = !(formatInfo & (1u << 17));
        const uint32_t endian = formatInfo & 7;
        const uint32_t format = Bits(formatInfo, 8, 6);

        struct Layout { uint32_t widths[4]; uint32_t count; uint32_t log2; };
        Layout layout{};
        enum { kInt, kHalf, kFloat } kind = kInt;
        switch (xenos::ColorFormat(format))
        {
        case xenos::ColorFormat::k_8: case xenos::ColorFormat::k_8_A: case xenos::ColorFormat::k_8_B:
            layout = { { 8 }, 1, 0 }; break;
        case xenos::ColorFormat::k_1_5_5_5: layout = { { 5, 5, 5, 1 }, 4, 1 }; break;
        case xenos::ColorFormat::k_5_6_5: layout = { { 5, 6, 5 }, 3, 1 }; break;
        case xenos::ColorFormat::k_6_5_5: layout = { { 5, 5, 6 }, 3, 1 }; break;
        case xenos::ColorFormat::k_8_8_8_8: case xenos::ColorFormat::k_8_8_8_8_A:
        case xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16:
            layout = { { 8, 8, 8, 8 }, 4, 2 }; break;
        case xenos::ColorFormat::k_2_10_10_10: case xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16:
            layout = { { 10, 10, 10, 2 }, 4, 2 }; break;
        case xenos::ColorFormat::k_8_8: layout = { { 8, 8 }, 2, 1 }; break;
        case xenos::ColorFormat::k_4_4_4_4: layout = { { 4, 4, 4, 4 }, 4, 1 }; break;
        case xenos::ColorFormat::k_10_11_11: case xenos::ColorFormat::k_10_11_11_AS_16_16_16_16:
            layout = { { 11, 11, 10 }, 3, 2 }; break;
        case xenos::ColorFormat::k_11_11_10: case xenos::ColorFormat::k_11_11_10_AS_16_16_16_16:
            layout = { { 10, 11, 11 }, 3, 2 }; break;
        case xenos::ColorFormat::k_16: layout = { { 16 }, 1, 1 }; break;
        case xenos::ColorFormat::k_16_16: layout = { { 16, 16 }, 2, 2 }; break;
        case xenos::ColorFormat::k_16_16_16_16: layout = { { 16, 16, 16, 16 }, 4, 3 }; break;
        case xenos::ColorFormat::k_16_FLOAT: layout = { {}, 1, 1 }; kind = kHalf; break;
        case xenos::ColorFormat::k_16_16_FLOAT: layout = { {}, 2, 2 }; kind = kHalf; break;
        case xenos::ColorFormat::k_16_16_16_16_FLOAT: layout = { {}, 4, 3 }; kind = kHalf; break;
        case xenos::ColorFormat::k_32_FLOAT: layout = { {}, 1, 2 }; kind = kFloat; break;
        case xenos::ColorFormat::k_32_32_FLOAT: layout = { {}, 2, 3 }; kind = kFloat; break;
        case xenos::ColorFormat::k_32_32_32_32_FLOAT: layout = { {}, 4, 4 }; kind = kFloat; break;
        default: return false;
        }

        const uint64_t byte0 = (uint64_t(a[0]) << 2 & 0xFFFFFFFFu) + (uint64_t(eM0Index) << layout.log2);
        for (uint32_t i = 0; i < 5; i++)
        {
            if (!(toStore & (1u << i))) continue;
            float e[4] = { sink.eM[i][0], sink.eM[i][1], sink.eM[i][2], sink.eM[i][3] };
            if (swapRedBlue) std::swap(e[0], e[2]);

            uint32_t v[4] = {};
            if (kind == kFloat)
            {
                for (uint32_t c = 0; c < layout.count; c++) std::memcpy(&v[c], &e[c], 4);
            }
            else if (kind == kHalf)
            {
                for (uint32_t c = 0; c < layout.count; c++)
                {
                    const uint32_t h = Half(std::isnan(e[c]) ? 0.0f : e[c]);
                    if (layout.count == 1) v[0] = h;
                    else v[c / 2] |= h << (16 * (c & 1));
                }
            }
            else if (layout.log2 == 3)      // four 16-bit values in two dwords
            {
                for (uint32_t pair = 0; pair < 2; pair++)
                {
                    const float two[4] = { e[2 * pair], e[2 * pair + 1], 0, 0 };
                    const uint32_t w[2] = { 16, 16 };
                    v[pair] = Pack(two, w, 2, isSigned, isNorm);
                }
            }
            else
            {
                v[0] = Pack(e, layout.widths, layout.count, isSigned, isNorm);
            }
            EndianSwap(v, endian);

            const uint64_t address = byte0 + (uint64_t(i) << layout.log2);
            const uint32_t size = 1u << layout.log2;
            if (address + size > job.physicalSize) continue;
            uint8_t* destination = job.physicalBase + address;
            // Little-endian bytes of the swapped words: the memory holds what
            // the guest wrote, so the title's own byte order is in `endian`.
            if (size >= 4) std::memcpy(destination, v, size);
            else if (size == 2) { const uint16_t h = uint16_t(v[0]); std::memcpy(destination, &h, 2); }
            else destination[0] = uint8_t(v[0]);
        }
        return true;
    }
}

uint32_t xe_memexport::Execute(const Job& job)
{
    static RegisterFile* none = nullptr; (void)none;
    RegisterFile registers(job.registers);
    rex::memory::Memory memory(job.physicalBase);
    ShaderInterpreter interpreter(registers, memory);
    interpreter.SetShader(xenos::ShaderType::kVertex, job.vertexCode);
    Sink sink;
    interpreter.SetExportSink(&sink);

    uint32_t stored = 0;
    for (uint32_t i = 0; i < job.vertexCount; i++)
    {
        uint32_t index = i;
        if (job.indices)
        {
            if (job.index32) { uint32_t v; std::memcpy(&v, job.indices + 4 * i, 4); index = __builtin_bswap32(v) & 0xFFFFFF; }
            else { uint16_t v; std::memcpy(&v, job.indices + 2 * i, 2); index = uint16_t(__builtin_bswap16(v)); }
        }
        sink.Reset();
        interpreter.temp_registers()[0] = float(index);
        interpreter.Execute();
        if (Store(sink, job)) stored++;
    }
    interpreter.SetExportSink(nullptr);
    return stored;
}

namespace
{
    struct TraceSink : ShaderInterpreter::ExportSink
    {
        std::string text;
        void Export(ucode::ExportRegister reg, const float* v, uint32_t mask) override
        {
            char t[128];
            std::snprintf(t, sizeof t, " e%u(%x)=[%g %g %g %g]", uint32_t(reg), mask, v[0], v[1], v[2], v[3]);
            text += t;
        }
    };
}

namespace
{
    struct PositionSink : ShaderInterpreter::ExportSink
    {
        bool seen = false;
        float position[4]{};
        void Export(ucode::ExportRegister reg, const float* v, uint32_t mask) override
        {
            if (uint32_t(reg) != 62) return;
            for (int i = 0; i < 4; i++) if (mask & (1u << i)) position[i] = v[i];
            seen = true;
        }
    };
}

bool xe_memexport::Position(const Job& job, uint32_t index, float* position)
{
    RegisterFile registers(job.registers);
    rex::memory::Memory memory(job.physicalBase);
    ShaderInterpreter interpreter(registers, memory);
    interpreter.SetShader(xenos::ShaderType::kVertex, job.vertexCode);
    PositionSink sink;
    interpreter.SetExportSink(&sink);
    interpreter.temp_registers()[0] = float(index + job.firstVertex);
    interpreter.Execute();
    interpreter.SetExportSink(nullptr);
    if (!sink.seen) return false;
    for (int i = 0; i < 4; i++) position[i] = sink.position[i];
    return true;
}

bool xe_memexport::PositionZ(const Job& job, float& z)
{
    RegisterFile registers(job.registers);
    rex::memory::Memory memory(job.physicalBase);
    ShaderInterpreter interpreter(registers, memory);
    interpreter.SetShader(xenos::ShaderType::kVertex, job.vertexCode);
    PositionSink sink;
    interpreter.SetExportSink(&sink);
    interpreter.temp_registers()[0] = float(job.firstVertex);
    interpreter.Execute();
    interpreter.SetExportSink(nullptr);
    if (!sink.seen) return false;
    z = sink.position[3] != 0.0f ? sink.position[2] / sink.position[3] : sink.position[2];
    return true;
}

void xe_memexport::TraceVertices(const Job& job, void (*line)(const char* text))
{
    RegisterFile registers(job.registers);
    rex::memory::Memory memory(job.physicalBase);
    ShaderInterpreter interpreter(registers, memory);
    interpreter.SetShader(xenos::ShaderType::kVertex, job.vertexCode);
    TraceSink sink;
    interpreter.SetExportSink(&sink);
    // The first six vertices, and for an indexed draw also six from the middle
    // (a later billboard of a quad list).
    const uint32_t picks[12] = { 0, 1, 2, 3, 4, 5, job.vertexCount / 2, job.vertexCount / 2 + 1,
                                 job.vertexCount / 2 + 2, job.vertexCount / 2 + 3, job.vertexCount - 2, job.vertexCount - 1 };
    for (uint32_t n = 0; n < (job.indices ? 12u : 6u); n++)
    {
        const uint32_t i = picks[n];
        if (i >= job.vertexCount) continue;
        sink.text.clear();
        uint32_t index = i;
        if (job.indices)
        {
            if (job.index32) { uint32_t v; std::memcpy(&v, job.indices + i * 4, 4); index = __builtin_bswap32(v); }
            else { uint16_t v; std::memcpy(&v, job.indices + i * 2, 2); index = __builtin_bswap16(v); }
        }
        interpreter.temp_registers()[0] = float(index + job.firstVertex);
        interpreter.Execute();
        char head[48];
        std::snprintf(head, sizeof head, "vertex %u (index %u):", i, index);
        line((std::string(head) + sink.text).c_str());
    }
    interpreter.SetExportSink(nullptr);
}
