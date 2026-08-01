package com.friendorfoe.data.repository

interface RuntimePermissionChangeNotifier {
    fun onRuntimePermissionsChanged()

    companion object {
        val NoOp: RuntimePermissionChangeNotifier = object : RuntimePermissionChangeNotifier {
            override fun onRuntimePermissionsChanged() = Unit
        }
    }
}
