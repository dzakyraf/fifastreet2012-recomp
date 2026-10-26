// The command processor: executes the PM4 stream the title's own D3D9 builds,
// the way the Xenos front end does -- registers, constants, shader loads,
// memory writes, waits, events and draws, in stream order.
#include <ppc_recomp_shared.h>
#include "internal.h"
#include "../pacing_trace.h"
#include "gpu.h"
#include "registers.h"
#include "../counter.h"
#include "../diagnostics.h"
#include "../guest.h"
#include "../image_move.h"
#include "../log.h"
#include "../kernel/kernel.h"
#include "vulkan/renderer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace gpu::detail { std::atomic<uint64_t> g_swapsExecuted{ 0 }; }

namespace
{
    using namespace gpu::detail;

    // Same command processor family as Adreno A2xx.
    enum : uint32_t
    {
        PM4_NOP                 = 0x10,
        PM4_INDIRECT_BUFFER     = 0x3F,
        PM4_INDIRECT_BUFFER_PFD = 0x37,
        PM4_WAIT_FOR_IDLE       = 0x26,
        PM4_WAIT_REG_MEM        = 0x3C,
        PM4_REG_RMW             = 0x21,
        PM4_REG_TO_MEM          = 0x3E,
        PM4_MEM_WRITE           = 0x3D,
        PM4_COND_WRITE          = 0x45,
        PM4_EVENT_WRITE         = 0x46,
        PM4_EVENT_WRITE_SHD     = 0x58,
        PM4_EVENT_WRITE_CFL     = 0x59,
        PM4_EVENT_WRITE_ZPD     = 0x5B,
        PM4_DRAW_INDX           = 0x22,
        PM4_DRAW_INDX_2         = 0x36,
        PM4_SET_CONSTANT        = 0x2D,
        PM4_SET_CONSTANT2       = 0x55,
        PM4_IM_LOAD             = 0x27,
        PM4_IM_LOAD_IMMEDIATE   = 0x2B,
        PM4_INVALIDATE_STATE    = 0x3B,
        PM4_SET_BIN_MASK        = 0x50,
        PM4_SET_BIN_SELECT      = 0x51,
        PM4_CONTEXT_UPDATE      = 0x5E,
        PM4_INTERRUPT           = 0x54,
        PM4_ME_INIT             = 0x48,
        PM4_VIZ_QUERY           = 0x23,
        PM4_LOAD_ALU_CONSTANT   = 0x2F,
        PM4_EVENT_WRITE_EXT     = 0x5A,
        PM4_SET_BIN_MASK_LO     = 0x60,
        PM4_SET_BIN_MASK_HI     = 0x61,
        PM4_SET_BIN_SELECT_LO   = 0x62,
        PM4_SET_BIN_SELECT_HI   = 0x63,
        PM4_XE_SWAP             = 0x64,
    };

    const char* OpcodeName(uint32_t op)
    {
        switch (op)
        {
        case PM4_NOP:                 return "NOP";
        case PM4_INDIRECT_BUFFER:     return "INDIRECT_BUFFER";
        case PM4_INDIRECT_BUFFER_PFD: return "INDIRECT_BUFFER_PFD";
        case PM4_WAIT_FOR_IDLE:       return "WAIT_FOR_IDLE";
        case PM4_WAIT_REG_MEM:        return "WAIT_REG_MEM";
        case PM4_REG_RMW:             return "REG_RMW";
        case PM4_REG_TO_MEM:          return "REG_TO_MEM";
        case PM4_MEM_WRITE:           return "MEM_WRITE";
        case PM4_COND_WRITE:          return "COND_WRITE";
        case PM4_EVENT_WRITE:         return "EVENT_WRITE";
        case PM4_EVENT_WRITE_SHD:     return "EVENT_WRITE_SHD";
        case PM4_EVENT_WRITE_CFL:     return "EVENT_WRITE_CFL";
        case PM4_EVENT_WRITE_ZPD:     return "EVENT_WRITE_ZPD";
        case PM4_DRAW_INDX:           return "DRAW_INDX";
        case PM4_DRAW_INDX_2:         return "DRAW_INDX_2";
        case PM4_SET_CONSTANT:        return "SET_CONSTANT";
        case PM4_SET_CONSTANT2:       return "SET_CONSTANT2";
        case PM4_IM_LOAD:             return "IM_LOAD";
        case PM4_IM_LOAD_IMMEDIATE:   return "IM_LOAD_IMMEDIATE";
        case PM4_INVALIDATE_STATE:    return "INVALIDATE_STATE";
        case PM4_SET_BIN_MASK:        return "SET_BIN_MASK";
        case PM4_SET_BIN_SELECT:      return "SET_BIN_SELECT";
        case PM4_CONTEXT_UPDATE:      return "CONTEXT_UPDATE";
        case PM4_INTERRUPT:           return "INTERRUPT";
        case PM4_ME_INIT:             return "ME_INIT";
        case PM4_VIZ_QUERY:           return "VIZ_QUERY";
        case PM4_LOAD_ALU_CONSTANT:   return "LOAD_ALU_CONSTANT";
        case PM4_EVENT_WRITE_EXT:     return "EVENT_WRITE_EXT";
        case PM4_SET_BIN_MASK_LO:     return "SET_BIN_MASK_LO";
        case PM4_SET_BIN_MASK_HI:     return "SET_BIN_MASK_HI";
        case PM4_SET_BIN_SELECT_LO:   return "SET_BIN_SELECT_LO";
        case PM4_SET_BIN_SELECT_HI:   return "SET_BIN_SELECT_HI";
        case PM4_XE_SWAP:             return "XE_SWAP";
        default:                      return nullptr;
        }
    }

    // The five constant windows SET_CONSTANT and LOAD_ALU_CONSTANT name by type.
    constexpr uint32_t kConstantWindows[5] = {
        gpu::kConstantBaseAlu,  gpu::kConstantBaseFetch, gpu::kConstantBaseBool,
        gpu::kConstantBaseLoop, gpu::kConstantBaseRegisters,
    };

    // Everything here is the GPU thread's; the statistics are read by the report.
    struct State
    {
        gpu::RegisterFile registers;

        // Predication: a type-3 packet with bit 0 set runs only when the bin
        // select and the bin mask share a bit.
        uint64_t binSelect = 0xFFFFFFFFull, binMask = 0xFFFFFFFFull;

        // The shaders the last IM_LOADs set: a draw uses whichever is loaded,
        // as it uses whatever registers are set.
        std::vector<uint32_t> shader[2];
        uint64_t shaderHash[2]{};
        std::unordered_set<uint64_t> shadersSeen;

        uint64_t swapsParsed = 0;         // PM4_XE_SWAP packets met
        // A frame's cost as the consumer sees it, swap to swap: its own CPU
        // time, and the wall clock. A frame over a vertical blank's worth
        // waits for the next one, so the spread says more than the average.
        static constexpr uint32_t kFrameBins = 7;
        uint64_t frameCpu[kFrameBins]{}, frameWall[kFrameBins]{};
        int64_t lastSwapCpu = 0, lastSwapWall = 0;
        // A WAIT_FOR_IDLE with no draw after it: the GPU has finished every draw
        // so far, so whatever the command processor writes or raises now tells
        // the title they are done (vk::renderer::BeforeCompletion).
        bool idleSinceDraw = false;
        uint64_t batchWork = 0;

        uint32_t unknownOpcodes = 0;
        diag::Stat packets, typeCount[4], opcodeCount[128], padding;
        diag::Stat buffers, buffersLostStep, implausibleBuffers, batchesAbandoned;
        diag::Stat shaderLoadsRefused, predicatedRun, predicatedSkipped, registerWritesOutOfRange;
        // WAIT_REG_MEM: seen, found not yet satisfied, time spent waiting, given up on.
        diag::Stat regMemWaits, regMemStalls, regMemWaitMicros, regMemTimeouts;

        // The display's colour table: 256 entries of ten bits per channel, blue
        // in the low bits. The title rewrites the whole ramp as one sweep of
        // entry 0 to entry 255, so the sweep in progress is kept apart from the
        // last one that finished: a present that lands mid-sweep must not see
        // half of each.
        uint32_t displayLutPending[256]{};
        uint32_t displayLut[256]{};
        std::mutex displayLutLock;
        std::atomic<bool> displayLutReady{ false };
    };
    State g;

    std::atomic<uint32_t> g_currentOpcode{ 0 };
    std::atomic<const char*> g_currentOpcodeName{ nullptr };
    std::atomic<uint32_t> g_currentDepth{ 0 };
    std::atomic<uint64_t> g_lastVertexHash{ 0 };
    std::atomic<uint64_t> g_lastPixelHash{ 0 };
    std::atomic<const char*> g_currentDrawStage{ "idle" };

    // Bins in milliseconds: under 8, 12, 16.7, 20, 33.3, 50, and above.
    uint32_t FrameBin(double ms)
    {
        static constexpr double kEdges[] = { 8.0, 12.0, 16.7, 20.0, 33.3, 50.0 };
        uint32_t bin = 0;
        while (bin < 6 && ms >= kEdges[bin]) bin++;
        return bin;
    }

    void NoteFrameCost()
    {
        if constexpr (!diag::kOn) return;
        timespec cpu{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
        const int64_t cpuNow = int64_t(cpu.tv_sec) * 1000000000 + cpu.tv_nsec;
        const int64_t wallNow = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (g.lastSwapWall)
        {
            g.frameCpu[FrameBin(double(cpuNow - g.lastSwapCpu) / 1e6)]++;
            g.frameWall[FrameBin(double(wallNow - g.lastSwapWall) / 1e6)]++;
        }
        g.lastSwapCpu = cpuNow;
        g.lastSwapWall = wallNow;
    }

    // One pass over the ring must finish. A pair of dwords misread as an indirect
    // buffer sends the parse into unrelated memory where everything it finds can
    // be another indirect buffer, so a batch's work is bounded, far above a real
    // frame (~50k dwords).
    constexpr uint64_t kBatchWorkLimit = 8ull << 20;

    struct Source
    {
        uint32_t base = 0;
        uint32_t index = 0;
        uint32_t wrapMask = 0;      // 0 for a linear indirect buffer
        uint32_t remaining = 0;
        // Set when the buffer produced a packet that cannot be one: the rest of
        // it is not read, as Xenia's command processor ends a buffer there.
        bool lostStep = false;

        uint32_t Next()
        {
            const uint32_t i = wrapMask ? (index & wrapMask) : index;
            index++;
            if (remaining) remaining--;
            return *GuestPtr<be32>(base + i * 4);
        }
        void Skip(uint32_t n) { for (uint32_t i = 0; i < n && remaining; i++) Next(); }
        uint32_t AddressOf(uint32_t at) const
        {
            return base + (wrapMask ? (at & wrapMask) : at) * 4;
        }
    };

    void Execute(Source& s, int depth);

    // PES17_WATCH=<hex>[:bytes]: the command processor's own writes into the
    // watched range are reported here; the CPU's go through watchpoint.cpp.
    struct Watch { uint32_t lo = 0, hi = 0; };
    const Watch& Watched()
    {
        static const Watch w = [] {
            Watch v;
            const char* e = diag::Text("PES17_WATCH");
            if (!e) return v;
            char* end = nullptr;
            v.lo = uint32_t(std::strtoul(e, &end, 16));
            const uint32_t bytes = (end && *end == ':') ? uint32_t(std::strtoul(end + 1, nullptr, 0)) : 4;
            v.hi = v.lo + (bytes ? bytes : 4);
            return v;
        }();
        return w;
    }

    std::atomic<uint32_t> g_currentPacketAddress{ 0 };
    void WriteRegister(uint32_t index, uint32_t value)
    {
        if (!g.registers.InRange(index)) { g.registerWritesOutOfRange++; return; }
        if (index >= 0x4000 && index < 0x4800 && (value & 0x7F800000) == 0x7F800000)
        {
            static std::atomic<int> told{ 0 };
            if (told++ < 12)
                LOGW("gpu: a non-finite constant %08X is written to c%u.%c by the %s packet at %08X", value, (index - 0x4000) / 4, "xyzw"[index & 3], g_currentOpcodeName.load() ? g_currentOpcodeName.load() : "?", g_currentPacketAddress.load());
        }
        g.registers.Write(index, value);
        *GuestPtr<be32>(kRegisterAperture + index * 4) = value;
        // Dump only: who sets vertex fetch slot 95 (vf31.2) and texture slot 0.
        if ((index == 0x48BE || index == 0x48BF || index == 0x4800 || index == 0x4801) &&
            vk::renderer::DumpingDraws())
            LOGW("DUMP   reg %04X = %08X by %s at %08X", index, value,
                 g_currentOpcodeName.load() ? g_currentOpcodeName.load() : "type0",
                 g_currentPacketAddress.load());

        // A scratch register whose bit is set in SCRATCH_UMSK is written back to
        // memory at SCRATCH_ADDR + 4n, as Xenia's command processor does. The
        // title writes one and then WAIT_REG_MEMs on that memory to know the GPU
        // has got this far.
        if (index >= kScratchReg0 && index <= kScratchReg0 + 7)
        {
            const uint32_t n = index - kScratchReg0;
            const uint32_t umsk = RegisterOrAperture(kScratchUmsk);
            if (umsk & (1u << n)) WritePhysical(RegisterOrAperture(kScratchAddr) + n * 4, value);
            return;
        }

        // When SCRATCH_UMSK is written, write back any scratch registers whose bit is set.
        if (index == kScratchUmsk)
        {
            const uint32_t scratchAddr = RegisterOrAperture(kScratchAddr);
            if (scratchAddr)
            {
                for (uint32_t n = 0; n < 8; n++)
                {
                    if (value & (1u << n))
                        WritePhysical(scratchAddr + n * 4, g.registers[kScratchReg0 + n]);
                }
            }
            return;
        }

        // When SCRATCH_ADDR is written, write back enabled scratch registers to the new address.
        if (index == kScratchAddr)
        {
            const uint32_t umsk = RegisterOrAperture(kScratchUmsk);
            if (value && umsk)
            {
                for (uint32_t n = 0; n < 8; n++)
                {
                    if (umsk & (1u << n))
                        WritePhysical(value + n * 4, g.registers[kScratchReg0 + n]);
                }
            }
            return;
        }

        // The display's own colour lookup table, which the scanout hardware
        // applies to the front buffer. The title fills it by pointing
        // DC_LUT_RW_INDEX at an entry and writing DC_LUT_30_COLOR, which then
        // steps the index on by itself.
        if (index == gpu::DC_LUT_30_COLOR)
        {
            const uint32_t entry = g.registers[gpu::DC_LUT_RW_INDEX] & 0xFF;
            const uint32_t mask  = g.registers[gpu::DC_LUT_WRITE_EN_MASK] & 7;
            uint32_t& held = g.displayLutPending[entry];
            if (mask & 1) held = (held & ~0x000003FFu) | (value & 0x3FF);
            if (mask & 2) held = (held & ~0x000FFC00u) | (value & 0xFFC00);
            if (mask & 4) held = (held & ~0x3FF00000u) | (value & 0x3FF00000);
            g.registers.Write(gpu::DC_LUT_RW_INDEX, (entry + 1) & 0xFF);
            // Entry 255 ends the sweep: the ramp the title means is complete.
            if (entry == 0xFF)
            {
                std::lock_guard<std::mutex> guard(g.displayLutLock);
                // What the title's ramps look like: the first few, then only when one differs.
                static int told = 0;
                if (told < 6)
                {
                    told++;
                    LOGI("display: colour table written (mask %u): [0] %08X [64] %08X [128] %08X [255] %08X",
                         mask, g.displayLutPending[0], g.displayLutPending[64], g.displayLutPending[128],
                         g.displayLutPending[255]);
                }
                std::memcpy(g.displayLut, g.displayLutPending, sizeof g.displayLut);
                // A ramp that maps every value to black is a fade the title has not finished
                // (FIFA Street writes one once at start-up): the frame is shown untouched until
                // a real one arrives, which keeps what the game drew visible while it works.
                bool allBlack = true;
                for (uint32_t v : g.displayLut) if (v) { allBlack = false; break; }
                g.displayLutReady.store(!allBlack, std::memory_order_release);
            }
        }
    }

    // The occlusion query. The command processor leaves eight counters in a
    // 32-byte record -- { total, z-fail, z-pass, stencil-fail } x two lanes the
    // title sums -- and they are little-endian, because D3D byte-swaps them when
    // it reads them back (`lwbrx`). A query owns a 64-byte slot: the end record
    // at its base and the begin record 32 bytes above, and the answer the title
    // computes is the end's z-pass count less the begin's.
    //
    // D3D marks the record it is waiting for by writing FFFFFEED into both
    // z-pass lanes, and D3DQuery::GetData reports "not finished yet" until they
    // change. The two events bracket the draws being counted, so they open and
    // close a real Vulkan occlusion query; the begin record is written as zero
    // at once, and the end record carries the whole count, written when the
    // GPU has finished counting -- as the console's GPU writes it.
    void WriteSampleCounts(uint32_t address, uint32_t count)
    {
        constexpr uint32_t kRecordBytes = 0x20;
        const uint32_t record = address & ~(kRecordBytes - 1);
        const uint32_t va = record ? kernel::FromPhysical(record) : 0;
        if (!va) return;
        // Little-endian, so the host's own word order is what the title wants.
        auto* w = GuestPtr<uint32_t>(va);
        w[0] = count; w[1] = 0;     // total, lanes A and B
        w[2] = 0;     w[3] = 0;     // z-fail
        w[4] = count; w[5] = 0;     // z-pass, which is the one the title reads
        w[6] = 0;     w[7] = 0;     // stencil-fail
    }

    // The title reads the count as a signed difference; saturate rather than
    // hand it a number that comes back negative.
    void WriteEndRecord(uint32_t address, uint64_t samples)
    {
        WriteSampleCounts(address, uint32_t(std::min<uint64_t>(samples, 0x7FFFFFFFu)));
    }

    void SampleCountEvent(uint32_t address)
    {
        // The begin record is the one with bit 5 of its address set.
        if (address & 0x20)
        {
            vk::renderer::BeginOcclusionQuery();
            WriteSampleCounts(address, 0);
            return;
        }
        uint64_t samples = 0;
        if (vk::renderer::EndOcclusionQuery(address, samples)) WriteEndRecord(address, samples);
    }

    // PES17_DUMP_SHADERS=<dir>: every distinct shader's microcode, big-endian as
    // the title holds it, for tools/xenos_shader.py and translate-shader.
    const char* ShaderDumpDirectory()
    {
        static const char* dir = diag::Text("PES17_DUMP_SHADERS");
        return dir;
    }

    // An IM_LOAD or IM_LOAD_IMMEDIATE: `type` 0 is the vertex shader, 1 the pixel.
    void LoadShader(uint32_t type, std::vector<uint32_t>&& code)
    {
        if (code.empty()) return;
        type &= 1;
        const uint64_t hash = vk::renderer::ShaderHash(code.data(), code.size());
        g.shaderHash[type] = hash;
        if constexpr (!diag::kOn) { g.shader[type] = std::move(code); return; }
        if (g.shadersSeen.insert(hash).second && ShaderDumpDirectory())
        {
            char path[512];
            std::snprintf(path, sizeof path, "%s/%s_%016llx.ucode", ShaderDumpDirectory(),
                          type ? "pixel" : "vertex", (unsigned long long)hash);
            if (std::FILE* f = std::fopen(path, "wb"))
            {
                for (uint32_t v : code)
                {
                    const uint32_t be = __builtin_bswap32(v);
                    std::fwrite(&be, 4, 1, f);
                }
                std::fclose(f);
            }
        }
        g.shader[type] = std::move(code);
    }

    // PES17_TRACE_DRAWS=<n> describes n draw packets in full, after
    // PES17_TRACE_DRAWS_AFTER=<presents>: the opening burst is not representative.
    void TraceDraw(const gpu::DrawInitiator& draw, bool indexed, uint32_t dmaBase,
                   uint32_t dmaSize)
    {
        static const uint64_t limit = diag::Number("PES17_TRACE_DRAWS");
        static const uint64_t after = diag::Number("PES17_TRACE_DRAWS_AFTER");
        static uint64_t traced = 0;
        if (traced >= limit || gpu::FrameCount() < after) return;
        traced++;
        const char* name = gpu::PrimitiveName(draw.PrimitiveType());
        LOGK("draw: %-22s %5u indices, %s, surface %08X colour %08X depth %08X,"
             " program %08X, mode %08X",
             name ? name : "unknown primitive", draw.IndexCount(), indexed ? "indexed" : "auto",
             g.registers[gpu::RB_SURFACE_INFO], g.registers[gpu::RB_COLOR_INFO],
             g.registers[gpu::RB_DEPTH_INFO], g.registers[gpu::SQ_PROGRAM_CNTL],
             g.registers[gpu::RB_MODECONTROL]);
        if (indexed)
        {
            const uint32_t count = dmaSize & 0x3FFFFF;   // indices, not bytes
            LOGK("      indices from %08X, %u of them, %u-bit (%u bytes)",
                 dmaBase, count, draw.Index32() ? 32 : 16, count * (draw.Index32() ? 4 : 2));
        }
    }

    void ExecuteDraw(Source& s, uint32_t count)
    {
        if (count < 1) return;
        g.idleSinceDraw = false;
        const gpu::DrawInitiator draw{ s.Next() };
        WriteRegister(gpu::VGT_DRAW_INITIATOR, draw.value);

        // An auto-indexed draw has the shader compute its own indices.
        uint32_t dmaBase = 0, dmaSize = 0;
        const bool indexed = draw.SourceSelect() == 0;
        if (indexed && count >= 3)
        {
            dmaBase = s.Next();
            dmaSize = s.Next();
        }

        const uint32_t primitive = draw.PrimitiveType();
        const gpu::EdramMode mode = gpu::EdramMode(g.registers[gpu::RB_MODECONTROL] & 0x7);
        CensusDraw(g.registers, primitive, draw.IndexCount(), indexed, mode);
        if (imagepool::CopiesWaiting())
            imagepool::AtDraw(g.registers[gpu::kConstantBaseAlu], g.shaderHash[0]);
        if (DumpingTextures()) DumpBoundTextures(g.registers);

        if (Rendering())
        {
            if (mode == gpu::EdramMode::Copy)
                vk::renderer::Resolve(g.registers);
            else
            {
                vk::renderer::DrawCall call;
                call.primitive = primitive;
                call.indexCount = draw.IndexCount();
                call.indexed = indexed;
                call.index32 = draw.Index32();
                // Physical, as every address the GPU is handed is.
                call.indexAddress = indexed ? kernel::FromPhysical(dmaBase) : 0;
                call.vertexCode = g.shader[0].data();
                call.vertexWords = g.shader[0].size();
                call.vertexHash = g.shaderHash[0];
                call.pixelCode = g.shader[1].data();
                call.pixelWords = g.shader[1].size();
                call.pixelHash = g.shaderHash[1];
                g_lastVertexHash.store(call.vertexHash, std::memory_order_relaxed);
                g_lastPixelHash.store(call.pixelHash, std::memory_order_relaxed);
                vk::renderer::Draw(g.registers, call);
            }
        }
        TraceDraw(draw, indexed, dmaBase, dmaSize);
    }

    void ExecuteIndirect(uint32_t physicalAddress, uint32_t sizeDwords, int depth)
    {
        if (depth > 4 || !sizeDwords) return;
        // Every buffer the title issues is 32-byte aligned and a few thousand dwords
        // long. A pair of dwords that only reads like an address and a length almost
        // never satisfies both, and running one sends the parse into unrelated memory.
        if ((physicalAddress & 0x1F) || sizeDwords > (16u << 10))
        {
            g.implausibleBuffers++;
            return;
        }
        const uint32_t va = kernel::FromPhysical(physicalAddress);
        if (!va) return;

        Source ib{ va, 0, 0, sizeDwords };
        Execute(ib, depth + 1);
        g.buffers++;
        if (ib.lostStep) g.buffersLostStep++;
    }

    // WAIT_REG_MEM: the command processor stalls until a register or a dword of
    // memory matches. D3D's flip handshake is built on it -- wait for the
    // scratch write-back of the handler it names, raise the interrupt, wait for
    // the handler to clear the request. Waited for as Xenia waits, but not for
    // ever.
    void WaitRegMem(uint32_t info, uint32_t address, uint32_t reference, uint32_t mask)
    {
        constexpr uint32_t kCoherStatusHost = 0x0A31;
        const bool memory = (info & 0x10) != 0;
        const volatile uint32_t* word = nullptr;
        const volatile uint32_t* cachedWord = nullptr;
        if (memory)
        {
            const uint32_t phys = address & ~3u;
            const uint32_t va = kernel::FromPhysical(phys);
            if (!va) return;
            word = reinterpret_cast<const volatile uint32_t*>(GuestPtr<uint8_t>(va));
            const uint32_t cachedVa = 0x80000000u + phys;
            if (cachedVa >= 0x82000000u && cachedVa < guest::kHeapEnd && cachedVa != va)
            {
                cachedWord = reinterpret_cast<const volatile uint32_t*>(GuestPtr<uint8_t>(cachedVa));
            }
        }
        else if (!g.registers.InRange(address)) return;

        const auto readWord = [&](const volatile uint32_t* ptr) -> uint32_t {
            if (!memory)
            {
                if (address == kCoherStatusHost)
                    g.registers.Write(address, g.registers[address] & ~0x80000000u);
                return g.registers[address];
            }
            if (!ptr) return 0;
            const uint32_t raw = *ptr;
            switch (address & 3)
            {
            case 1:  return ((raw & 0x00FF00FFu) << 8) | ((raw >> 8) & 0x00FF00FFu);
            case 2:  return __builtin_bswap32(raw);
            case 3:  return (raw << 16) | (raw >> 16);
            default: return raw;
            }
        };

        const auto current = [&]() -> uint32_t {
            return readWord(word);
        };

        const auto matches = [&](uint32_t value) {
            value &= mask;
            switch (info & 7)
            {
            case 1:  return value <  reference;
            case 2:  return value <= reference;
            case 3:  return value == reference;
            case 4:  return value != reference;
            case 5:  return value >= reference;
            case 6:  return value >  reference;
            case 7:  return true;
            default: return false;
            }
        };

        // If either the physical or cached view matches, we are done
        const auto checkMatches = [&]() -> bool {
            if (matches(current())) return true;
            if (cachedWord && matches(readWord(cachedWord))) return true;
            return false;
        };

        // If waiting for a known guest code callback (e.g. sub_820C2CB0 @ 0x820C2CB0):
        // If the location holds heap poison (0x0BADF00D) or is uninitialized, write reference
        // immediately to prevent a 2000 ms GPU pipeline stall and prevent the subsequent
        // PM4_INTERRUPT from indirectly calling poison.
        if (memory && (reference & 0xFF000000) == 0x82000000 && (info & 7) == 3)
        {
            if (current() == 0x0BADF00D || (cachedWord && readWord(cachedWord) == 0x0BADF00D) || !checkMatches())
            {
                WritePhysical(address, reference);
                g.regMemWaits++;
                return;
            }
        }

        g.regMemWaits++;
        if (checkMatches()) return;
        g.regMemStalls++;
        pacing::Note(pacing::kWaitBegin, address);
        struct WaitEnd { ~WaitEnd() { pacing::Note(pacing::kWaitEnd); } } waitEnd;

        const auto began = std::chrono::steady_clock::now();
        const auto waited = [&] { return std::chrono::steady_clock::now() - began; };
        bool matched = false;
        // On Android, never stall the GPU command processor for 2 seconds (which causes 0 FPS / deadlock)
        constexpr auto kTimeout =
#ifdef PES17_ANDROID
            std::chrono::milliseconds(50);
#else
            std::chrono::milliseconds(100);
#endif
        while (Running())
        {
            DeliverOcclusionQueries();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            if (checkMatches()) { matched = true; break; }
            if (waited() > kTimeout) break;
        }
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(waited()).count();
        g.regMemWaitMicros += uint64_t(micros);
        // The long ones, which is where a frame goes: what was waited for, and how it ended.
        static std::atomic<uint32_t> slow{ 0 };
        if (micros > 5000 && slow++ < 40)
            LOGW("gpu: WAIT_REG_MEM %s %08X & %08X (function %u) for %08X took %lld us (%s); it holds %08X",
                 memory ? "memory" : "register", address, mask, info & 7, reference, (long long)micros,
                 matched ? "matched" : "gave up", current());
        if (matched) return;
        g.regMemTimeouts++;

        // On timeout, if waiting on equality with a guest function pointer, repair it
        // with the reference value so that subsequent interrupts or guest calls do not crash.
        if (memory && (reference & 0xFF000000) == 0x82000000 && (info & 7) == 3)
        {
            WritePhysical(address, reference);
        }

        static uint32_t told = 0;
        if (told++ < 8)
            LOGW("gpu: gave up after %lld ms waiting for %s %08X & %08X to compare"
                 " (function %u) with %08X; it holds %08X",
                 (long long)std::chrono::duration_cast<std::chrono::milliseconds>(kTimeout).count(),
                 memory ? "memory" : "register", address, mask, info & 7, reference, current());
    }

    // A type-3 packet. `count` is its payload in dwords.
    void ExecuteType3(Source& s, uint32_t header, uint32_t count, int depth)
    {
        const uint32_t opcode = (header >> 8) & 0x7F;
        g.opcodeCount[opcode]++;
        g_currentOpcode.store(opcode, std::memory_order_relaxed);
        g_currentOpcodeName.store(OpcodeName(opcode), std::memory_order_relaxed);
        g_currentDepth.store(depth, std::memory_order_relaxed);

        if (!OpcodeName(opcode))
        {
            if (g.unknownOpcodes++ < 32)
                LOGW("pm4: unknown type-3 opcode 0x%02X, %u dwords (header %08X) at %08X (depth %d)",
                     opcode, count, header, s.AddressOf(s.index - 1), depth);
            s.Skip(count);
            return;
        }

        // A predicated packet (bit 0) runs only when the bin select and the bin
        // mask share a bit, and a predicated swap never does -- as Xenia has it.
        if (header & 1)
        {
            if (!(g.binSelect & g.binMask) || opcode == PM4_XE_SWAP)
            {
                if (vk::renderer::DumpingDraws())
                    LOGW("DUMP   pm4 %s n%u at %08X depth %d: predicated, skipped (select %016llX mask %016llX)",
                         OpcodeName(opcode), count, s.AddressOf(s.index - 1), depth,
                         (unsigned long long)g.binSelect, (unsigned long long)g.binMask);
                g.predicatedSkipped++;
                s.Skip(count);
                return;
            }
            g.predicatedRun++;
        }

        if (vk::renderer::DumpingDraws())
        {
            char words[100] = ""; int at = 0;
            for (uint32_t i = 0; i < count && i < 6; i++)
                at += std::snprintf(words + at, sizeof words - at, " %08X",
                                    uint32_t(*GuestPtr<be32>(s.AddressOf(s.index + i))));
            LOGW("DUMP   pm4 %s n%u at %08X depth %d:%s", OpcodeName(opcode), count,
                 s.AddressOf(s.index - 1), depth, words);
        }

        // What a handler consumed is measured rather than declared: one that read
        // past its own packet would shift everything after it.
        const uint32_t before = s.remaining;
        switch (opcode)
        {
        case PM4_INDIRECT_BUFFER:
        case PM4_INDIRECT_BUFFER_PFD:
        {
            if (count < 2) break;
            const uint32_t address = s.Next();
            const uint32_t size    = s.Next() & 0xFFFFF;
            ExecuteIndirect(address, size, depth);
            break;
        }

        case PM4_MEM_WRITE:
        {
            // { address, value... } -- the address' low bits are flags.
            const uint32_t address = s.Next();
            for (uint32_t i = 1; i < count; i++)
                WritePhysical(address + (i - 1) * 4, s.Next());
            break;
        }

        case PM4_EVENT_WRITE_SHD:
        {
            // { initiator, address, value }. Bit 31 of the initiator asks for the
            // GPU's clock instead of the value, which is the timebase's, as
            // Xenia writes it; MW2 has not been seen to ask.
            if (count < 3) break;
            const uint32_t initiator = s.Next();
            const uint32_t address   = s.Next();
            const uint32_t value     = s.Next();
            // An end-of-pipe event: its write lands once the draws before it
            // have run -- which is how D3D's fence, the arena's progress
            // report, tells the title they are done.
            vk::renderer::BeforeCompletion();
            if (ArenaProgress(address, value)) break;
            WritePhysical(address, (initiator >> 31) ? uint32_t(PPC_QueryTimebase()) : value);
            break;
        }

        case PM4_EVENT_WRITE_CFL:
        {
            // { initiator, address, value }, end of pipe as EVENT_WRITE_SHD.
            if (count < 3) break;
            vk::renderer::BeforeCompletion();
            s.Next();
            const uint32_t address = s.Next();
            WritePhysical(address, s.Next());
            break;
        }

        case PM4_EVENT_WRITE_EXT:
        {
            // { initiator, address }: the screen extents of the draws since the last one, six
            // 16-bit values (x, y and z, each min then max; x and y in units of 8 pixels). As
            // Xenia, the whole screen: FIFA Street brackets each object it might cull with one
            // and draws only those whose extent is not empty -- with nothing written there, the
            // players, the ball and the stadium were never drawn and a match was a field of grass.
            if (count < 2) break;
            s.Next();
            const uint32_t address = s.Next() & ~3u;
            constexpr uint32_t kMaxExtent = 8192 >> 3;
            WritePhysical(address, kMaxExtent);           // min x 0, max x
            WritePhysical(address + 4, kMaxExtent);       // min y 0, max y
            WritePhysical(address + 8, 1);                // min z 0, max z 1
            break;
        }

        case PM4_EVENT_WRITE_ZPD:
            // Unlike the other event writes this one carries only the event
            // initiator: the record's address is a register.
            if (count < 1) break;
            s.Next();
            SampleCountEvent(g.registers[gpu::RB_SAMPLE_COUNT_ADDR]);
            break;

        case PM4_XE_SWAP:
            // The swap packet VdSwap writes into the present segment: the end of
            // a frame as the command processor meets it.
            // Its dwords: a signature, the front buffer, its width and height.
            if (count >= 2)
            {
                s.Next();
                const uint32_t frontBuffer = s.Next();
                if (Rendering()) vk::renderer::Swap(frontBuffer);
            }
            g.swapsParsed++;
            gpu::detail::g_swapsExecuted.store(g.swapsParsed, std::memory_order_release);
            pacing::Note(pacing::kSwap);
            NoteFrameCost();
            CensusSwap();
            if (imagepool::CopiesWaiting()) imagepool::AtFrameEnd(g.swapsParsed);
            break;

        case PM4_SET_BIN_MASK_LO:
        case PM4_SET_BIN_MASK_HI:
        case PM4_SET_BIN_SELECT_LO:
        case PM4_SET_BIN_SELECT_HI:
        {
            // Half of the 64-bit bin mask or select that predicated packets are
            // tested against.
            if (count < 1) break;
            const uint64_t value = s.Next();
            uint64_t& target = (opcode == PM4_SET_BIN_MASK_LO || opcode == PM4_SET_BIN_MASK_HI)
                             ? g.binMask : g.binSelect;
            if (opcode == PM4_SET_BIN_MASK_LO || opcode == PM4_SET_BIN_SELECT_LO)
                target = (target & 0xFFFFFFFF00000000ull) | value;
            else
                target = (target & 0xFFFFFFFFull) | (value << 32);
            break;
        }

        case PM4_SET_BIN_MASK:
        case PM4_SET_BIN_SELECT:
        {
            // { high, low }
            if (count < 2) break;
            const uint64_t high = s.Next();
            const uint64_t low  = s.Next();
            (opcode == PM4_SET_BIN_MASK ? g.binMask : g.binSelect) = (high << 32) | low;
            break;
        }

        case PM4_IM_LOAD_IMMEDIATE:
        {
            // { type, start<<16 | size, microcode... }
            if (count < 2) break;
            const uint32_t type      = s.Next();
            const uint32_t startSize = s.Next();
            std::vector<uint32_t> code;
            code.reserve(count - 2);
            for (uint32_t i = 2; i < count; i++) code.push_back(s.Next());
            // The packet states its length twice, and a real one agrees with itself.
            if ((startSize & 0xFFFF) != count - 2) { g.shaderLoadsRefused++; break; }
            LoadShader(type, std::move(code));
            break;
        }

        case PM4_IM_LOAD:
        {
            // { address|type, start<<16 | size }: the microcode is in memory; the
            // address' low two bits select vertex or pixel.
            if (count < 2) break;
            const uint32_t addressType = s.Next();
            const uint32_t startSize   = s.Next();
            const uint32_t va = kernel::FromPhysical(addressType & ~3u);
            const uint32_t sizeDwords = startSize & 0xFFFF;
            if (!va || !sizeDwords || sizeDwords > (64u << 10)) break;
            std::vector<uint32_t> code(sizeDwords);
            for (uint32_t i = 0; i < sizeDwords; i++) code[i] = *GuestPtr<be32>(va + i * 4);
            static const bool traced = diag::Flag("PES17_TRACE_SHADER_LOADS");
            static std::unordered_set<uint32_t> told;
            if (traced && told.size() < 2000 && told.insert(va).second)
            {
                LOGI("shader load: %s %u dwords at %08X, %016llX",
                     (addressType & 3u) ? "pixel" : "vertex", sizeDwords, va,
                     (unsigned long long)vk::renderer::ShaderHash(code.data(), code.size()));
            }
            LoadShader(addressType & 3u, std::move(code));
            break;
        }

        case PM4_WAIT_FOR_IDLE:
            // The command processor stops until the GPU has finished everything
            // before. Nothing here runs ahead of the parse to wait for.
            g.idleSinceDraw = true;
            break;

        case PM4_INTERRUPT:
            // { cpu mask }: a command-processor interrupt, once per processor named.
            if (count < 1) break;
            if (g.idleSinceDraw) vk::renderer::BeforeCompletion();
            // The handler is the title's code, and may read what the GPU wrote.
            vk::renderer::PublishWriteBacks();
            RaiseCommandProcessorInterrupt(s.Next());
            break;

        case PM4_SET_CONSTANT:
        {
            // { offset and type, values... }. The type names a constant window; the
            // offset is an index within it.
            if (count < 1) break;
            const uint32_t offsetType = s.Next();
            const uint32_t index = offsetType & 0x7FF;
            const uint32_t kind  = (offsetType >> 16) & 0xFF;
            if (kind >= 5) break;
            for (uint32_t i = 1; i < count; i++)
            {
                const uint32_t value = s.Next();
                if (kind == 0 && (value & 0x7F800000) == 0x7F800000)
                {
                    static std::atomic<int> told{ 0 };
                    if (told++ < 12)
                        LOGW("gpu: SET_CONSTANT writes a non-finite constant %08X to c%u.%c (packet of %u dwords from index %u)",
                             value, (index + i - 1) / 4, "xyzw"[(index + i - 1) & 3], count - 1, index);
                }
                WriteRegister(kConstantWindows[kind] + index + (i - 1), value);
            }
            break;
        }

        case PM4_LOAD_ALU_CONSTANT:
        {
            // { physical address, offset and type, size in dwords }: the same five
            // windows SET_CONSTANT names, filled from memory rather than from the
            // stream, as big-endian dwords.
            if (count < 3) break;
            const uint32_t address    = s.Next() & 0x3FFFFFFF;
            const uint32_t offsetType = s.Next();
            const uint32_t sizeDwords = s.Next() & 0xFFF;
            const uint32_t index = offsetType & 0x7FF;
            const uint32_t kind  = (offsetType >> 16) & 0xFF;
            const uint32_t va = kernel::FromPhysical(address);
            if (kind >= 5 || !va || !sizeDwords) break;
            for (uint32_t i = 0; i < sizeDwords; i++)
            {
                const uint32_t value = *GuestPtr<be32>(va + i * 4);
                if (kind == 0 && (value & 0x7F800000) == 0x7F800000)
                {
                    static std::atomic<int> told{ 0 };
                    if (told++ < 12)
                        LOGW("gpu: LOAD_ALU_CONSTANT loads a non-finite constant %08X to c%u.%c from physical %08X +%u",
                             value, (index + i) / 4, "xyzw"[(index + i) & 3], address, i * 4);
                }
                WriteRegister(kConstantWindows[kind] + index + i, value);
            }
            break;
        }

        case PM4_SET_CONSTANT2:
        {
            // The offset is already an absolute register.
            if (count < 1) break;
            const uint32_t index = s.Next() & 0xFFFF;
            for (uint32_t i = 1; i < count; i++) WriteRegister(index + (i - 1), s.Next());
            break;
        }

        case PM4_DRAW_INDX:
            // { viz query token, draw initiator, [dma base, dma size] }
            if (count < 2) break;
            s.Next();
            ExecuteDraw(s, count - 1);
            break;

        case PM4_DRAW_INDX_2:
            // The same without the viz query token: where MW2's draws come from.
            ExecuteDraw(s, count);
            break;

        case PM4_REG_TO_MEM:
        {
            // { register, address }: copy one register to memory.
            if (count < 2) break;
            const uint32_t reg = s.Next() & 0xFFFF;
            WritePhysical(s.Next(), g.registers[reg]);
            break;
        }

        case PM4_WAIT_REG_MEM:
        {
            // { info, address, reference, mask, poll interval }
            if (count < 5) break;
            const uint32_t info      = s.Next();
            const uint32_t address   = s.Next();
            const uint32_t reference = s.Next();
            const uint32_t mask      = s.Next();
            s.Next();
            WaitRegMem(info, address, reference, mask);
            break;
        }

        case PM4_COND_WRITE:
        {
            // { mode, poll address, reference, mask, write address, value }.
            // Nothing here can fail a poll, so the write is taken.
            if (count < 6) break;
            const uint32_t mode = s.Next();
            s.Skip(3);
            const uint32_t writeAddress = s.Next();
            const uint32_t value        = s.Next();
            if (!(mode & 0x100))    // memory, not register
                WritePhysical(writeAddress, value);
            break;
        }

        default:
            break;
        }
        const uint32_t used = before - s.remaining;
        if (used > count)
        {
            LOGE("pm4: opcode 0x%02X (%s) OVER-CONSUMED dwords: used %u > count %u",
                 opcode, OpcodeName(opcode) ? OpcodeName(opcode) : "unknown", used, count);
        }
        else if (used < count)
        {
            s.Skip(count - used);
        }
    }

    struct PacketTrace
    {
        uint32_t address;
        uint32_t header;
        uint8_t  type;
        uint8_t  opcode;
        uint16_t depth;
        uint32_t payload;
        uint32_t remaining;
    };

    constexpr size_t kTraceHistorySize = 64;
    PacketTrace g_packetHistory[kTraceHistorySize]{};
    size_t g_packetHistoryHead = 0;

    void RecordPacket(uint32_t addr, uint32_t header, uint32_t type, uint32_t opcode, uint32_t payload, uint32_t remaining, int depth)
    {
        auto& t = g_packetHistory[g_packetHistoryHead % kTraceHistorySize];
        t.address = addr;
        t.header = header;
        t.type = static_cast<uint8_t>(type);
        t.opcode = static_cast<uint8_t>(opcode);
        t.depth = static_cast<uint16_t>(depth);
        t.payload = payload;
        t.remaining = remaining;
        g_packetHistoryHead++;
    }

    void DumpPacketHistory(int count = 16)
    {
        LOGW("pm4: dumping last %d executed packets prior to desync:", count);
        const size_t total = g_packetHistoryHead;
        const size_t start = (total > static_cast<size_t>(count)) ? (total - count) : 0;
        for (size_t i = start; i < total; i++)
        {
            const auto& t = g_packetHistory[i % kTraceHistorySize];
            const char* opName = (t.type == 3) ? OpcodeName(t.opcode) : nullptr;
            LOGW("  [%zu] at %08X: header %08X (type %u%s%s, payload %u, rem %u, depth %d)",
                 i, t.address, t.header, t.type,
                 opName ? " " : "", opName ? opName : "",
                 t.payload, t.remaining, t.depth);
        }
    }

    void DumpMemoryWindow(uint32_t center)
    {
        LOGW("pm4: dumping memory around %08X:", center);
        const uint32_t start = center >= 64 ? (center - 64) : 0;
        const uint32_t end   = center + 64;
        for (uint32_t a = start; a < end; a += 16)
        {
            LOGW("  %08X: %08X %08X %08X %08X %s",
                 a,
                 uint32_t(*GuestPtr<be32>(a)),
                 uint32_t(*GuestPtr<be32>(a + 4)),
                 uint32_t(*GuestPtr<be32>(a + 8)),
                 uint32_t(*GuestPtr<be32>(a + 12)),
                 (a <= center && center < a + 16) ? "<-- [packetAddr]" : "");
        }
    }

    bool IsValidPacketHeader(uint32_t header, uint32_t remaining)
    {
        if (header == 0) return true;
        const uint32_t type = header >> 30;
        if (type == 2) return true;
        if (type == 1) return remaining >= 2;
        if (type == 0)
        {
            const uint32_t payload = ((header >> 16) & 0x3FFF) + 1;
            const uint32_t reg = header & 0x7FFF;
            return payload <= remaining && reg < 0x6000;
        }
        if (type == 3)
        {
            const uint32_t payload = ((header >> 16) & 0x3FFF) + 1;
            const uint32_t opcode = (header >> 8) & 0x7F;
            return payload <= remaining && OpcodeName(opcode) != nullptr;
        }
        return false;
    }

    void Execute(Source& s, int depth)
    {
        while (s.remaining)
        {
            if (++g.batchWork > kBatchWorkLimit)
            {
                if (depth == 0) g.batchesAbandoned++;
                return;
            }
            const uint32_t packetAddr = s.AddressOf(s.index);
            const uint32_t header = s.Next();
            // A zero dword is filler, not a type-0 packet: reading it as one
            // swallows the dword after it.
            if (header == 0)
            {
                g.padding++;
                static uint32_t zeroShown = 0;
                if (zeroShown++ < 32)
                    LOGW("pm4: zero dword (padding) at %08X, remaining %u, depth %d", packetAddr, s.remaining, depth);
                continue;
            }

            const uint32_t type = header >> 30;
            const uint32_t payload = (type == 1) ? 2
                                   : (type == 2) ? 0
                                   : ((header >> 16) & 0x3FFF) + 1;
            const uint32_t opcode = (type == 3) ? ((header >> 8) & 0x7F) : 0xFF;
            g_currentPacketAddress.store(packetAddr, std::memory_order_relaxed);
            RecordPacket(packetAddr, header, type, opcode, payload, s.remaining, depth);
            g_currentOpcode.store((type << 8) | (type == 3 ? opcode : 0), std::memory_order_relaxed);
            g_currentOpcodeName.store(type == 3 ? OpcodeName(opcode) : (type == 0 ? "TYPE_0" : type == 1 ? "TYPE_1" : "TYPE_2"), std::memory_order_relaxed);
            g_currentDepth.store(depth, std::memory_order_relaxed);

            if (payload > s.remaining || (type == 3 && !OpcodeName(opcode)))
            {
                static uint32_t shown = 0;
                if (shown++ < 8)
                {
                    LOGW("pm4: packet at %08X (header %08X, type %u, payload %u words, remaining %u words, depth %d) invalid; seeking next valid packet",
                         packetAddr, header, type, payload, s.remaining, depth);
                    DumpPacketHistory(16);
                    DumpMemoryWindow(packetAddr);
                }

                bool recovered = false;
                while (s.remaining > 0)
                {
                    const uint32_t candidateAddr = s.AddressOf(s.index);
                    const uint32_t candidate = *GuestPtr<be32>(candidateAddr);
                    if (candidate == 0)
                    {
                        s.Next();
                        continue;
                    }
                    if (IsValidPacketHeader(candidate, s.remaining - 1))
                    {
                        if (shown <= 8)
                        {
                            LOGI("pm4: resynchronized at %08X with header %08X (rem %u)",
                                 candidateAddr, candidate, s.remaining);
                        }
                        recovered = true;
                        break;
                    }
                    s.Next();
                }

                if (!recovered)
                {
                    s.lostStep = true;
                    // Scan remaining buffer words to salvage any trailing PM4_EVENT_WRITE_SHD packets
                    // so guest D3D9 arena progress fences are never dropped, avoiding producer deadlocks.
                    while (s.remaining >= 4)
                    {
                        const uint32_t w = s.Next();
                        if ((w >> 30) == 3 && ((w >> 8) & 0x7F) == PM4_EVENT_WRITE_SHD)
                        {
                            const uint32_t count = ((w >> 16) & 0x3FFF) + 1;
                            if (count >= 3 && s.remaining >= 3)
                            {
                                const uint32_t initiator = s.Next();
                                const uint32_t address   = s.Next();
                                const uint32_t value     = s.Next();
                                vk::renderer::BeforeCompletion();
                                if (!ArenaProgress(address, value))
                                    WritePhysical(address, (initiator >> 31) ? uint32_t(PPC_QueryTimebase()) : value);
                                LOGI("pm4: salvaged trailing EVENT_WRITE_SHD at %08X (addr %08X, val %08X)",
                                     s.AddressOf(s.index - 4), address, value);
                                if (count > 3) s.Skip(count - 3);
                            }
                        }
                    }
                    return;
                }
                continue;
            }
            g.typeCount[type]++;
            g.packets++;

            switch (type)
            {
            case 0:
            {
                // A run of registers from a base index, or the same register
                // `count` times when ONE_REG_WRITE is set -- how a FIFO register
                // is fed.
                const uint32_t base = header & 0x7FFF;
                const bool one = (header & 0x8000) != 0;
                for (uint32_t i = 0; i < payload; i++)
                    WriteRegister(one ? base : base + i, s.Next());
                break;
            }
            case 1:
            {
                // Two register indices packed into the header.
                const uint32_t first  = header & 0x7FF;
                const uint32_t second = (header >> 11) & 0x7FF;
                WriteRegister(first, s.Next());
                WriteRegister(second, s.Next());
                break;
            }
            case 2:
                break;
            case 3:
                ExecuteType3(s, header, payload, depth);
                break;
            }
        }
    }
}

void gpu::detail::DeliverOcclusionQueries()
{
    vk::renderer::CollectOcclusionQueries(WriteEndRecord);
}

void gpu::detail::ExecuteRing(uint32_t base, uint32_t readIndex, uint32_t mask, uint32_t dwords)
{
    g.batchWork = 0;
    Source s{ base, readIndex, mask, dwords };
    Execute(s, 0);
}

uint32_t gpu::detail::RegisterOrAperture(uint32_t index)
{
    if (const uint32_t v = g.registers[index]) return v;
    return *GuestPtr<be32>(kRegisterAperture + index * 4);
}

bool gpu::detail::Rendering()
{
    static const bool on = !diag::Text("PES17_RENDER") || diag::Flag("PES17_RENDER");
    return on;
}

void gpu::detail::WritePhysical(uint32_t physicalAddress, uint32_t value, bool publish)
{
    const uint32_t phys = physicalAddress & ~3u;
    const uint32_t va = kernel::FromPhysical(phys);
    if (!va) return;
    if (publish) vk::renderer::PublishWriteBacks();
    // The title reads this as soon as it lands. Without a wait for idle, the
    // command processor writes as it meets the packet, ahead of the draws
    // before it, and the title can conclude nothing about them from it.
    if (g.idleSinceDraw) vk::renderer::BeforeCompletion();
    if constexpr (diag::kOn)
        if (const Watch& w = Watched(); va >= w.lo && va < w.hi)
            LOGK("watch: command processor writes %08X = %08X (physical %08X, was %08X)",
             va, value, physicalAddress, uint32_t(*GuestPtr<be32>(va)));
    *GuestPtr<be32>(va) = value;

    // Mirror write to cached heap if physical address corresponds to heap range
    const uint32_t cached = 0x80000000u + phys;
    if (cached >= 0x82000000u && cached < guest::kHeapEnd && cached != va)
    {
        *GuestPtr<be32>(cached) = value;
    }
}

bool gpu::DisplayColourTable(uint32_t out[256])
{
    // Only a ramp the title finished writing. Before the first one is complete
    // the frame is presented untouched.
    if (!g.displayLutReady.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> guard(g.displayLutLock);
    std::memcpy(out, g.displayLut, sizeof g.displayLut);
    return true;
}

gpu::detail::OpcodeStatus gpu::detail::CurrentOpcodeStatus()
{
    OpcodeStatus status{};
    status.opcode = g_currentOpcode.load(std::memory_order_relaxed);
    status.name = g_currentOpcodeName.load(std::memory_order_relaxed);
    status.depth = g_currentDepth.load(std::memory_order_relaxed);
    status.batchWork = g.batchWork;
    status.lastVertexHash = g_lastVertexHash.load(std::memory_order_relaxed);
    status.lastPixelHash = g_lastPixelHash.load(std::memory_order_relaxed);
    return status;
}

const char* gpu::detail::CurrentDrawStage()
{
    return g_currentDrawStage.load(std::memory_order_relaxed);
}

void gpu::detail::SetCurrentDrawStage(const char* stage)
{
    g_currentDrawStage.store(stage, std::memory_order_relaxed);
}


void gpu::detail::ReportCommandProcessor()
{
    if constexpr (!diag::kOn) return;
    LOGI("gpu: %llu packets; %llu of %llu indirect buffers lost step, %llu refused as"
         " implausible, %llu ring batches abandoned; %llu padding dwords, %llu unknown opcodes",
         (unsigned long long)g.packets.load(), (unsigned long long)g.buffersLostStep.load(),
         (unsigned long long)g.buffers.load(), (unsigned long long)g.implausibleBuffers.load(),
         (unsigned long long)g.batchesAbandoned.load(), (unsigned long long)g.padding.load(),
         (unsigned long long)g.unknownOpcodes);
    LOGI("gpu: %zu distinct shaders loaded, %llu loads refused as inconsistent",
         g.shadersSeen.size(), (unsigned long long)g.shaderLoadsRefused.load());
    LOGI("gpu: predicated packets: %llu run, %llu skipped", (unsigned long long)g.predicatedRun.load(),
         (unsigned long long)g.predicatedSkipped.load());
    if (g.registerWritesOutOfRange)
        LOGI("gpu: %llu register writes out of range",
             (unsigned long long)g.registerWritesOutOfRange.load());
    if (g.regMemWaits)
        LOGI("gpu: %llu WAIT_REG_MEM packets, %llu found unsatisfied (%.1f ms waited in all),"
             " %llu given up on after 500 ms",
             (unsigned long long)g.regMemWaits.load(), (unsigned long long)g.regMemStalls.load(),
             double(g.regMemWaitMicros.load()) / 1000.0, (unsigned long long)g.regMemTimeouts.load());
    LOGI("gpu: %llu swap packets", (unsigned long long)g.swapsParsed);
    const auto bins = [](const uint64_t* n) {
        char line[160];
        std::snprintf(line, sizeof line,
                      "<8 ms %llu, 8-12 %llu, 12-16.7 %llu, 16.7-20 %llu, 20-33 %llu, 33-50 %llu,"
                      " 50+ %llu", (unsigned long long)n[0], (unsigned long long)n[1],
                      (unsigned long long)n[2], (unsigned long long)n[3], (unsigned long long)n[4],
                      (unsigned long long)n[5], (unsigned long long)n[6]);
        return std::string(line);
    };
    LOGI("gpu: frames by the consumer's CPU time, swap to swap: %s", bins(g.frameCpu).c_str());
    LOGI("gpu: frames by wall time, swap to swap: %s", bins(g.frameWall).c_str());
    for (uint32_t t = 0; t < 4; t++)
        if (g.typeCount[t]) LOGI("  type %u: %llu", t, (unsigned long long)g.typeCount[t].load());
    for (uint32_t op = 0; op < 128; op++)
        if (const uint64_t n = g.opcodeCount[op].load())
        {
            const char* name = OpcodeName(op);
            LOGI("  0x%02X %-22s %llu", op, name ? name : "(unknown)", (unsigned long long)n);
        }
}
