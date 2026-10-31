plugins {
    id("com.android.application")
}

android {
    namespace = "dev.wristctl"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "dev.wristctl"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }

    externalNativeBuild {
        cmake {
            // The app is the same WatchView the desktop builds; the
            // Android-only sources are the entry point and the JNI BLE
            // transport. android/CMakeLists.txt sets C11 for the core and
            // C++17 for the framework.
            path = file("../CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
}

// android/CMakeLists.txt's vce_compile_slang() writes .spv files directly into
// app/src/main/assets/shaders as a side effect of the native build. AGP's asset
// merge only tracks declared source-set inputs, so without this the merge can
// run (and be cached) before CMake produces anything. Same ordering trap
// Matrix Player documents.
afterEvaluate {
    val nativeBuildTasks = tasks.matching { it.name.startsWith("buildCMake") }
    tasks.matching { it.name.startsWith("merge") && it.name.endsWith("Assets") }
        .configureEach {
            dependsOn(nativeBuildTasks)
        }
}
