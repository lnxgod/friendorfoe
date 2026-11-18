# Android native library compatibility

For the 64-bit ABIs shipped by Friend or Foe, [Android's 16 KB page-size guidance](https://developer.android.com/guide/practices/page-sizes) requires compatible native ELF load segments **and** compatible packaging of uncompressed libraries. Repackaging a 4 KiB ELF alone does not fix it. These are build checks, not proof of operation on a 16 KiB device or Play acceptance.

The recorder-first debug build contained 4 KiB `PT_LOAD` segments in both ARCore libraries (`libarcore_sdk_{c,jni}.so`) and CameraX's `libimage_processing_util_jni.so` on arm64-v8a and x86_64. The [ARCore 1.50.0 AAR](https://dl.google.com/dl/android/maven2/com/google/ar/core/1.50.0/core-1.50.0.aar) and [CameraX 1.4.2 AAR](https://dl.google.com/dl/android/maven2/androidx/camera/camera-core/1.4.2/camera-core-1.4.2.aar) ship aligned replacements on both ABIs. The existing ML Kit pipeline and DataStore JNI libraries were already aligned.

On the actual release candidate:

1. `python3 scripts/check_android_native_alignment.py app-release.aab` checks every arm64-v8a/x86_64 ELF in every module, including transitive dependencies. Changing dependency versions can change the bundled libraries.
2. `bundletool dump config --bundle=app-release.aab` should report `PAGE_ALIGNMENT_16K`. For a generated APK run `zipalign -c -P 16 -v 4 app-release.apk` (Build Tools 35+). AAB file offsets themselves do not describe APK ZIP alignment.
3. On a 16 KiB Android device or emulator, confirm `adb shell getconf PAGE_SIZE` returns `16384`, then install generated APKs and exercise camera preview, AR where supported, image analysis and preference writes. A successful ELF check cannot test vendor services or reveal hard-coded page-size assumptions at runtime.
