package com.signalbridge.pebble

import android.content.ComponentName
import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import android.text.TextUtils
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

/**
 * Minimal launcher activity — shows service status and guides the user
 * through granting notification access permission.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var tvStatus: TextView
    private lateinit var tvPebble: TextView
    private lateinit var btnGrant: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvStatus = findViewById(R.id.tv_status)
        tvPebble = findViewById(R.id.tv_pebble)
        btnGrant = findViewById(R.id.btn_grant)

        btnGrant.setOnClickListener {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }
    }

    override fun onResume() {
        super.onResume()
        refreshStatus()
    }

    private fun refreshStatus() {
        val granted = isNotificationListenerEnabled()
        tvStatus.text = if (granted)
            "Notification access: GRANTED\nListening for Signal messages..."
        else
            "Notification access: NOT GRANTED\nTap the button below."
        btnGrant.isEnabled = !granted

        tvPebble.text = "Watch UUID:\n${PebbleCommunicator.APP_UUID}\n\n" +
            "Install this app UUID in your Pebble watch app.\n" +
            "Ensure Gadgetbridge is installed and connected."
    }

    private fun isNotificationListenerEnabled(): Boolean {
        val flat = Settings.Secure.getString(
            contentResolver,
            "enabled_notification_listeners"
        ) ?: return false
        val cn = ComponentName(this, SignalNotificationListener::class.java)
        return flat.split(":").any {
            ComponentName.unflattenFromString(it) == cn
        }
    }
}
