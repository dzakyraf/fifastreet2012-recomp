// The order launcher/version.h gives release tags, and the tags it refuses.
//
// This is the part of the update check that can be wrong quietly. A tag the
// updater cannot read makes it say the newest version "is not one" -- the
// launcher then never offers that release -- and a tag ordered wrongly offers
// an update that goes backwards. Neither needs a network, a release or a
// launcher to check, which is why it is here.
//
//   * "v1.2.3" and "1.2.3" are the same version, and "1.2" is "1.2.0".
//   * A pre-release is older than its release: 0.0.2-alpha < 0.0.2.
//   * Pre-releases of one version are ordered by identifier: alpha.2 <
//     alpha.10 (numerically, not as text), alpha < alpha.1, alpha < beta.
//   * The project's own tags read: v0.1.0-alpha.13, v0.4.1.
//   * What is not a version is not one: empty, "v", "nightly", "1.x", "1..2",
//     "1.2.", "v1.2.3-".
#include "version.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void Check(bool ok, const std::string& what)
    {
        g_checks++;
        if (ok) return;
        g_failures++;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }

    bool Valid(const std::string& tag)
    {
        return version::Parse(tag).valid;
    }

    // "a < b", checked as both directions so one wrong answer cannot pass.
    void Older(const std::string& a, const std::string& b)
    {
        const version::Tag left = version::Parse(a), right = version::Parse(b);
        Check(left.valid && right.valid, "\"" + a + "\" and \"" + b + "\" are versions");
        Check(version::Compare(left, right) < 0, a + " < " + b);
        Check(version::Compare(right, left) > 0, b + " > " + a);
        Check(!version::Newer(left, right), a + " is not newer than " + b);
    }

    void Same(const std::string& a, const std::string& b)
    {
        const version::Tag left = version::Parse(a), right = version::Parse(b);
        Check(left.valid && right.valid, "\"" + a + "\" and \"" + b + "\" are versions");
        Check(version::Compare(left, right) == 0, a + " == " + b);
        Check(!version::Newer(left, right) && !version::Newer(right, left), a + " and " + b);
    }

    void Rejected(const std::string& tag)
    {
        Check(!Valid(tag), "\"" + tag + "\" is not a version");
    }
}

int main()
{
    // The plain forms, and the ones the project's tags use.
    Same("v0.4.2", "0.4.2");
    Same("v1.2", "v1.2.0");
    Same("V1.0.0", "v1.0.0");
    Same("v0.0.2-alpha", "0.0.2-alpha");
    Older("v0.0.1", "v0.0.2");
    Older("v0.0.2-alpha", "v0.0.2");
    Older("v0.0.2", "v0.0.3-alpha");
    Older("v0.9.9", "v1.0.0");

    // Pre-release precedence, which is what "-alpha" tags need.
    Older("v0.0.2-alpha", "v0.0.2-alpha.1");
    Older("v0.0.2-alpha.2", "v0.0.2-alpha.10");
    Older("v0.0.2-alpha.9", "v0.0.2-alpha.10");
    Older("v0.0.2-alpha", "v0.0.2-beta");
    Older("v0.0.2-beta", "v0.0.2");
    Older("v0.0.2-alpha.99", "v0.0.2-beta.1");
    Older("v0.0.2-1", "v0.0.2-alpha");        // a numeric identifier sorts below text
    Same("v0.0.2-alpha.1", "v0.0.2-alpha.01");

    // The tags this project has published, against the ones it will.
    Older("v0.1.0-alpha.9", "v0.1.0-alpha.13");
    Older("v0.1.0-alpha.13", "v0.1.0");
    Older("v0.4.1", "v0.4.2");
    Older("v0.0.2-alpha", "v0.4.1");
    Older("v0.1.0-alpha.13", "v0.4.1");

    // Four fields are read, not refused: Xbox title updates are named that way.
    Older("v1.2.3", "v1.2.3.1");

    // Not versions: a build whose own tag is one of these does not update,
    // which is the safe answer, rather than one it compared by accident.
    Rejected("");
    Rejected("v");
    Rejected("nightly");
    Rejected("1.x");
    Rejected("1..2");
    Rejected("1.2.");
    Rejected("v1.2.3-");
    Rejected("v1.2.3-alpha.");
    Rejected("v1.2.3-alpha..1");
    Rejected("v1.2.3 ");       // a trailing space is not the tag
    Rejected("v 1.2.3");
    Rejected("-1.2.3");

    // An invalid tag compares equal to nothing, rather than older or newer:
    // a caller that did not check `valid` must not get a direction it can act on.
    const version::Tag none = version::Parse("nightly"), some = version::Parse("v1.0.0");
    Check(version::Compare(none, some) == 0 && version::Compare(some, none) == 0,
          "an unreadable tag has no order");
    Check(!version::Newer(none, some), "an unreadable tag is not newer");

    if (g_failures)
    {
        std::fprintf(stderr, "version: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("version: %d checks passed\n", g_checks);
    return 0;
}
