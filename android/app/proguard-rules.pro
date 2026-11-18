# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.kts.

# Keep Retrofit interfaces
-keep,allowobfuscation interface * {
    @retrofit2.http.* <methods>;
}

# Keep Gson/Retrofit response models (class + fields + constructors)
# Must use -keep (not -keepclassmembers) so R8 retains classes that
# Retrofit creates via reflection/generics
-keep class com.friendorfoe.data.remote.** {
    <fields>;
    <init>(...);
}

# Keep Room entities
-keep class com.friendorfoe.data.local.** { *; }

# Keep domain models (used by Gson)
-keep class com.friendorfoe.domain.model.** { *; }

# Nearby identifiers, coordinates and endpoint errors can enter diagnostic messages.
# Release and Play builds keep functional error handling without writing those messages to Logcat.
-assumenosideeffects class android.util.Log {
    public static int v(...);
    public static int d(...);
    public static int i(...);
    public static int w(...);
    public static int e(...);
}
