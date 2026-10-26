#pragma once
#include <cstddef>
#include <cstdint>

// Memory export ("memexport"): a vertex shader that writes its results to guest
// memory through the eA/eM export registers, which the title uses as a GPU
// compute step (here, the front end's layout). The draw has no pixels; the
// writes are the effect. The host GPU cannot see guest RAM, so the shader runs
// on the CPU, one invocation per vertex, in Xenia's shader interpreter, and
// the writes land in guest memory before the next draw reads them.
namespace xe_memexport
{
    struct Job
    {
        const uint32_t* registers;      // the register file's values
        const uint32_t* vertexCode;
        size_t vertexWords;
        uint32_t vertexCount;           // indices to run
        uint32_t firstVertex = 0;       // added to each index (VGT_INDX_OFFSET)
        const uint8_t* indices;         // null for auto-index; else big-endian guest indices
        bool index32;
        uint8_t* physicalBase;          // host address of physical address 0
        uint32_t physicalSize;
    };

    // Debugging: runs the vertex shader per vertex and calls `line` with what it
    // exports to the position (62) and interpolator registers.
    void TraceVertices(const Job& job, void (*line)(const char* text));

    // The position (export 62) the shader writes for the job's first vertex,
    // divided by w: what a clear rectangle drawn with the viewport off writes
    // as depth. False when the shader exports no position.
    bool PositionZ(const Job& job, float& z);
    // The position (export 62) of vertex `index` (plus firstVertex), as written.
    bool Position(const Job& job, uint32_t index, float* position);

    // Runs the shader; returns the number of invocations that wrote something.
    uint32_t Execute(const Job& job);
}
