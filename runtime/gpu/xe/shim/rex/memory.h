#pragma once
// Shim for rex/memory.h: a fourcc, a bit cast, and the guest's physical memory.
#include <bit>
#include <cstdint>
namespace rex::memory
{
    using fourcc_t = uint32_t;
    constexpr fourcc_t make_fourcc(const char (&s)[5])
    {
        return fourcc_t(uint8_t(s[0])) << 24 | fourcc_t(uint8_t(s[1])) << 16 |
               fourcc_t(uint8_t(s[2])) << 8 | fourcc_t(uint8_t(s[3]));
    }
    template <typename To, typename From> To Reinterpret(const From& from) { return std::bit_cast<To>(from); }

    // What the interpreter reads vertex data through: the host address of
    // physical address zero.
    class Memory
    {
    public:
        explicit Memory(const uint8_t* physical) : physical_(physical) {}
        const uint8_t* physical_membase() const { return physical_; }
    private:
        const uint8_t* physical_;
    };
}
