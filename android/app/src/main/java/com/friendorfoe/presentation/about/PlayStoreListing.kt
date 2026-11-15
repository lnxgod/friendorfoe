package com.friendorfoe.presentation.about

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import com.friendorfoe.BuildConfig

internal fun Context.openPlayStoreListing() {
    val listing = "https://play.google.com/store/apps/details?id=" + BuildConfig.APPLICATION_ID
    try {
        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(listing)).setPackage("com.android.vending"))
    } catch (_: ActivityNotFoundException) {
        try {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(listing)))
        } catch (_: ActivityNotFoundException) {
            Toast.makeText(this, "Install Google Play or a browser to open the listing.", Toast.LENGTH_LONG).show()
        }
    }
}
