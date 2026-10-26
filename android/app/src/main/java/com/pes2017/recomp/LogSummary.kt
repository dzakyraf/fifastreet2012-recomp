package com.pes2017.recomp

import android.content.Context
import java.io.File

/**
 * The log screen's text: what is running, what went wrong, and where the run got
 * to -- not the log.
 *
 * The log is a few hundred lines that record every decision the runtime took,
 * which is the right thing to attach to a bug report and the wrong thing to put
 * in front of somebody on a phone: the four lines that matter are somewhere in
 * the middle of it. So this reads the end of the log, keeps the run that is
 * being asked about, counts what warned and what failed, and prints each kind of
 * warning once with a count rather than printing four hundred of them. The whole
 * log is still one button away, because sometimes the whole log is what is
 * wanted.
 */
object LogSummary {

    /** A run is appended after a line of its own, so the tail holds several. */
    private const val kRunMarker = "===== new run"

    /** The end of the log is the part that describes the run being looked at. */
    private const val kScanBytes = 1 shl 20

    /** Kinds of warning and error printed; the rest are counted, not shown. */
    private const val kNotableLines = 20

    /** How far the run got: a line or two, twelve to be sure. */
    private const val kTailLines = 12

    /** A crash is the most precise evidence there is, and its top says it. */
    private const val kCrashLines = 14

    /** One line, so a dumped buffer cannot fill the screen it is shown on. */
    private const val kLineLength = 200

    /**
     * An address, which is the part a line repeats with and the part nobody
     * reads. Two lines that differ only here are one line that happened twice:
     * the runtime naming four hundred different blocks in four hundred warnings
     * is four hundred warnings about one thing.
     */
    private val ADDRESS = Regex("0x[0-9a-fA-F]+")

    fun build(context: Context, renderer: String, sessionTail: String): String
    {
        val log = File(context.filesDir, "game.log").readTextTail(kScanBytes)
            .ifBlank { File(context.filesDir, "launcher.log").readTextTail(kScanBytes) }
        val crash = crashOf(context)
        // Nothing has run yet: the screen says so rather than counting the
        // errors of a log that does not exist.
        if (log.isBlank() && crash.isBlank() && sessionTail.isBlank())
            return context.getString(R.string.log_empty)

        val lines = log.substringAfterLast(kRunMarker).lines().filter { it.isNotBlank() }

        val errors = lines.count { levelOf(it) == 'E' }
        val warnings = lines.count { levelOf(it) == 'w' }

        // In the order each kind first appears, with how often it happened.
        val counts = LinkedHashMap<String, Int>()
        val examples = LinkedHashMap<String, String>()
        for (line in lines)
        {
            val level = levelOf(line) ?: continue
            if (level != 'w' && level != 'E') continue
            val shape = shapeOf(line)
            if (!counts.containsKey(shape)) examples[shape] = trimmed(line)
            counts[shape] = (counts[shape] ?: 0) + 1
        }
        val recent = counts.keys.toList().takeLast(kNotableLines)
        val hidden = counts.size - recent.size

        val where = collapse(lines.takeLast(kTailLines * 4).ifEmpty {
            sessionTail.lines().filter { it.isNotBlank() }.takeLast(kTailLines * 4)
        })

        return buildString {
            append(context.getString(R.string.app_name))
                .append(" · build ").append(BuildConfig.BUILD_SHA).append('\n')
            if (renderer.isNotBlank())
                append(renderer.split('|').joinToString(" · ")).append('\n')

            if (crash.isNotBlank())
            {
                append('\n').append(context.getString(R.string.log_stopped)).append('\n')
                crash.lines().filter { it.isNotBlank() }.take(kCrashLines)
                    .forEach { append(trimmed(it)).append('\n') }
            }

            append('\n')
            if (errors == 0 && warnings == 0)
                append(context.getString(R.string.log_none)).append('\n')
            else
                append(context.getString(R.string.log_counts, errors, warnings)).append('\n')
            for (shape in recent)
            {
                append(examples[shape])
                val count = counts[shape] ?: 1
                if (count > 1) append("  ×").append(count)
                append('\n')
            }
            if (hidden > 0) append(context.getString(R.string.log_more, hidden)).append('\n')

            val tail = where.takeLast(kTailLines)
            if (tail.isNotEmpty())
            {
                append('\n').append(context.getString(R.string.log_where)).append('\n')
                for ((line, count) in tail)
                {
                    append(line)
                    if (count > 1) append("  ×").append(count)
                    append('\n')
                }
            }

            append('\n').append(context.getString(R.string.log_share_note))
        }
    }

    /** Everything the app has written, in one string: what Share sends. */
    fun whole(context: Context, sessionTail: String): String = buildString {
        for (name in listOf("game.log", "launcher.log"))
        {
            val file = File(context.filesDir, name)
            if (!file.exists()) continue
            append("===== ").append(name).append(" =====\n")
            append(file.readTextTail(60_000)).append("\n\n")
        }
        for (name in listOf("last-crash-game.txt", "last-crash-launcher.txt"))
        {
            val file = File(context.filesDir, name)
            if (!file.exists()) continue
            append("===== ").append(name).append(" =====\n")
            append(file.readTextTail(8_000)).append("\n\n")
        }
        if (sessionTail.isNotBlank())
            append("===== this session =====\n").append(sessionTail)
    }.ifBlank { context.getString(R.string.log_empty) }

    /** The other process's crash, which is the one that matters, or this one's. */
    private fun crashOf(context: Context): String
    {
        for (name in listOf("last-crash-game.txt", "last-crash-launcher.txt"))
        {
            val file = File(context.filesDir, name)
            if (file.exists() && file.length() > 0) return file.readTextTail(8_000)
        }
        return Trail.lastCrash(context)
    }

    /**
     * Runs of one line, as (the last of them, how many). The end of a log is
     * often the same line sixty times -- sixty objects waited on, sixty textures
     * restaged -- and the line that says where the run got to is the one after
     * them, which this is what leaves room for.
     */
    private fun collapse(lines: List<String>): List<Pair<String, Int>>
    {
        val out = ArrayList<Pair<String, Int>>()
        for (line in lines)
        {
            val last = out.lastOrNull()
            if (last != null && shapeOf(last.first) == shapeOf(line))
                out[out.size - 1] = last.first to last.second + 1
            else
                out.add(trimmed(line) to 1)
        }
        return out
    }

    /**
     * The level a line carries, or null for anything that is not one of the
     * runtime's lines: a stack trace, a blank line, the separator between runs.
     *
     * The marker is "[x] " at the front, except that PES17_LOG_TIME puts nine
     * columns of seconds in front of it -- so the search is bounded rather than
     * a prefix test, and the bracket is closed at once, which is what keeps a
     * genuine line apart from a bracketed address inside a trace.
     */
    private fun levelOf(line: String): Char?
    {
        val at = line.indexOf('[')
        if (at < 0 || at > 16) return null
        if (at + 2 >= line.length || line[at + 2] != ']') return null
        val level = line[at + 1]
        return if (level == 'i' || level == 'w' || level == 'E' || level == 'k') level else null
    }

    private fun shapeOf(line: String): String = ADDRESS.replace(line, "0x…")

    private fun trimmed(line: String): String =
        if (line.length <= kLineLength) line else line.take(kLineLength - 3) + "..."
}
