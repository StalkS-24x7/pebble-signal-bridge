package com.signalbridge.pebble

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.Base64

/**
 * Scales a Bitmap to IMG_SIDE×IMG_SIDE and quantises each pixel to the
 * Pebble 64-colour palette (2 bits per RGB channel, stored as GColor8:
 * 0bRRGGBBAA where AA is always 0b11 = fully opaque).
 *
 * Returns the raw pixel bytes, split into base64-encoded chunks suitable
 * for AppMessage transmission.
 */
object ImageProcessor {

    const val IMG_SIDE    = 120  // target dimensions — must match main.c IMG_SIDE
    const val CHUNK_BYTES = 500  // raw bytes per chunk — must match main.c CHUNK_BYTES

    fun processAndChunk(src: Bitmap): List<String> {
        val scaled = scaleBitmap(src, IMG_SIDE, IMG_SIDE)
        val raw    = quantiseToPebblePalette(scaled)
        scaled.recycle()
        return chunkBase64(raw)
    }

    // ----------------------------------------------------------------
    // Scale preserving aspect ratio, padding with black if needed
    // ----------------------------------------------------------------
    private fun scaleBitmap(src: Bitmap, w: Int, h: Int): Bitmap {
        val out    = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(out)
        canvas.drawColor(Color.BLACK)

        val srcW   = src.width.toFloat()
        val srcH   = src.height.toFloat()
        val scale  = minOf(w / srcW, h / srcH)
        val dstW   = (srcW * scale).toInt()
        val dstH   = (srcH * scale).toInt()
        val left   = (w - dstW) / 2f
        val top    = (h - dstH) / 2f

        val scaled = Bitmap.createScaledBitmap(src, dstW, dstH, true)
        canvas.drawBitmap(scaled, left, top, Paint())
        scaled.recycle()
        return out
    }

    // ----------------------------------------------------------------
    // Map each pixel to the nearest Pebble GColor8
    // GColor8 = 0bRRGGBBAA  (2 bits each, AA = 0b11 = opaque)
    // ----------------------------------------------------------------
    private fun quantiseToPebblePalette(bmp: Bitmap): ByteArray {
        val pixels = IntArray(IMG_SIDE * IMG_SIDE)
        bmp.getPixels(pixels, 0, IMG_SIDE, 0, 0, IMG_SIDE, IMG_SIDE)
        return ByteArray(pixels.size) { i ->
            val argb = pixels[i]
            val r2   = (Color.red(argb)   ushr 6) and 0x3
            val g2   = (Color.green(argb) ushr 6) and 0x3
            val b2   = (Color.blue(argb)  ushr 6) and 0x3
            ((r2 shl 6) or (g2 shl 4) or (b2 shl 2) or 0x3).toByte()
        }
    }

    // ----------------------------------------------------------------
    // Split raw bytes into base64 chunks
    // ----------------------------------------------------------------
    private fun chunkBase64(raw: ByteArray): List<String> {
        val chunks = mutableListOf<String>()
        var offset = 0
        while (offset < raw.size) {
            val end   = minOf(offset + CHUNK_BYTES, raw.size)
            val slice = raw.copyOfRange(offset, end)
            chunks.add(Base64.encodeToString(slice, Base64.NO_WRAP))
            offset = end
        }
        return chunks
    }
}
