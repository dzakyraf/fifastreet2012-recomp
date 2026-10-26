#pragma once
// A release tag as a value: "v0.4.2", and the pre-releases the project also
// publishes -- "v0.0.2-alpha", "v0.1.0-alpha.13", "v0.4.0-beta.2".
//
// The updater used to read a tag as a run of numbers and nothing else, so a
// pre-release could not be read at all: a build whose own tag was
// "v0.1.0-alpha.4" could neither be told apart from the newest one nor be
// offered it, and a pre-release of a newer version looked like a broken
// description of the release rather than what it was. That is the whole of why
// this is its own file: the comparison is the part that was wrong, and it can
// be checked without a launcher, a network or a release to look at
// (tests/version_test.cpp).
//
// The order is the one every package manager uses (semantic versioning,
// pre-release precedence): 0.0.2-alpha < 0.0.2-beta < 0.0.2 < 0.0.3-alpha. A
// pre-release of a version is older than that version, and pre-releases of one
// version are ordered by their identifiers, numerically where they are numbers
// ("alpha.10" after "alpha.9", not before it).
#include <cstdint>
#include <string>
#include <vector>

namespace version
{
    // A tag that could not be read: empty, "nightly", "v1.x". It is not a
    // version and must not compare as one -- a caller says so rather than
    // guessing, since guessing is what a wrong update is made of.
    struct Tag
    {
        std::vector<uint64_t> numbers;    // 0.4.2 -> {0, 4, 2}
        std::vector<std::string> pre;     // "-alpha.3" -> {"alpha", "3"}
        bool valid = false;
    };

    namespace detail
    {
        inline bool Digits(const std::string& text, size_t from, size_t to)
        {
            if (from >= to) return false;
            for (size_t i = from; i < to; i++)
                if (text[i] < '0' || text[i] > '9') return false;
            return true;
        }

        // A numeric identifier too long for the type counts as very large
        // rather than wrapping around to a small number.
        inline uint64_t Number(const std::string& text, size_t from, size_t to)
        {
            uint64_t value = 0;
            for (size_t i = from; i < to; i++)
            {
                const uint64_t digit = uint64_t(text[i] - '0');
                if (value > (UINT64_MAX - digit) / 10) return UINT64_MAX;
                value = value * 10 + digit;
            }
            return value;
        }

        inline bool Numeric(const std::string& id)
        {
            return Digits(id, 0, id.size());
        }

        // A pre-release identifier sorted as the specification does: numeric
        // ones by value, and a numeric one below an alphanumeric one.
        inline int CompareIdentifier(const std::string& left, const std::string& right)
        {
            const bool leftNumber = Numeric(left), rightNumber = Numeric(right);
            if (leftNumber && rightNumber)
            {
                const uint64_t a = Number(left, 0, left.size()), b = Number(right, 0, right.size());
                return a < b ? -1 : a > b ? 1 : 0;
            }
            if (leftNumber != rightNumber) return leftNumber ? -1 : 1;
            return left < right ? -1 : left > right ? 1 : 0;
        }
    }

    inline Tag Parse(const std::string& tag)
    {
        Tag out;
        size_t at = 0;
        if (at < tag.size() && (tag[at] == 'v' || tag[at] == 'V')) at++;
        if (at >= tag.size()) return out;

        // The release numbers: one or more of them, separated by periods. A
        // trailing period, a doubled one and a non-digit all mean the tag is
        // not a version -- "1.2." is not "1.2".
        for (;;)
        {
            const size_t start = at;
            while (at < tag.size() && tag[at] >= '0' && tag[at] <= '9') at++;
            if (!detail::Digits(tag, start, at)) return Tag{};
            out.numbers.push_back(detail::Number(tag, start, at));
            if (at < tag.size() && tag[at] == '.') { at++; continue; }
            break;
        }

        if (at < tag.size())
        {
            // Anything but the pre-release separator is not this tag's.
            if (tag[at] != '-') return Tag{};
            at++;
            if (at >= tag.size()) return Tag{};
            for (;;)
            {
                const size_t start = at;
                while (at < tag.size() && tag[at] != '.')
                {
                    const char c = tag[at];
                    const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                                       (c >= 'A' && c <= 'Z') || c == '-';
                    if (!alnum) return Tag{};
                    at++;
                }
                if (at == start) return Tag{};        // "alpha..1", or a trailing '.'
                out.pre.emplace_back(tag.substr(start, at - start));
                if (at >= tag.size()) break;
                at++;                                  // the '.'
                if (at >= tag.size()) return Tag{};
            }
        }

        out.valid = true;
        return out;
    }

    // -1 older, 0 the same version, 1 newer.
    inline int Compare(const Tag& left, const Tag& right)
    {
        if (!left.valid || !right.valid) return 0;

        const size_t fields = left.numbers.size() > right.numbers.size()
                                ? left.numbers.size() : right.numbers.size();
        for (size_t i = 0; i < fields; i++)
        {
            // "1.2" is "1.2.0": a field one tag does not write is a zero.
            const uint64_t a = i < left.numbers.size() ? left.numbers[i] : 0;
            const uint64_t b = i < right.numbers.size() ? right.numbers[i] : 0;
            if (a != b) return a < b ? -1 : 1;
        }
        // The same release: the one without a pre-release came after those with.
        if (left.pre.empty() != right.pre.empty()) return left.pre.empty() ? 1 : -1;
        for (size_t i = 0; i < left.pre.size() && i < right.pre.size(); i++)
        {
            const int order = detail::CompareIdentifier(left.pre[i], right.pre[i]);
            if (order) return order;
        }
        if (left.pre.size() != right.pre.size())
            return left.pre.size() < right.pre.size() ? -1 : 1;
        return 0;
    }

    inline bool Newer(const Tag& left, const Tag& right) { return Compare(left, right) > 0; }
}
