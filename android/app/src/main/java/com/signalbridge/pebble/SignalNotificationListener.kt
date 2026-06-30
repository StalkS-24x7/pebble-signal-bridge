package com.signalbridge.pebble

import android.app.Notification
import android.graphics.Bitmap
import android.graphics.drawable.Icon
import android.os.Build
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log

/**
 * Listens for Signal notifications and forwards them to the Pebble watch
 * via PebbleCommunicator.
 *
 * Conversation ID is derived from the notification tag/key because Signal
 * does not expose an explicit thread ID in its notification extras.
 */
class SignalNotificationListener : NotificationListenerService() {

    private val TAG = "SignalListener"
    private val SIGNAL_PKG = "org.thoughtcrime.securesms"

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        if (sbn.packageName != SIGNAL_PKG) return
        val n       = sbn.notification
        val extras  = n.extras ?: return

        // Filter out non-message notifications (call prompts, etc.)
        val style = extras.getString(Notification.EXTRA_TEMPLATE) ?: ""
        if (style.contains("CallStyle")) return

        val sender  = extras.getCharSequence(Notification.EXTRA_TITLE)?.toString() ?: return
        val body    = (extras.getCharSequence(Notification.EXTRA_BIG_TEXT)
                    ?: extras.getCharSequence(Notification.EXTRA_TEXT))?.toString() ?: ""

        // Use notification tag as a stable per-conversation ID
        val convId  = sbn.tag ?: sbn.key

        Log.i(TAG, "Signal notification: sender=$sender convId=$convId")

        // Cache the RemoteInput reply action before forwarding
        SignalReplySender.cacheReplyAction(convId, sbn)

        // Extract image attachment (BigPicture style)
        val image: Bitmap? = extractImage(extras)

        PebbleCommunicator.sendMessage(
            context  = applicationContext,
            sender   = sender,
            body     = body,
            convId   = convId,
            image    = image,
        )
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        // Nothing to do — reply actions remain cached until app restart
    }

    // ----------------------------------------------------------------
    // Extract the BigPicture bitmap from notification extras
    // Returns null if not present or not decodeable
    // ----------------------------------------------------------------

    private fun extractImage(extras: android.os.Bundle): Bitmap? {
        // Android 12+ may have the picture as an Icon rather than Bitmap
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val icon = extras.getParcelable(
                Notification.EXTRA_PICTURE, Icon::class.java
            )
            if (icon != null) {
                return try {
                    icon.loadDrawable(applicationContext)?.let { d ->
                        val bmp = Bitmap.createBitmap(
                            d.intrinsicWidth.coerceAtLeast(1),
                            d.intrinsicHeight.coerceAtLeast(1),
                            Bitmap.Config.ARGB_8888
                        )
                        val canvas = android.graphics.Canvas(bmp)
                        d.setBounds(0, 0, canvas.width, canvas.height)
                        d.draw(canvas)
                        bmp
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "Could not load picture Icon", e)
                    null
                }
            }
        }

        @Suppress("DEPRECATION")
        val bmp = extras.getParcelable<Bitmap>(Notification.EXTRA_PICTURE)
        return bmp
    }
}
