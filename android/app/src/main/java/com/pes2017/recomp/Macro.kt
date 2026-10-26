package com.pes2017.recomp

import android.content.Context
import android.os.SystemClock
import android.util.Log
import java.io.File

/**
 * Records what a physical controller does and plays it back, so a run can be
 * repeated exactly -- to test a change, or to get to the same moment of a
 * match -- without anyone holding the pad.
 *
 * A macro is a plain text file in `files/macros/<name>.macro`, one line per
 * change of a pad's state:
 *
 *     <ms since the start> <slot> <buttons> <lt> <rt> <lx> <ly> <rx> <ry>
 *
 * The numbers are exactly what NativeBridge.nativePadState takes, so a file
 * can be read, trimmed or written by hand. Lines starting with # are ignored.
 *
 * Slots 0..3 are physical pads. Slot 4 ([TOUCH]) is the on-screen overlay's
 * buttons, triggers and sticks, so a scenario can mix both players. The look
 * area is sent as finger movement, not a stick, and is not recorded.
 */
object Macro {
    private const val TAG = "PES17.Macro"

    /** The overlay controls, as a slot of their own in a macro file. */
    const val TOUCH = 4

    private class Frame(
        val ms: Long, val slot: Int, val buttons: Int, val lt: Int, val rt: Int,
        val lx: Float, val ly: Float, val rx: Float, val ry: Float,
    )

    private val frames = ArrayList<Frame>()
    private val last = arrayOfNulls<Frame>(5)
    private var startedAt = 0L

    @Volatile var recording = false
        private set

    @Volatile var playing = false
        private set

    @Volatile private var player: Thread? = null

    fun directory(context: Context): File =
        File(context.getExternalFilesDir(null) ?: context.filesDir, "macros").apply { mkdirs() }

    fun names(context: Context): List<String> =
        directory(context).listFiles { f -> f.name.endsWith(".macro") }
            ?.map { it.name.removeSuffix(".macro") }?.sorted() ?: emptyList()

    fun eventCount(): Int = synchronized(frames) { frames.size }

    // ---- recording -----------------------------------------------------------

    fun startRecording() {
        synchronized(frames) {
            frames.clear()
            last.fill(null)
            startedAt = SystemClock.elapsedRealtime()
            recording = true
        }
        Log.i(TAG, "recording started")
    }

    /** Called with every state a physical pad sends; a no-op unless recording. */
    fun record(slot: Int, buttons: Int, lt: Int, rt: Int, lx: Float, ly: Float, rx: Float, ry: Float) {
        if (!recording || slot !in 0..TOUCH) return
        synchronized(frames) {
            val previous = last[slot]
            if (previous != null && previous.buttons == buttons && previous.lt == lt && previous.rt == rt &&
                previous.lx == lx && previous.ly == ly && previous.rx == rx && previous.ry == ry
            ) return
            val frame = Frame(SystemClock.elapsedRealtime() - startedAt, slot, buttons, lt, rt, lx, ly, rx, ry)
            last[slot] = frame
            frames.add(frame)
        }
    }

    /** Stops and saves. Returns the number of events written, or -1 if nothing was recorded. */
    fun stopRecording(context: Context, name: String): Int {
        val copy: List<Frame>
        val length: Long
        synchronized(frames) {
            recording = false
            copy = ArrayList(frames)
            length = SystemClock.elapsedRealtime() - startedAt
        }
        if (copy.isEmpty()) return -1
        val file = File(directory(context), "${safeName(name)}.macro")
        file.bufferedWriter().use { out ->
            out.write("# pes17-macro v1, ${copy.size} events, ${length} ms\n")
            for (f in copy) {
                out.write("${f.ms} ${f.slot} ${f.buttons} ${f.lt} ${f.rt} ${f.lx} ${f.ly} ${f.rx} ${f.ry}\n")
            }
            // A final line so the end of the macro is part of it: the wait
            // after the last press is as much a part of the run as the press.
            out.write("# end ${length}\n")
        }
        Log.i(TAG, "recorded ${copy.size} events to ${file.name}")
        return copy.size
    }

    // ---- playback ------------------------------------------------------------

    /** Plays a macro on its own thread. `loops` of 0 repeats until stopped. */
    fun play(context: Context, name: String, loops: Int = 1, onDone: (String) -> Unit = {}): Boolean {
        if (playing) return false
        val file = File(directory(context), "${safeName(name)}.macro")
        if (!file.isFile) {
            onDone("no macro named ${safeName(name)}")
            return false
        }
        val events = ArrayList<Frame>()
        var endMs = 0L
        try {
            for (line in file.readLines()) {
                if (line.startsWith("# end ")) endMs = line.removePrefix("# end ").trim().toLongOrNull() ?: 0L
                if (line.isBlank() || line.startsWith("#")) continue
                val p = line.trim().split(' ')
                if (p.size < 9) continue
                events.add(Frame(p[0].toLong(), p[1].toInt(), p[2].toInt(), p[3].toInt(), p[4].toInt(),
                    p[5].toFloat(), p[6].toFloat(), p[7].toFloat(), p[8].toFloat()))
            }
        } catch (error: Throwable) {
            onDone("could not read ${file.name}: ${error.message}")
            return false
        }
        if (events.isEmpty()) {
            onDone("${file.name} has no events")
            return false
        }
        if (endMs < events.last().ms) endMs = events.last().ms
        val slots = events.map { it.slot }.toSet()
        playing = true
        val thread = Thread({
            var message = "finished"
            try {
                for (slot in slots) if (slot < TOUCH) NativeBridge.nativePadConnected(slot, true)
                var round = 0
                while (playing && (loops == 0 || round < loops)) {
                    val t0 = SystemClock.elapsedRealtime()
                    for (f in events) {
                        if (!sleepUntil(t0 + f.ms)) break
                        send(f.slot, f.buttons, f.lt, f.rt, f.lx, f.ly, f.rx, f.ry)
                    }
                    sleepUntil(t0 + endMs)
                    round++
                }
                if (!playing) message = "stopped"
            } catch (error: Throwable) {
                message = "failed: ${error.message}"
            } finally {
                // Every input let go, or the last pressed one stays pressed.
                for (slot in slots) send(slot, 0, 0, 0, 0f, 0f, 0f, 0f)
                playing = false
                player = null
                Log.i(TAG, "playback $message")
                onDone(message)
            }
        }, "pes17-macro")
        player = thread
        thread.isDaemon = true
        thread.start()
        Log.i(TAG, "playing ${file.name}: ${events.size} events, ${endMs} ms, loops=$loops")
        return true
    }

    private fun send(slot: Int, buttons: Int, lt: Int, rt: Int, lx: Float, ly: Float, rx: Float, ry: Float) {
        if (slot == TOUCH) NativeBridge.nativeTouchState(buttons, lt, rt, lx, ly, rx, ry)
        else NativeBridge.nativePadState(slot, buttons, lt, rt, lx, ly, rx, ry)
    }

    fun stopPlayback() {
        playing = false
        player?.interrupt()
    }

    /** False when playback was stopped while waiting. */
    private fun sleepUntil(uptimeMs: Long): Boolean {
        while (true) {
            if (!playing) return false
            val wait = uptimeMs - SystemClock.elapsedRealtime()
            if (wait <= 0) return true
            try {
                Thread.sleep(minOf(wait, 50L))
            } catch (_: InterruptedException) {
                return playing
            }
        }
    }

    private fun safeName(name: String): String =
        name.trim().ifEmpty { "last" }.map { if (it.isLetterOrDigit() || it == '-' || it == '_') it else '_' }.joinToString("")
}
