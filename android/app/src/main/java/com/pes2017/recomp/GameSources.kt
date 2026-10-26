package com.pes2017.recomp

import android.content.Context
import android.os.Environment
import java.io.File

/** A disc image or an extracted folder the installer could read the game from. */
data class GameSource(val file: File, val isFolder: Boolean, val bytes: Long) {
    val kind: String get() = if (isFolder) "folder" else "ISO"
}

/**
 * Finds the game on the phone without being told where: disc images and
 * extracted folders in the usual places. A folder counts when it holds
 * default.xex and fifadllzf.xex.dll, which is what FIFA Street unpacks to.
 * Reading outside the app's own folder needs the all-files permission, so what
 * this finds depends on what has been granted; the installer's own pickers
 * cover the rest.
 */
object GameSources {
    private const val MIN_ISO_BYTES = 500_000_000L
    private val NAME_HINTS = listOf("fifa", "street", "game")

    fun find(context: Context, installedDir: File?): List<GameSource> {
        val roots = linkedSetOf<File>()
        runCatching { roots += Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS) }
        roots += File("/sdcard/Download")
        roots += File("/sdcard")
        context.getExternalFilesDir(null)?.let { roots += it; roots += File(it, "import") }

        val found = linkedMapOf<String, GameSource>()
        fun consider(file: File, depth: Int) {
            val path = runCatching { file.canonicalPath }.getOrDefault(file.absolutePath)
            if (installedDir != null && path == runCatching { installedDir.canonicalPath }.getOrNull()) return
            when {
                file.isFile && file.name.endsWith(".iso", ignoreCase = true) && file.length() > MIN_ISO_BYTES ->
                    found.putIfAbsent(path, GameSource(file, false, file.length()))
                file.isDirectory && isExtractedGame(file) ->
                    found.putIfAbsent(path, GameSource(file, true, folderBytes(file)))
                file.isDirectory && depth < 2 ->
                    file.listFiles()?.forEach { consider(it, depth + 1) }
            }
        }
        for (root in roots) if (root.isDirectory) root.listFiles()?.forEach { consider(it, 1) }

        // Likeliest first: names that sound like the game, then folders (nothing to
        // copy twice), then the biggest.
        return found.values.sortedWith(
            compareByDescending<GameSource> { source -> NAME_HINTS.any { source.file.name.contains(it, ignoreCase = true) } }
                .thenByDescending { it.isFolder }
                .thenByDescending { it.bytes }
        )
    }

    fun isExtractedGame(folder: File): Boolean =
        File(folder, "default.xex").isFile && File(folder, "fifadllzf.xex.dll").isFile

    private fun folderBytes(folder: File): Long =
        folder.listFiles()?.filter { it.isFile }?.sumOf { it.length() } ?: 0L
}
