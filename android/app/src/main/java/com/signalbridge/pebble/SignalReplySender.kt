package com.signalbridge.pebble

import android.app.RemoteInput
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.service.notification.StatusBarNotification
import android.util.Log

/**
 * Injects a reply text into an active Signal notification using
 * Android's RemoteInput API — the same mechanism Android Auto uses.
 *
 * We keep a cache of the most recent reply action per conversation ID
 * so that the watch can trigger a reply after the notification has scrolled.
 */
object SignalReplySender {

    private const val TAG = "SignalReplySender"
    private const val SIGNAL_PKG = "org.thoughtcrime.securesms"

    // convId → (replyAction, RemoteInput)
    private val replyCache =
        mutableMapOf<String, Pair<android.app.Notification.Action, RemoteInput>>()

    // ----------------------------------------------------------------
    // Called by SignalNotificationListener to cache reply actions
    // ----------------------------------------------------------------

    fun cacheReplyAction(convId: String, sbn: StatusBarNotification) {
        val actions = sbn.notification.actions ?: return
        for (action in actions) {
            val remoteInputs = action.remoteInputs ?: continue
            if (remoteInputs.isEmpty()) continue
            // Signal's reply action has a RemoteInput with a non-empty result key
            val ri = remoteInputs.firstOrNull() ?: continue
            replyCache[convId] = Pair(action, ri)
            Log.d(TAG, "Cached reply action for $convId")
            return
        }
    }

    fun clearCache() = replyCache.clear()

    // ----------------------------------------------------------------
    // Called by PebbleCommunicator when a reply arrives from the watch
    // ----------------------------------------------------------------

    fun sendReply(context: Context, convId: String, text: String) {
        val (action, remoteInput) = replyCache[convId] ?: run {
            Log.w(TAG, "No cached reply action for convId=$convId")
            return
        }
        if (text.isBlank()) return

        try {
            val intent = Intent()
            val bundle = Bundle()
            bundle.putCharSequence(remoteInput.resultKey, text)
            RemoteInput.addResultsToIntent(
                arrayOf(remoteInput), intent, bundle
            )
            action.actionIntent.send(context, 0, intent)
            Log.i(TAG, "Sent reply to Signal: $text")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to send reply to Signal", e)
        }
    }
}
