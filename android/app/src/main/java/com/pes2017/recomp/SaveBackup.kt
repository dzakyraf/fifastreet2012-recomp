package com.pes2017.recomp

import android.app.ActivityManager
import android.content.Context
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * Copies of the game's saves: Master League, Become a Legend, edit data, the
 * system data and the profile. The runtime keeps them all under `saves/`
 * beside the game folder (runtime/android/jni.cpp changes to the game folder's
 * parent; docs/saves.md), one directory per content package, so a backup is
 * that folder as a zip whose entries all start with `saves/`.
 *
 * Two kinds: one the player writes wherever they like (Settings > Game files),
 * and an automatic one taken in the app's own folder each time a match is
 * started from the launcher, the last [KEEP_AUTOMATIC] of them kept.
 */
object SaveBackup {

    private const val PREFIX = "saves/"
    private const val KEEP_AUTOMATIC = 5

    /** The folder the runtime saves into for the chosen game folder. */
    fun savesDir(context: Context, prefs: Prefs): File {
        val game = prefs.gameDirectory.ifEmpty { null }?.let { File(it) }
        val base = context.getExternalFilesDir(null) ?: context.filesDir
        return File(game?.parentFile ?: base, "saves")
    }

    /** Where the automatic backups are kept: the app's own folder, so no permission is needed. */
    fun automaticDir(context: Context): File =
        File(context.getExternalFilesDir(null) ?: context.filesDir, "save_backups")

    /** The packages in the saves folder (one directory each), for showing what a backup holds. */
    fun packages(saves: File): List<String> =
        saves.listFiles()?.filter { it.isDirectory && !it.name.endsWith(".bak") }?.map { it.name }?.sorted()
            ?: emptyList()

    /** Whether the game's process is up: its saves must not be replaced under it. */
    fun gameRunning(context: Context): Boolean {
        val manager = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager ?: return false
        val game = context.packageName + ":game"
        return manager.runningAppProcesses?.any { it.processName == game } == true
    }

    /** Writes the saves folder as a zip. Returns the number of files written. */
    fun write(saves: File, out: OutputStream): Int {
        var count = 0
        ZipOutputStream(out.buffered()).use { zip ->
            saves.walkTopDown().filter { it.isFile }.forEach { file ->
                val name = PREFIX + file.relativeTo(saves).invariantSeparatorsPath
                zip.putNextEntry(ZipEntry(name).apply { time = file.lastModified() })
                file.inputStream().use { it.copyTo(zip) }
                zip.closeEntry()
                count++
            }
        }
        return count
    }

    /**
     * Replaces the saves folder with a backup's. The backup is unpacked beside
     * it first and swapped in only once it is whole; the saves it replaces are
     * kept as `saves.before-restore-<time>`, so a wrong choice can be undone by
     * hand. Throws if the zip is not one of these backups.
     */
    fun restore(saves: File, input: InputStream): Int {
        val parent = saves.absoluteFile.parentFile ?: throw IllegalStateException("no folder above $saves")
        val staging = File(parent, "saves.restoring")
        staging.deleteRecursively()
        staging.mkdirs()
        var count = 0
        try {
            ZipInputStream(input.buffered()).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    val name = entry.name.replace('\\', '/')
                    if (!name.startsWith(PREFIX)) throw IllegalArgumentException("not a save backup: $name")
                    val target = File(staging, name.removePrefix(PREFIX)).canonicalFile
                    if (!target.path.startsWith(staging.canonicalPath + File.separator)) {
                        throw IllegalArgumentException("a path outside the saves: $name")
                    }
                    if (entry.isDirectory) { target.mkdirs(); continue }
                    target.parentFile?.mkdirs()
                    target.outputStream().use { zip.copyTo(it) }
                    if (entry.time > 0) target.setLastModified(entry.time)
                    count++
                }
            }
            if (count == 0) throw IllegalArgumentException("the backup is empty")
            var kept: File? = null
            if (saves.exists()) {
                kept = File(parent, "saves.before-restore-" + stamp())
                if (!saves.renameTo(kept)) throw IllegalStateException("could not move the current saves aside")
            }
            if (!staging.renameTo(saves)) {
                kept?.renameTo(saves)   // put the player's own saves back rather than leave none
                throw IllegalStateException("could not put the restored saves in place")
            }
            return count
        } finally {
            staging.deleteRecursively()
        }
    }

    /** The automatic backup before a match; quiet, and only when there is something to keep. */
    fun automatic(context: Context, prefs: Prefs) {
        val saves = savesDir(context, prefs)
        if (packages(saves).isEmpty()) return
        val dir = automaticDir(context)
        dir.mkdirs()
        val file = File(dir, "auto-${stamp()}.zip")
        try {
            file.outputStream().use { write(saves, it) }
        } catch (_: Throwable) {
            file.delete()
            return
        }
        automaticBackups(context).drop(KEEP_AUTOMATIC).forEach { it.delete() }
    }

    /** The automatic backups, newest first. */
    fun automaticBackups(context: Context): List<File> =
        automaticDir(context).listFiles { f -> f.isFile && f.name.endsWith(".zip") }
            ?.sortedByDescending { it.name } ?: emptyList()

    /** The file name offered for a backup the player saves. */
    fun suggestedName(): String = "pes2017-saves-${stamp()}.zip"

    private fun stamp(): String = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
}
