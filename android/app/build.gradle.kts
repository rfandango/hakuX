import java.util.Properties
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
  id("com.android.application")
  id("org.jetbrains.kotlin.android")
  id("org.jetbrains.kotlin.plugin.compose")
}

val keystorePropertiesFile = rootProject.file("key.properties")
val keystoreProperties = Properties()
val hasKeystoreProperties = keystorePropertiesFile.exists()

if (hasKeystoreProperties) {
  keystorePropertiesFile.inputStream().use { keystoreProperties.load(it) }
}

val hasReleaseKeystore = hasKeystoreProperties &&
  listOf("storeFile", "storePassword", "keyAlias", "keyPassword").all {
    !keystoreProperties.getProperty(it).isNullOrBlank()
  }

android {
  namespace = "com.rfandango.haku_x"
  compileSdk = 36
  buildToolsVersion = "36.1.0"
  ndkVersion = "29.0.14206865"

  defaultConfig {
    applicationId = "com.rfandango.haku_x"
    minSdk = 28
    targetSdk = 36

    versionCode = 9
    versionName = "1.3.0"

    ndk {
      abiFilters += listOf("arm64-v8a")
    }
    // The native core (libhakuxcore.so, libSDL3.so) comes prebuilt in
    // src/main/jniLibs from native/ (tools/native/app-libs.sh); the app
    // builds no native code of its own.

  }

  signingConfigs {
    if (hasReleaseKeystore) {
      create("release") {
        storeFile = file(keystoreProperties.getProperty("storeFile"))
        storePassword = keystoreProperties.getProperty("storePassword")
        keyAlias = keystoreProperties.getProperty("keyAlias")
        keyPassword = keystoreProperties.getProperty("keyPassword")
      }
    }
  }

  buildTypes {
    debug {
      applicationIdSuffix = ".debug"
      resValue("string", "app_name", "hakuX Debug")
      ndk {
        debugSymbolLevel = "NONE"
      }
      // A debuggable app is never compiled ahead of time: its Kotlin runs in
      // ART's interpreter until the JIT picks it up. Unshrunk (28 MB of dex),
      // the interpreter's method and field caches kept missing, and the
      // library stuttered on fast scrolls. R8 shrinks and optimizes it as in
      // release; names are kept (proguard-debug.pro).
      isMinifyEnabled = true
      proguardFiles(
        getDefaultProguardFile("proguard-android-optimize.txt"),
        "proguard-rules.pro",
        "proguard-debug.pro"
      )
    }
    release {
      resValue("string", "app_name", "hakuX")
      isMinifyEnabled = true
      proguardFiles(
        getDefaultProguardFile("proguard-android-optimize.txt"),
        "proguard-rules.pro"
      )
      if (hasReleaseKeystore) {
        signingConfig = signingConfigs.getByName("release")
      }
    }
  }


  packaging {
    resources.excludes += setOf(
      "**/*.md",
      "META-INF/LICENSE*",
      "META-INF/NOTICE*"
    )
    jniLibs.useLegacyPackaging = true
    jniLibs.keepDebugSymbols += setOf("**/*.so")
  }

  compileOptions {
    sourceCompatibility = JavaVersion.VERSION_21
    targetCompatibility = JavaVersion.VERSION_21
  }

  buildFeatures {
    compose = true
    buildConfig = true    // BuildConfig.DEBUG: what only debug builds show (the hitch line)
  }

  // The repository's mods/ folder as assets/mods (syncModAssets below).
  sourceSets.getByName("main").assets.srcDir(layout.buildDirectory.dir("generated/modAssets").get().asFile)
}

// The repository's mods/ folder (mods and cheats per game: mods/README.md),
// packed as the APK's assets/mods for the library's Mods & cheats; its
// README stays out.
val syncModAssets = tasks.register<Sync>("syncModAssets") {
  from(rootProject.file("../mods")) { exclude("README.md") }
  into(layout.buildDirectory.dir("generated/modAssets/mods"))
}
tasks.named("preBuild") { dependsOn(syncModAssets) }

dependencies {
  implementation("androidx.core:core-ktx:1.15.0")
  implementation("androidx.documentfile:documentfile:1.0.1")

  // The UI: Jetpack Compose with Material 3.
  implementation(platform("androidx.compose:compose-bom:2025.06.00"))
  implementation("androidx.compose.ui:ui")
  implementation("androidx.compose.ui:ui-tooling-preview")
  implementation("androidx.compose.material3:material3")
  implementation("androidx.compose.material:material-icons-core")
  implementation("androidx.activity:activity-compose:1.10.1")
  implementation("androidx.navigation:navigation-compose:2.9.0")
  implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.9.1")
  implementation("androidx.lifecycle:lifecycle-runtime-compose:2.9.1")
  debugImplementation("androidx.compose.ui:ui-tooling")

  // JVM unit tests (the hard disk image formats: data/hdd)
  testImplementation("junit:junit:4.13.2")
}

kotlin {
  compilerOptions {
    jvmTarget.set(JvmTarget.JVM_21)
  }
}
