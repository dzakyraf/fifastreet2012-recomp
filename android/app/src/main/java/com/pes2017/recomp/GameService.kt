package com.pes2017.recomp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

/**
 * Holds the run's process above the low-memory killer for as long as the game is
 * up.
 *
 * A run is a process holding the guest's 512 MB bank, the upload arena and the
 * texture cache -- by far the largest thing on the phone. When the player steps
 * out to a message the activity stops, the process drops to the cached bucket,
 * and from there Android may freeze it, and the low-memory killer may take it,
 * which costs the whole load again. A foreground service is the only thing an
 * application can hold that says "this is still wanted" about a process nobody
 * is looking at.
 *
 * What it deliberately does not do is keep the game *playing* while it is away:
 * the activity still pauses the guest when it goes off screen
 * (`nativeSetPaused`). This keeps the process alive to be resumed quickly, and
 * out of the killer's queue while the game is the thing in front. The other
 * half of the story is runtime/android/perf.cpp, which is what gives memory back
 * when the system asks for it anyway -- a service raises the priority, it does
 * not license holding half a gigabyte through a phone call.
 *
 * It lives in ":game" beside the activity, so the run is one process and one
 * notification rather than a second copy of a half-gigabyte runtime.
 */
class GameService : Service() {

    private lateinit var notification: Notification

    /** Nothing binds to it; the run is reached through the app's own activity. */
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()

        // IMPORTANCE_LOW: a line in the shade, no sound and no heads-up. The
        // notification is a reason for the process to exist, not news.
        val channel = NotificationChannel(
            CHANNEL,
            getString(R.string.run_channel),
            NotificationManager.IMPORTANCE_LOW,
        )
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
            .createNotificationChannel(channel)

        // Tapping it goes back to the game rather than starting a second one:
        // GameActivity is singleTask.
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, GameActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        notification = Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(getString(R.string.run_working))
            .setContentIntent(open)
            .setOngoing(true)
            .setShowWhen(false)
            .build()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Called here rather than in onCreate, and on every start, because the
        // five seconds Android allows between startForegroundService and
        // startForeground are counted again for each request; starting twice
        // (a second run after the first ended) is harmless this way and an
        // Application Not Responding otherwise.
        //
        // Android 14 takes a foreground service only for a type the manifest
        // declares, and names it here: a game holding a session open is
        // "special use".
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }

        // Not sticky: there is nothing here without a run behind it, and a
        // service restarted by the system would hold a notification open over a
        // dead game.
        return START_NOT_STICKY
    }

    companion object {
        private const val CHANNEL = "pes17-run"
        private const val NOTIFICATION_ID = 0x4D57

        /** Once the run has started. The caller is in ":game" as well. */
        fun start(context: Context) {
            context.startForegroundService(Intent(context, GameService::class.java))
        }

        /** The run is over, so the notification goes with it. */
        fun stop(context: Context) {
            context.stopService(Intent(context, GameService::class.java))
        }
    }
}
