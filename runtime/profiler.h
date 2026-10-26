#pragma once
// PES17_PROFILE=<names>: a sampling profiler for the threads whose registered names contain
// one of the comma-separated substrings ("main thread,gpu ring,recorder,routine 83863380").
//
// simpleperf and perf_event_open are refused on some phones (the POCO X7 Pro's: neither
// cpu-cycles nor cpu-clock is supported), so this samples from inside: a thread of its own
// signals each chosen thread about five hundred times a second, and the handler writes down
// where the thread is (its pc and a few return addresses) into a ring. Touching
// files/profile_dump makes it write what it has to the log, as module+offset lines to be turned
// into function names on the host (tools/profile_report.py), and start again from nothing.
// Wall-clock samples, not CPU ones: a thread asleep in a wait shows as the wait.
namespace profiler
{
    // Called with every thread's registered name, from the thread itself.
    void Note(const char* name);
}
