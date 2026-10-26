// The log's repeat counting (runtime/log.h): that one line happening four
// hundred times is written a handful of times with the size of it beside, that
// the same line on four hundred different addresses is one kind of line, that a
// repetition which stops gets its exact size, and that two lines which differ by
// more than a number stay two lines.
//
// It asserts rather than reporting, so ctest passes or fails; nothing it
// includes touches the guest, the disc, the GPU or the platform, so it runs on a
// tree with none of them.
#include "log.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void Check(bool ok, const std::string& what)
    {
        g_checks++;
        if (ok) return;
        g_failures++;
        std::fprintf(stdout, "FAIL: %s\n", what.c_str());
    }

    std::vector<std::string> LinesOf(const std::string& text)
    {
        std::vector<std::string> lines;
        for (size_t at = 0; at < text.size();)
        {
            const size_t end = text.find('\n', at);
            if (end == std::string::npos) { lines.push_back(text.substr(at)); break; }
            lines.push_back(text.substr(at, end - at));
            at = end + 1;
        }
        return lines;
    }

    // The lines carrying this text. Every case below shares this process with the
    // one before it, and the counting is global by design -- a repetition left
    // open by one case closes, in its own words, into the file of the next. So
    // what is asserted is the lines a case wrote, not how long its file is.
    int Count(const std::vector<std::string>& lines, const std::string& text)
    {
        int found = 0;
        for (const std::string& line : lines)
            if (line.find(text) != std::string::npos) found++;
        return found;
    }

    bool Any(const std::vector<std::string>& lines, const std::string& text)
    {
        return Count(lines, text) > 0;
    }

    // Runs `body` with the log pointed at a file of its own, and answers with
    // what it wrote.
    std::vector<std::string> Written(const char* name, const std::function<void()>& body)
    {
        const std::string path = std::string("log-repeat-") + name + ".txt";
        std::fflush(stderr);
        if (!std::freopen(path.c_str(), "w", stderr))
        {
            Check(false, std::string("could not open ") + path);
            return {};
        }
        body();
        std::fflush(stderr);

        std::string all;
        if (std::FILE* read = std::fopen(path.c_str(), "rb"))
        {
            char chunk[4096];
            for (size_t got; (got = std::fread(chunk, 1, sizeof chunk, read)) > 0;)
                all.append(chunk, got);
            std::fclose(read);
        }
        std::remove(path.c_str());
        return LinesOf(all);
    }
}

int main()
{
    // One line, four hundred times. 400 is not a special number: the counting
    // writes the 1st, 2nd, 4th ... 256th, which is nine lines, and the ninth says
    // how far the repetition had got when it was last written.
    {
        const std::vector<std::string> lines = Written("same", []
        {
            for (int i = 0; i < 400; i++) LOGW("case-same: restage skipped, memory moved under it");
        });
        Check(lines.size() == 9, "400 identical lines are written 9 times, not " +
                                 std::to_string(lines.size()));
        if (!lines.empty())
        {
            Check(lines.front().rfind("[w] ", 0) == 0, "the line keeps its level marker");
            Check(lines.front().find("×") == std::string::npos, "the first one carries no count");
        }
        if (lines.size() >= 2)
            Check(lines[1].find("(×2)") != std::string::npos, "the second says twice: " + lines[1]);
        if (lines.size() == 9)
            Check(lines.back().find("(×256)") != std::string::npos, "the last says 256: " + lines.back());
    }

    // The same line on four hundred different addresses, which is what most of
    // the repeats in this codebase look like: %08X writes a number with no 0x in
    // front of it, and reading one as text makes four hundred kinds of line.
    {
        const std::vector<std::string> lines = Written("addresses", []
        {
            for (int i = 0; i < 400; i++)
                LOGW("case-addresses: restaged %08X (%u bytes)", 0x8a00000u + unsigned(i) * 4u, 4096u);
        });
        Check(Count(lines, "restaged") == 9, "400 differing addresses are written 9 times, not " +
                                             std::to_string(Count(lines, "restaged")));
    }

    // Shader and pipeline hashes are written without 0x too, and are the other
    // thing here that repeats with only a number changing.
    {
        const std::vector<std::string> lines = Written("hashes", []
        {
            for (unsigned i = 0; i < 200; i++)
                LOGW("case-hashes: vertex shader %016llX not usable: no translator for it",
                     (unsigned long long)(0x0123456789ABCDEFull + i));
        });
        Check(Count(lines, "not usable") == 8, "200 differing shader hashes are written 8 times, not " +
                                               std::to_string(Count(lines, "not usable")));
    }

    // Three kinds of line taking turns, which is what a log looks like: a
    // texture warning, a kernel trace, a gpu wait, a texture warning. A
    // repetition is a repetition whether or not something else is logged between
    // its lines, and this is the case that says so -- an earlier version of this
    // closed each repetition when the next kind appeared, so three kinds taking
    // turns wrote one line per call, which is the flood it exists to stop.
    {
        const std::vector<std::string> lines = Written("interleaved", []
        {
            for (int i = 0; i < 3000; i++)
            {
                if (i % 3 == 0)
                    LOGW("case-interleaved-a: restaged %08X (%u bytes)",
                         0x8a00000u + unsigned(i) * 4u, 4096u);
                else if (i % 3 == 1)
                    LOGI("case-interleaved-b: object %08X created, handle %u",
                         0x1000u + unsigned(i) * 4u, i + 1);
                else
                    LOGW("case-interleaved-c: gave up waiting for %08X to compare", 0x1234u + unsigned(i));
            }
        });
        Check(lines.size() == 30, "3000 interleaved lines are written 30 times, not " +
                                  std::to_string(lines.size()));
        Check(Count(lines, "case-interleaved-a") == 10, "the first kind is counted over the others");
        Check(Count(lines, "case-interleaved-b") == 10, "so is the second");
        Check(Count(lines, "case-interleaved-c") == 10, "so is the third");
    }

    // A repetition that reaches a thousand says so, and one that stops a line
    // later does not write anything more: the count written is the largest of the
    // doublings, never above the truth.
    {
        const std::vector<std::string> lines = Written("thousand", []
        {
            for (int i = 0; i < 1000; i++) LOGW("case-thousand: arena overflowed at draw %d", i);
            LOGW("gpu: something else entirely");
        });
        Check(Count(lines, "case-thousand") == 10, "1000 repetitions are written 10 times, not " +
                                                    std::to_string(Count(lines, "case-thousand")));
        Check(Any(lines, "(×512)"), "and the last of them says how far it got");
        Check(!Any(lines, "in all"), "nothing is written when a repetition stops");
    }

    // Different messages are different lines, and are not folded together.
    {
        const std::vector<std::string> lines = Written("distinct", []
        {
            LOGW("case-distinct: vulkan: no command pool");
            LOGW("case-distinct: textures: no fence");
            LOGW("case-distinct: perf: memory is short (level 10); the texture budget is now 512 MB");
        });
        Check(Count(lines, "no command pool") == 1, "the first message is written once");
        Check(Count(lines, "no fence") == 1, "the second is not folded into it");
        Check(Count(lines, "texture budget is now") == 1, "nor is the third");
    }

    // A line longer than any fixed buffer survives entire: a truncated
    // diagnostic is missing the part it was written for.
    {
        const std::string body(900, 'x');
        std::string written;
        const std::vector<std::string> lines = Written("long", [&]
        {
            LOGW("case-long: dump: %s", body.c_str());
        });
        Check(Count(lines, "case-long") == 1, "one long line is one line");
        for (const std::string& line : lines)
            if (line.find("case-long") != std::string::npos) written = line;
        Check(written.size() >= 900, "a 900 character line is not truncated: " +
                                     std::to_string(written.size()) + " characters");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
