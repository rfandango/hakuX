#include <jni.h>

#include <array>
#include <string>

extern "C" int xiso_convert_iso_to_xiso(const char *input_path,
                                          const char *output_path,
                                          char *err_buf,
                                          size_t err_buf_len);

extern "C" JNIEXPORT jstring JNICALL
Java_com_rfandango_xemuandroid_XisoConverterNative_nativeConvertIsoToXiso(
    JNIEnv *env,
    jclass,
    jstring input_path,
    jstring output_path) {
  if (input_path == nullptr || output_path == nullptr) {
    return env->NewStringUTF("Input/output path is missing");
  }

  const char *input_chars = env->GetStringUTFChars(input_path, nullptr);
  if (input_chars == nullptr) {
    return env->NewStringUTF("Failed to read input path");
  }
  const char *output_chars = env->GetStringUTFChars(output_path, nullptr);
  if (output_chars == nullptr) {
    env->ReleaseStringUTFChars(input_path, input_chars);
    return env->NewStringUTF("Failed to read output path");
  }

  std::array<char, 4096> error_buffer{};
  int rc = xiso_convert_iso_to_xiso(
      input_chars,
      output_chars,
      error_buffer.data(),
      error_buffer.size());

  env->ReleaseStringUTFChars(input_path, input_chars);
  env->ReleaseStringUTFChars(output_path, output_chars);

  if (rc == 0) {
    return nullptr;
  }

  const char *msg = error_buffer[0] != '\0'
                        ? error_buffer.data()
                        : "ISO conversion failed";
  return env->NewStringUTF(msg);
}