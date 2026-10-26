#pragma once
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include "diagnostics.h"
// Minimal logging. LOGK is for the lines that trace what the title asked of the
// kernel or the GPU, "[k]" so they can be told from the rest.
//
// A whole run emits a few hundred lines, so this is not a hot path. Configure
// with -DPES17_LOGGING=OFF to compile every call below to nothing, warnings
// included; -DPES17_DIAGNOSTICS=OFF is what removes the traces and reports.
//
// The disabled form keeps the arguments inside an `if (false)`, so they are
// still type-checked and still count as used, and no expression in a log call
// may have a side effect the program needs.
//
// PES17_LOG_TIME=1 starts every line with the seconds since the first one, so a
// line can be matched with something somebody saw on screen at that moment.
//
// A line that repeats is written once and then counted rather than written
// again (Repeat, below). The runs worth reading are the ones that log one thing
// a thousand times -- a texture that will not restage, an opcode the command
// processor does not implement -- and in a log where that line is written a
// thousand times the line that says what actually happened is unfindable.
// PES17_LOG_ALL=1 writes every line as it comes, for when the count is not enough.
namespace pes17log
{
    inline const char* Stamp()
    {
        static const bool on = diag::Flag("PES17_LOG_TIME");
        if (!on) return "";
        static const auto start = std::chrono::steady_clock::now();
        thread_local char text[24];
        const double seconds = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - start).count();
        std::snprintf(text, sizeof text, "%9.3f ", seconds);
        return text;
    }

    // "[w] what the caller said", in the calling thread's own buffer and of any
    // length: the longer messages here (a pipeline's state, a texture's) are the
    // ones somebody is reading precisely because something went wrong with them,
    // and truncating one cuts the part that was wanted. Never null, so a caller
    // that formats and then counts does not have to test it.
    inline const std::string& Line(char level, const char* format, va_list arguments)
    {
        thread_local std::string text;
        char prefix[8];
        const int prefixLength = std::snprintf(prefix, sizeof prefix, "[%c] ", level);
        const size_t at = prefixLength > 0 ? size_t(prefixLength) : 0;
        text.assign(prefix, at);

        size_t room = 256;
        for (;;)
        {
            text.resize(at + room);
            va_list copy;
            va_copy(copy, arguments);
            const int written = std::vsnprintf(text.data() + at, room, format, copy);
            va_end(copy);
            if (written < 0) { text.resize(at); return text; }
            if (size_t(written) < room) { text.resize(at + size_t(written)); return text; }
            room = size_t(written) + 1;
        }
    }

    // The line with its numbers taken out: what two lines that differ only in an
    // address, a size or a shader hash have in common. Those two are one thing
    // happening twice, which is why the count is kept against this and not
    // against the line itself -- a texture failing on four hundred different
    // addresses is otherwise four hundred different lines.
    inline std::string Shape(const std::string& line)
    {
        std::string out;
        out.reserve(line.size());
        for (size_t at = 0; at < line.size();)
        {
            const unsigned char c = (unsigned char)line[at];

            // One run of hexadecimal with a digit in it: a number, however it
            // was written. Without a digit it is a word spelled in a-f -- "e",
            // "fence", "beef", "decade" -- and words are what tells one message
            // from another, so they stay. A single digit counts: "handle 1" and
            // "handle 2" are the same message twice, and reading them as two is
            // how the first version of this left the first nine lines of a
            // repetition unfolded.
            //
            // Hex and decimal are one rule rather than two, because the same
            // number arrives both ways and splitting them is how the second
            // version missed %08X: it read "08A00000" as a two-digit decimal
            // followed by a word, so four hundred addresses were four hundred
            // different lines. "0x1F00" comes out as "#x#" by itself, the x
            // being a character this does not eat.
            if (std::isxdigit(c))
            {
                size_t end = at;
                bool digit = std::isdigit(c) != 0;
                while (end < line.size() && std::isxdigit((unsigned char)line[end]))
                {
                    if (std::isdigit((unsigned char)line[end])) digit = true;
                    end++;
                }
                if (digit) out += '#';
                else out.append(line, at, end - at);
                at = end;
                continue;
            }
            out += char(c);
            at++;
        }
        return out;
    }

    // Whether this line is one to write, and what to put after it: "" the first
    // time, "  (×412)" on one of the doublings, and null for one of the hundreds
    // that are being counted instead. A repeat is therefore visible as a repeat,
    // with the size it had reached beside it, for the price of twice the
    // logarithm of the lines it replaces.
    //
    // The count written is the largest of the doublings a repetition has reached,
    // so it is never above the truth and within a factor of two of it. That is
    // what saying four hundred happened costs without writing four hundred lines;
    // PES17_LOG_ALL=1 writes the four hundred.
    //
    // Nothing here ends a repetition when the lines of it stop. The first version
    // did, and the benchmark of interleaved messages -- texture, kernel, gpu,
    // texture -- showed what that costs: three kinds taking turns end each other's
    // repetitions on every line, so a quarter of a million calls wrote a quarter of
    // a million lines. A repetition is a repetition whether or not something else
    // was logged between its lines.
    inline const char* Repeat(const std::string& line)
    {
        static const bool all = env::Flag("PES17_LOG_ALL");
        if (all) return "";

        // One entry per kind of line. Past a hundred and twenty-eight kinds a
        // new one is written as it comes, which is the right answer: a log with
        // more distinct messages than that is not one where counting helps.
        constexpr size_t kKinds = 128;
        struct Entry
        {
            std::string shape;
            uint64_t hits;
        };

        static std::mutex lock;
        static Entry kinds[kKinds];
        static size_t used = 0;

        const std::string shape = Shape(line);

        std::lock_guard guard(lock);
        for (size_t i = 0; i < used; i++)
        {
            if (kinds[i].shape != shape) continue;
            const uint64_t hits = ++kinds[i].hits;
            // 2nd, 4th, 8th, 16th ... and nothing between them.
            if ((hits & (hits - 1)) != 0) return nullptr;
            thread_local char suffix[32];
            std::snprintf(suffix, sizeof suffix, "  (×%llu)", (unsigned long long)hits);
            return suffix;
        }
        if (used < kKinds)
        {
            kinds[used].shape = shape;
            kinds[used].hits = 1;
            used++;
        }
        return "";
    }
}

// Android has no stderr to write to: a process's output goes nowhere unless
// the device was set up to keep it. The lines go to logcat and to the app's
// own log screen instead (runtime/android/log.cpp), through one function
// rather than the macro's format string, so the level survives.
#ifdef PES17_ANDROID
namespace pes17log
{
    void AndroidWrite(char level, const char* format, ...) __attribute__((format(printf, 2, 3)));
}
#else
namespace pes17log
{
    // The line, and the count of its repeats, to stderr. A function rather than
    // a macro so that the format is still checked and the buffer it formats into
    // can grow; see Line above.
    inline void Write(char level, const char* format, ...) __attribute__((format(printf, 2, 3)));
    inline void Write(char level, const char* format, ...)
    {
        va_list arguments;
        va_start(arguments, format);
        const std::string& line = Line(level, format, arguments);
        va_end(arguments);
        if (const char* suffix = Repeat(line))
            std::fprintf(stderr, "%s%s%s\n", Stamp(), line.c_str(), suffix);
    }
}
#endif

#if defined(PES17_NO_LOGGING)
#   define PES17_LOG_AT(level, fmt, ...) \
        do { if (false) std::fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while (false)
#elif defined(PES17_ANDROID)
#   define PES17_LOG_AT(level, fmt, ...) pes17log::AndroidWrite(level, fmt, ##__VA_ARGS__)
#else
#   define PES17_LOG_AT(level, fmt, ...) pes17log::Write(level, fmt, ##__VA_ARGS__)
#endif

#define LOGI(fmt, ...) PES17_LOG_AT('i', fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) PES17_LOG_AT('w', fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) PES17_LOG_AT('E', fmt, ##__VA_ARGS__)
#define LOGK(fmt, ...) PES17_LOG_AT('k', fmt, ##__VA_ARGS__)
