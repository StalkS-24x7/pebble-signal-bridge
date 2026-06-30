package com.signalbridge.pebble

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * Receives AppMessage data sent FROM the watch to the phone via Gadgetbridge.
 *
 * Gadgetbridge broadcasts com.getpebble.android.kit.RECEIVE_DATA when the
 * watch app calls app_message_outbox_send().  The UUID in the intent must
 * match our app's UUID so Gadgetbridge routes it to us.
 */
class PebbleReceiver : BroadcastReceiver() {

    private val TAG = "PebbleReceiver"

    override fun onReceive(context: Context, intent: Intent) {
        val uuid    = intent.getStringExtra("uuid") ?: return
        val payload = intent.getStringExtra("msg_data") ?: return

        if (!uuid.equals(PebbleCommunicator.APP_UUID.toString(), ignoreCase = true)) return

        Log.d(TAG, "← Watch: $payload")
        PebbleCommunicator.onWatchMessage(context, payload)
    }
}
