# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.kts.

# Keep Retrofit interfaces
-keep,allowobfuscation interface * {
    @retrofit2.http.* <methods>;
}

# Keep Gson serialized models
-keepclassmembers class com.friendorfoe.data.remote.** {
    <fields>;
}

# Keep Room entities
-keep class com.friendorfoe.data.local.** { *; }

# Keep domain models (used by Gson)
-keep class com.friendorfoe.domain.model.** { *; }
