#pragma once
#include <cstdint>

namespace gpu { struct TextureData; }

// Textures already converted for the host, kept on disk between runs.
//
// Where the GPU samples no BC, a BC texture is converted on the CPU every time it
// is uploaded -- to ASTC 4x4 under PES17_BC_ASTC. The game reads the same few
// thousand textures out of its archives every session, so the conversion is the
// same work every time. The store keeps its output, keyed by a hash of the guest
// blocks it was made from (after untiling, before conversion) together with the
// texture's shape and format: the next time those blocks are uploaded, from
// wherever the title put them, the output is read back instead of made again.
//
// One file, PES17_TEXTURE_STORE (the app sets it to texture_store.bin in its
// external folder). Records are appended by a thread of the store's own as
// conversions happen, so a session fills it for the next; a record cut short by
// a crash is dropped when the file is next opened. Only the index -- a few dozen
// bytes a texture -- is held in memory; the bytes are read from the file, which
// the store asks the kernel to read ahead at start. The header names the
// encoder's version: a file made by another one is started again.
namespace vk::texstore
{
    // Opens the store named by PES17_TEXTURE_STORE on a thread of its own, the
    // first time it is called; true once it is open and its index read.
    bool Open();

    // The converted form of `source` (a BC texture as gpu::ReadTexture returns it),
    // if the store has it: into `out`, with `vkFormat` the format it is in.
    // `hash` is the source's, for the Save that follows a miss.
    bool Load(const gpu::TextureData& source, uint32_t xenosFormat, gpu::TextureData& out,
              int& vkFormat, uint64_t& hash);

    // Keeps `converted`, made from `source` (whose hash Load gave), for the next
    // session. Copies; the write happens on the store's thread.
    void Save(const gpu::TextureData& source, uint32_t xenosFormat, uint64_t hash,
              const gpu::TextureData& converted, int vkFormat);

    struct Stats { uint64_t hits, misses, saved, savedBytes, hashNs, readNs, records, fileBytes, dropped; };
    Stats Counts();
}
