package com.rfandango.xemuandroid

object XisoConverterNative {
  private val isLibraryLoaded: Boolean = try {
    System.loadLibrary("xiso_converter")
    true
  } catch (_: UnsatisfiedLinkError) {
    false
  }

  @JvmStatic
  private external fun nativeConvertIsoToXiso(inputPath: String, outputPath: String): String?

  fun convertIsoToXiso(inputPath: String, outputPath: String): String? {
    if (!isLibraryLoaded) {
      return "ISO converter native library is unavailable"
    }
    return nativeConvertIsoToXiso(inputPath, outputPath)
  }

  fun isAvailable(): Boolean = isLibraryLoaded
}
