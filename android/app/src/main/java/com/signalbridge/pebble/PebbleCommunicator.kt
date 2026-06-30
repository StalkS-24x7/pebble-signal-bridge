package com.signalbridge.pebble

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID
import java.util.concurrent.atomic.AtomicInteger

/**
 * Sends AppMessages to the watch via Gadgetbridge's PebbleKit-compatible
 * broadcast intent API, and receives replies from the watch.
 *
 * Key layout must match watch-app/src/main.c and package.json appKeys.
 */
object PebbleCommunicator {

    private const val TAG = "PebbleCommunicator"

    // Must match package.json uuid field AND main.c APP_UUID comment
    val APP_UUID: UUID = UUID.fromString("a8e3c7f2-1d4b-4e8a-9c3d-5f7b2e1a6c4d")

    // Gadgetbridge / PebbleKit intent actions
    private const val ACTION_SEND    = "com.getpebble.android.kit.SEND_DATA"
    private const val ACTION_RECEIVE = "com.getpebble.android.kit.RECEIVE_DATA"

    // Protocol keys (mirror of AppMsgKey enum in main.c)
    private const val KEY_MSG_TYPE    = 0
    private const val KEY_SENDER      = 1
    private const val KEY_BODY        = 2
    private const val KEY_CONV_ID     = 3
    private const val KEY_HAS_IMAGE   = 4
    private const val KEY_CHUNK_INDEX = 5
    private const val KEY_CHUNK_TOTAL = 6
    private const val KEY_CHUNK_DATA  = 7
    private const val KEY_IMG_WIDTH   = 8
    private const val KEY_IMG_HEIGHT  = 9
    private const val KEY_REPLY_TEXT  = 10

    private const val MSG_NEW_MESSAGE = 1
    private const val MSG_IMAGE_CHUNK = 2

    private val txId = AtomicInteger(0)

    // ----------------------------------------------------------------
    // Public API
    // ----------------------------------------------------------------

    /**
     * Send a new Signal message notification to the watch.
     * If [image] is non-null, image chunks are sent immediately after.
     */
    fun sendMessage(
        context: Context,
        sender: String,
        body: String,
        convId: String,
        image: Bitmap? = null,
    ) {
        val dict = JSONObject().apply {
            put(KEY_MSG_TYPE.toString(),  MSG_NEW_MESSAGE)
            put(KEY_SENDER.toString(),    sender.take(47))
            put(KEY_BODY.toString(),      body.take(255))
            put(KEY_CONV_ID.toString(),   convId.take(63))
            put(KEY_HAS_IMAGE.toString(), if (image != null) 1 else 0)
        }
        sendDict(context, dict)

        if (image != null) sendImage(context, image)
    }

    // ----------------------------------------------------------------
    // Image — chunk and send
    // ----------------------------------------------------------------

    private fun sendImage(context: Context, bitmap: Bitmap) {
        val chunks = ImageProcessor.processAndChunk(bitmap)
        val total  = chunks.size
        Log.d(TAG, "Sending image in $total chunk(s)")

        chunks.forEachIndexed { idx, b64 ->
            val dict = JSONObject().apply {
                put(KEY_MSG_TYPE.toString(),    MSG_IMAGE_CHUNK)
                put(KEY_CHUNK_INDEX.toString(), idx)
                put(KEY_CHUNK_TOTAL.toString(), total)
                put(KEY_CHUNK_DATA.toString(),  b64)
                if (idx == 0) {
                    put(KEY_IMG_WIDTH.toString(),  ImageProcessor.IMG_SIDE)
                    put(KEY_IMG_HEIGHT.toString(), ImageProcessor.IMG_SIDE)
                }
            }
            sendDict(context, dict)
            // Small back-off so Gadgetbridge's queue doesn't overflow
            Thread.sleep(80)
        }
    }

    // ----------------------------------------------------------------
    // Receive reply from watch (called by PebbleReceiver)
    // ----------------------------------------------------------------

    fun onWatchMessage(context: Context, payload: String) {
        try {
            val arr     = JSONArray(payload)
            val dict    = arr.getJSONObject(0)
            val msgType = dict.optInt(KEY_MSG_TYPE.toString(), -1)
            if (msgType != 3 /* MSG_REPLY */) return

            val replyText = dict.optString(KEY_REPLY_TEXT.toString())
            val convId    = dict.optString(KEY_CONV_ID.toString())
            Log.i(TAG, "Watch reply to $convId: $replyText")

            SignalReplySender.sendReply(context, convId, replyText)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to parse watch message", e)
        }
    }

    // ----------------------------------------------------------------
    // Low-level send via Gadgetbridge broadcast
    // ----------------------------------------------------------------

    private fun sendDict(context: Context, dict: JSONObject) {
        val payload = JSONArray().put(dict)
        val intent  = Intent(ACTION_SEND).apply {
            putExtra("uuid",           APP_UUID.toString())
            putExtra("msg_data",       payload.toString())
            putExtra("transaction_id", txId.getAndIncrement())
        }
        context.sendBroadcast(intent)
        Log.v(TAG, "→ Pebble: ${dict.optInt(KEY_MSG_TYPE.toString())}")
    }
}
