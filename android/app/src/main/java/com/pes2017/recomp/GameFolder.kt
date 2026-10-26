package com.pes2017.recomp

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.Settings
import androidx.core.content.ContextCompat
import java.io.File

/**
 * Where the player's own PES 2017 files are, and whether the app may read them.
 *
 * The runtime opens the game by path, so a folder chosen through the system
 * picker (a tree of documents) is only usable in place when it can be turned
 * back into a path and the app is allowed to read that path. On Android 11
 * and later that is the "All files access" special permission; before that,
 * the ordinary storage permission.
 */
object GameFolder {

    const val XEX = "default.xex"

    /** Whether paths outside the app's own folders can be read (and written, for saves). */
    fun hasAccess(context: Context): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) Environment.isExternalStorageManager()
        else ContextCompat.checkSelfPermission(context, Manifest.permission.READ_EXTERNAL_STORAGE) ==
            PackageManager.PERMISSION_GRANTED

    /** The system screen where the player allows it, or null before Android 11 (a prompt instead). */
    fun accessIntent(context: Context): Intent? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:${context.packageName}"))
        } else null

    val legacyPermissions: Array<String> = arrayOf(
        Manifest.permission.READ_EXTERNAL_STORAGE,
        Manifest.permission.WRITE_EXTERNAL_STORAGE,
    )

    /**
     * The real path behind a picked tree, or null when it is not on storage a
     * path can name (a cloud provider, say). "primary:Download/PES" is
     * /storage/emulated/0/Download/PES; "1A2B-3C4D:PES" is /storage/1A2B-3C4D/PES.
     */
    fun pathOf(tree: Uri): File? = try {
        val id = DocumentsContract.getTreeDocumentId(tree)
        val volume = id.substringBefore(':')
        val relative = id.substringAfter(':', "")
        val base = if (volume.equals("primary", ignoreCase = true)) Environment.getExternalStorageDirectory()
        else File("/storage/$volume").takeIf { volume.isNotEmpty() && volume != id }
        if (base == null) null else if (relative.isEmpty()) base else File(base, relative)
    } catch (_: Throwable) {
        null
    }

    /**
     * Looks for the game without being told, the app's own storage first (it
     * needs no permission and is where an install lands), then the usual
     * places a player puts an extracted copy -- only those the app may read.
     * Null means the player has to choose the folder.
     */
    fun autoDetect(context: Context): File? {
        val own = context.getExternalFilesDir(null)
        val candidates = linkedSetOf<File>()
        if (own != null) {
            candidates += File(own, "game")
            candidates += own
            candidates += File(own, "import")
        }
        candidates += File(context.filesDir, "game")
        val names = listOf("PES2017", "PES 2017", "PES17", "pes2017", "pes17", "PES_2017", "Pro Evolution Soccer 2017")
        if (hasAccess(context)) {
            val roots = listOf(
                Environment.getExternalStorageDirectory(),
                File(Environment.getExternalStorageDirectory(), "Download"),
                File(Environment.getExternalStorageDirectory(), "Games"),
                File(Environment.getExternalStorageDirectory(), "Android/obb"),
            )
            for (root in roots) for (name in names) candidates += File(root, name)
            // A phone's SD card, if there is one.
            File("/storage").listFiles()?.filter { it.name != "emulated" && it.name != "self" }?.forEach { card ->
                for (name in names) candidates += File(card, name)
            }
        }
        for (candidate in candidates) {
            try {
                if (candidate.isDirectory) locate(candidate)?.let { return it }
            } catch (_: Throwable) {
            }
        }
        return null
    }

    /** The folder holding default.xex: this one, or one directly inside it. */
    fun locate(folder: File): File? {
        if (File(folder, XEX).isFile) return folder
        return folder.listFiles()?.firstOrNull { it.isDirectory && File(it, XEX).isFile }
    }

    fun sha256(file: File): String {
        val digest = java.security.MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(1 shl 20)
            while (true) {
                val n = input.read(buffer)
                if (n <= 0) break
                digest.update(buffer, 0, n)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }
}
