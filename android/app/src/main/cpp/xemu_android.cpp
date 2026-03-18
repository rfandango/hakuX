#include <SDL.h>
#include <SDL_main.h>
#include <SDL_system.h>

#include <GLES3/gl3.h>
#include <toml++/toml.h>

#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>

#include <climits>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <unistd.h>

#include "xemu-settings.h"
#include "hw/xbox/nv2a/debug.h"

extern "C" void xemu_set_fp_safe(bool enable);
extern "C" bool xemu_get_fp_safe(void);
extern "C" void xemu_set_fast_fences(bool enable);
extern "C" bool xemu_get_fast_fences(void);
extern "C" void xemu_set_fp_jit(bool enable);
extern "C" bool xemu_get_fp_jit(void);
extern "C" void xemu_set_draw_reorder(bool enable);
extern "C" bool xemu_get_draw_reorder(void);
extern "C" void xemu_set_bindless_textures(bool enable);
extern "C" bool xemu_get_bindless_textures(void);
extern "C" void xemu_set_async_compile(bool enable);
extern "C" bool xemu_get_async_compile(void);
extern "C" void xemu_set_frame_skip(bool enable);
extern "C" bool xemu_get_frame_skip(void);
extern "C" void xemu_set_submit_frames(int count);
extern "C" int xemu_get_submit_frames(void);
extern "C" bool runstate_is_running(void);
extern "C" void xemu_android_toggle_pause(void);
extern "C" void xemu_android_request_exit(void);

#ifdef CONFIG_VULKAN
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <volk.h>

static void* g_custom_vulkan_library = nullptr;

extern "C" PFN_vkGetInstanceProcAddr xemu_android_get_vk_proc_addr(void)
{
    if (!g_custom_vulkan_library) {
        return nullptr;
    }
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(g_custom_vulkan_library, "vkGetInstanceProcAddr"));
}
#endif

static int g_dvd_fd = -1;

namespace {
constexpr const char* kLogTag = "xemu-android";
constexpr const char* kPrefsName = "x1box_prefs";

static JNIEnv* GetEnv();
static jobject GetActivity(JNIEnv* env);
static bool HasException(JNIEnv* env, const char* context);

static void LogInfo(const char* msg) {
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", msg);
}

static void LogInfoFmt(const char* fmt, const char* detail) {
  __android_log_print(ANDROID_LOG_INFO, kLogTag, fmt, detail);
}

static void LogInfoInt(const char* fmt, int value) {
  __android_log_print(ANDROID_LOG_INFO, kLogTag, fmt, value);
}

static void LogError(const char* msg) {
  __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", msg);
}

static void LogErrorInt(const char* fmt, int value) {
  __android_log_print(ANDROID_LOG_ERROR, kLogTag, fmt, value);
}

static void LogErrorFmt(const char* fmt, const char* detail) {
  __android_log_print(ANDROID_LOG_ERROR, kLogTag, fmt, detail);
}

static bool EnsureDirExists(const std::string& path) {
  if (path.empty()) return false;
  if (mkdir(path.c_str(), 0755) == 0) return true;
  return errno == EEXIST;
}

static bool FileExists(const std::string& path) {
  if (path.empty()) return false;
  struct stat st {};
  return stat(path.c_str(), &st) == 0;
}

static int64_t FileSize(const std::string& path) {
  if (path.empty()) return -1;
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return -1;
  return static_cast<int64_t>(st.st_size);
}

static void LogQcow2Info(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return;

  uint8_t hdr[72];
  if (fread(hdr, 1, sizeof(hdr), f) < sizeof(hdr)) {
    fclose(f);
    return;
  }

  uint32_t magic = (uint32_t)hdr[0] << 24 | hdr[1] << 16 | hdr[2] << 8 | hdr[3];
  if (magic != 0x514649fbu) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "QCOW2 check: %s is raw (magic=0x%08x)", path.c_str(), magic);
    fclose(f);
    return;
  }

  uint64_t backing_offset = 0;
  uint32_t backing_size = 0;
  for (int i = 0; i < 8; i++) backing_offset = (backing_offset << 8) | hdr[8 + i];
  for (int i = 0; i < 4; i++) backing_size   = (backing_size   << 8) | hdr[16 + i];

  uint64_t virtual_size = 0;
  for (int i = 0; i < 8; i++) virtual_size = (virtual_size << 8) | hdr[24 + i];

  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "QCOW2: %s  virtual_size=%" PRIu64 " (%.1f GB)  file_size=%" PRId64,
                      path.c_str(), virtual_size,
                      (double)virtual_size / (1024.0 * 1024.0 * 1024.0),
                      FileSize(path));

  if (backing_offset != 0 && backing_size != 0 && backing_size < 4096) {
    std::vector<char> backing(backing_size + 1, '\0');
    fseek(f, (long)backing_offset, SEEK_SET);
    fread(backing.data(), 1, backing_size, f);
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "QCOW2 WARNING: backing file = '%s'  -- reads of unmodified "
                        "sectors will FAIL if this file is missing!",
                        backing.data());
  }
  fclose(f);
}

static bool IsTcgTuningEnabled() {
  const char* value = SDL_getenv("XEMU_ANDROID_TCG_TUNING");
  return !(value && value[0] == '0');
}

static void LoadGameControllerMappingsFromAssets() {
  constexpr const char* kDbAssetName = "gamecontrollerdb.txt";

  JNIEnv* env = GetEnv();
  jobject activity = GetActivity(env);
  if (!env || !activity) {
    LogInfo("Controller mappings: JNI unavailable");
    return;
  }

  jclass activityClass = env->GetObjectClass(activity);
  jmethodID getAssets = env->GetMethodID(
      activityClass, "getAssets", "()Landroid/content/res/AssetManager;");
  if (!getAssets) {
    LogInfo("Controller mappings: Activity.getAssets() not found");
    return;
  }

  jobject assetManagerObj = env->CallObjectMethod(activity, getAssets);
  if (HasException(env, "Activity.getAssets") || !assetManagerObj) {
    LogInfo("Controller mappings: could not access AssetManager");
    return;
  }

  AAssetManager* assetManager = AAssetManager_fromJava(env, assetManagerObj);
  env->DeleteLocalRef(assetManagerObj);
  if (!assetManager) {
    LogInfo("Controller mappings: AssetManager bridge failed");
    return;
  }

  AAsset* asset = AAssetManager_open(assetManager, kDbAssetName, AASSET_MODE_STREAMING);
  if (!asset) {
    LogInfo("Controller mappings: no custom gamecontrollerdb.txt in assets");
    return;
  }

  const off_t length = AAsset_getLength(asset);
  if (length <= 0 || length > INT_MAX) {
    AAsset_close(asset);
    LogError("Controller mappings: invalid gamecontrollerdb.txt size");
    return;
  }

  std::vector<char> data(static_cast<size_t>(length));
  size_t total = 0;
  while (total < data.size()) {
    const int read = AAsset_read(asset, data.data() + total,
                                 static_cast<size_t>(data.size() - total));
    if (read <= 0) {
      break;
    }
    total += static_cast<size_t>(read);
  }
  AAsset_close(asset);

  if (total == 0) {
    LogError("Controller mappings: gamecontrollerdb.txt is empty");
    return;
  }
  data.resize(total);

  SDL_RWops* rw = SDL_RWFromConstMem(data.data(), static_cast<int>(data.size()));
  if (!rw) {
    LogErrorFmt("Controller mappings: SDL_RWFromConstMem failed: %s", SDL_GetError());
    return;
  }

  const int added = SDL_GameControllerAddMappingsFromRW(rw, 1);
  if (added < 0) {
    LogErrorFmt("Controller mappings: failed to parse gamecontrollerdb.txt: %s", SDL_GetError());
    return;
  }

  LogInfoInt("Controller mappings loaded from assets: %d", added);
}

static const char* GetTcgThreadFromEnv() {
  const char* value = SDL_getenv("XEMU_ANDROID_TCG_THREAD");
  if (value && strcmp(value, "single") == 0) {
    return "single";
  }
  return "multi";
}

static int GetTcgTbSizeFromEnv() {
  constexpr int kDefaultTbSize = 256;
  constexpr int kMinTbSize = 32;
  constexpr int kMaxTbSize = 512;

  const char* value = SDL_getenv("XEMU_ANDROID_TCG_TB_SIZE");
  if (!value || value[0] == '\0') {
    return kDefaultTbSize;
  }

  char* end = nullptr;
  long parsed = strtol(value, &end, 10);
  if (end == value || (end && *end != '\0')) {
    return kDefaultTbSize;
  }
  if (parsed < kMinTbSize) {
    parsed = kMinTbSize;
  } else if (parsed > kMaxTbSize) {
    parsed = kMaxTbSize;
  }
  return static_cast<int>(parsed);
}

static JNIEnv* GetEnv() {
  return static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
}

static jobject GetActivity(JNIEnv* env) {
  (void)env;
  return reinterpret_cast<jobject>(SDL_AndroidGetActivity());
}

static bool HasException(JNIEnv* env, const char* context) {
  if (!env->ExceptionCheck()) return false;
  env->ExceptionDescribe();
  env->ExceptionClear();
  LogErrorFmt("JNI exception in %s", context);
  return true;
}

static std::string JStringToString(JNIEnv* env, jstring value) {
  if (!value) return {};
  const char* utf = env->GetStringUTFChars(value, nullptr);
  if (!utf) return {};
  std::string out(utf);
  env->ReleaseStringUTFChars(value, utf);
  return out;
}

static bool HasInlineAioCrashFlag(const std::string& flag_path) {
  if (flag_path.empty()) {
    return false;
  }
  struct stat st {};
  return stat(flag_path.c_str(), &st) == 0;
}

static bool ShouldEnableInlineAioWorkaround(const std::string& crash_flag_path) {
  const char* forced = SDL_getenv("XEMU_ANDROID_INLINE_AIO");
  if (forced) {
    return forced[0] != '\0' && forced[0] != '0';
  }

  if (HasInlineAioCrashFlag(crash_flag_path)) {
    LogInfoFmt("Inline AIO enabled from crash marker: %s",
               crash_flag_path.c_str());
  }

  return true;
}

static std::string GetPrefString(JNIEnv* env, jobject activity, const char* key) {
  jclass activityClass = env->GetObjectClass(activity);
  jmethodID getPrefs = env->GetMethodID(activityClass, "getSharedPreferences",
                                        "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");
  if (!getPrefs) return {};
  jstring prefsName = env->NewStringUTF(kPrefsName);
  jobject prefs = env->CallObjectMethod(activity, getPrefs, prefsName, 0);
  env->DeleteLocalRef(prefsName);
  if (HasException(env, "getSharedPreferences") || !prefs) return {};

  jclass prefsClass = env->GetObjectClass(prefs);
  jmethodID getString = env->GetMethodID(
      prefsClass, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
  if (!getString) return {};

  jstring jkey = env->NewStringUTF(key);
  jstring jdefault = nullptr;
  jstring value = static_cast<jstring>(env->CallObjectMethod(prefs, getString, jkey, jdefault));
  env->DeleteLocalRef(jkey);
  if (HasException(env, "SharedPreferences.getString")) return {};

  std::string out = JStringToString(env, value);
  if (value) env->DeleteLocalRef(value);
  return out;
}

static int GetPrefInt(JNIEnv* env, jobject activity, const char* key, int defaultValue) {
  jclass activityClass = env->GetObjectClass(activity);
  jmethodID getPrefs = env->GetMethodID(activityClass, "getSharedPreferences",
                                        "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");
  if (!getPrefs) return defaultValue;
  jstring prefsName = env->NewStringUTF(kPrefsName);
  jobject prefs = env->CallObjectMethod(activity, getPrefs, prefsName, 0);
  env->DeleteLocalRef(prefsName);
  if (HasException(env, "getSharedPreferences") || !prefs) return defaultValue;

  jclass prefsClass = env->GetObjectClass(prefs);
  jmethodID getInt = env->GetMethodID(prefsClass, "getInt", "(Ljava/lang/String;I)I");
  if (!getInt) return defaultValue;

  jstring jkey = env->NewStringUTF(key);
  jint result = env->CallIntMethod(prefs, getInt, jkey, (jint)defaultValue);
  env->DeleteLocalRef(jkey);
  if (HasException(env, "SharedPreferences.getInt")) return defaultValue;

  return result;
}

static bool GetPrefBool(JNIEnv* env, jobject activity, const char* key, bool defaultValue) {
  jclass activityClass = env->GetObjectClass(activity);
  jmethodID getPrefs = env->GetMethodID(activityClass, "getSharedPreferences",
                                        "(Ljava/lang/String;I)Landroid/content/SharedPreferences;");
  if (!getPrefs) return defaultValue;
  jstring prefsName = env->NewStringUTF(kPrefsName);
  jobject prefs = env->CallObjectMethod(activity, getPrefs, prefsName, 0);
  env->DeleteLocalRef(prefsName);
  if (HasException(env, "getSharedPreferences") || !prefs) return defaultValue;

  jclass prefsClass = env->GetObjectClass(prefs);
  jmethodID getBool = env->GetMethodID(prefsClass, "getBoolean", "(Ljava/lang/String;Z)Z");
  if (!getBool) return defaultValue;

  jstring jkey = env->NewStringUTF(key);
  jboolean result = env->CallBooleanMethod(prefs, getBool, jkey, (jboolean)defaultValue);
  env->DeleteLocalRef(jkey);
  if (HasException(env, "SharedPreferences.getBoolean")) return defaultValue;

  return result;
}

static int OpenUriAsNativeFd(JNIEnv* env, jobject activity, const std::string& uriString) {
  if (uriString.empty()) return -1;

  jclass activityClass = env->GetObjectClass(activity);
  jmethodID getContentResolver = env->GetMethodID(activityClass, "getContentResolver",
                                                   "()Landroid/content/ContentResolver;");
  if (!getContentResolver) return -1;
  jobject resolver = env->CallObjectMethod(activity, getContentResolver);
  if (HasException(env, "getContentResolver") || !resolver) return -1;

  jclass uriClass = env->FindClass("android/net/Uri");
  jmethodID parse = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
  jstring juri = env->NewStringUTF(uriString.c_str());
  jobject uri = env->CallStaticObjectMethod(uriClass, parse, juri);
  env->DeleteLocalRef(juri);
  if (HasException(env, "Uri.parse") || !uri) return -1;

  jclass resolverClass = env->GetObjectClass(resolver);
  jmethodID openFd = env->GetMethodID(resolverClass, "openFileDescriptor",
      "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
  if (!openFd) return -1;

  jstring mode = env->NewStringUTF("r");
  jobject pfd = env->CallObjectMethod(resolver, openFd, uri, mode);
  env->DeleteLocalRef(mode);
  if (HasException(env, "openFileDescriptor") || !pfd) return -1;

  jclass pfdClass = env->GetObjectClass(pfd);
  jmethodID detach = env->GetMethodID(pfdClass, "detachFd", "()I");
  if (!detach) {
    jmethodID closePfd = env->GetMethodID(pfdClass, "close", "()V");
    if (closePfd) env->CallVoidMethod(pfd, closePfd);
    return -1;
  }

  jint fd = env->CallIntMethod(pfd, detach);
  if (HasException(env, "ParcelFileDescriptor.detachFd")) return -1;

  return static_cast<int>(fd);
}

static bool CopyUriToPath(JNIEnv* env, jobject activity, const std::string& uriString, const std::string& path) {
  if (uriString.empty() || path.empty()) return false;

  jclass activityClass = env->GetObjectClass(activity);
  jmethodID getContentResolver = env->GetMethodID(activityClass, "getContentResolver",
                                                 "()Landroid/content/ContentResolver;");
  if (!getContentResolver) return false;
  jobject resolver = env->CallObjectMethod(activity, getContentResolver);
  if (HasException(env, "getContentResolver") || !resolver) return false;

  jclass uriClass = env->FindClass("android/net/Uri");
  jmethodID parse = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
  jstring juri = env->NewStringUTF(uriString.c_str());
  jobject uri = env->CallStaticObjectMethod(uriClass, parse, juri);
  env->DeleteLocalRef(juri);
  if (HasException(env, "Uri.parse") || !uri) return false;

  jclass resolverClass = env->GetObjectClass(resolver);
  jmethodID openInputStream = env->GetMethodID(
      resolverClass, "openInputStream", "(Landroid/net/Uri;)Ljava/io/InputStream;");
  jobject inputStream = env->CallObjectMethod(resolver, openInputStream, uri);
  if (HasException(env, "openInputStream") || !inputStream) return false;

  jclass fosClass = env->FindClass("java/io/FileOutputStream");
  jmethodID fosCtor = env->GetMethodID(fosClass, "<init>", "(Ljava/lang/String;)V");
  jstring jpath = env->NewStringUTF(path.c_str());
  jobject outputStream = env->NewObject(fosClass, fosCtor, jpath);
  env->DeleteLocalRef(jpath);
  if (HasException(env, "FileOutputStream.<init>") || !outputStream) return false;

  jclass inputClass = env->GetObjectClass(inputStream);
  jclass outputClass = env->GetObjectClass(outputStream);
  jmethodID readMethod = env->GetMethodID(inputClass, "read", "([B)I");
  jmethodID closeInput = env->GetMethodID(inputClass, "close", "()V");
  jmethodID writeMethod = env->GetMethodID(outputClass, "write", "([BII)V");
  jmethodID closeOutput = env->GetMethodID(outputClass, "close", "()V");
  if (!readMethod || !writeMethod) return false;

  bool ok = true;
  int64_t totalBytes = 0;
  const int kBufferSize = 64 * 1024;
  jbyteArray buffer = env->NewByteArray(kBufferSize);
  while (true) {
    jint read = env->CallIntMethod(inputStream, readMethod, buffer);
    if (HasException(env, "InputStream.read")) { ok = false; break; }
    if (read <= 0) break;
    env->CallVoidMethod(outputStream, writeMethod, buffer, 0, read);
    if (HasException(env, "OutputStream.write")) { ok = false; break; }
    totalBytes += read;
  }
  env->DeleteLocalRef(buffer);
  env->CallVoidMethod(inputStream, closeInput);
  env->CallVoidMethod(outputStream, closeOutput);
  HasException(env, "close streams");

  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "CopyUriToPath: %s -> %s  bytes=%" PRId64 " ok=%d",
                      uriString.c_str(), path.c_str(), totalBytes, ok);
  if (ok && totalBytes == 0) {
    LogError("CopyUriToPath: source was empty or unreadable");
    ok = false;
  }
  return ok;
}

struct SetupFiles {
  std::string mcpx;
  std::string flash;
  std::string hdd;
  std::string dvd;
  std::string eeprom;
  std::string config_path;
  std::string inline_aio_flag_path;
};

struct DisplaySettings {
  int surface_scale = 1;
  bool vsync = false;
  bool unlock_framerate = true;
  bool validation_layers = false;
  std::string filtering = "nearest";
  std::string aspect_ratio = "auto";
};

static bool WriteConfigToml(const std::string& config_path,
                            const std::string& mcpx,
                            const std::string& flash,
                            const std::string& hdd,
                            const std::string& dvd,
                            const std::string& eeprom,
                            int tcg_tb_size = 128,
                            const DisplaySettings& ds = {}) {
  if (config_path.empty()) return false;
  toml::table tbl;

  if (FileExists(config_path)) {
    try {
      tbl = toml::parse_file(config_path);
    } catch (const toml::parse_error&) {
      // Ignore parse errors; we'll rewrite a clean config.
    }
  }

  auto EnsureTable = [](toml::table& parent, std::string_view key) -> toml::table* {
    if (auto* node = parent.get(key)) {
      if (auto* existing = node->as_table()) {
        return existing;
      }
    }
    parent.insert_or_assign(key, toml::table{});
    return parent.get(key)->as_table();
  };

  toml::table* general = EnsureTable(tbl, "general");
  toml::table* display = EnsureTable(tbl, "display");
  toml::table* display_window = EnsureTable(*display, "window");
  toml::table* audio = EnsureTable(tbl, "audio");
  toml::table* audio_vp = EnsureTable(*audio, "vp");
  toml::table* android = EnsureTable(tbl, "android");
  toml::table* sys = EnsureTable(tbl, "sys");
  toml::table* files = EnsureTable(*sys, "files");
  if (!general || !display || !display_window || !audio || !audio_vp ||
      !android || !sys || !files) {
    LogErrorFmt("Failed to build config tables at %s", config_path.c_str());
    return false;
  }

  general->insert_or_assign("show_welcome", false);
  display->insert_or_assign("renderer", "vulkan");
  display->insert_or_assign("filtering", ds.filtering);
  display_window->insert_or_assign("vsync", ds.vsync);

  toml::table* display_quality = EnsureTable(*display, "quality");
  if (display_quality) {
    display_quality->insert_or_assign("surface_scale", ds.surface_scale);
  }

  toml::table* display_ui = EnsureTable(*display, "ui");
  if (display_ui) {
    display_ui->insert_or_assign("aspect_ratio", ds.aspect_ratio);
  }
  toml::table* display_vulkan = EnsureTable(*display, "vulkan");
  if (display_vulkan) {
    display_vulkan->insert_or_assign("validation_layers", ds.validation_layers);
  }
  if (!audio_vp->contains("num_workers")) {
    audio_vp->insert_or_assign("num_workers", 0);
  }
  if (!audio->contains("hrtf")) {
    audio->insert_or_assign("hrtf", true);
  }
  if (!audio->contains("volume_limit")) {
    audio->insert_or_assign("volume_limit", 1.0);
  }
  if (!android->contains("force_cpu_blit")) {
    android->insert_or_assign("force_cpu_blit", false);
  }
  if (!android->contains("tcg_tuning")) {
    android->insert_or_assign("tcg_tuning", true);
  }
  if (!android->contains("tcg_thread")) {
    android->insert_or_assign("tcg_thread", "multi");
  }
  android->insert_or_assign("tcg_tb_size", tcg_tb_size);

  toml::table* perf = EnsureTable(tbl, "perf");
  if (perf) {
    perf->insert_or_assign("unlock_framerate", ds.unlock_framerate);
  }

  files->insert_or_assign("bootrom_path", mcpx);
  files->insert_or_assign("flashrom_path", flash);
  files->insert_or_assign("eeprom_path", eeprom);
  files->insert_or_assign("hdd_path", hdd);
  files->insert_or_assign("dvd_path", dvd);

  std::ofstream out(config_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    LogErrorFmt("Failed to write config at %s", config_path.c_str());
    return false;
  }
  out << tbl;
  out.close();
  return true;
}

static SetupFiles SyncSetupFiles() {
  SetupFiles out{};
  JNIEnv* env = GetEnv();
  jobject activity = GetActivity(env);
  if (!env || !activity) {
    LogError("JNI environment not ready for setup sync");
    return out;
  }

  LogInfo("SyncSetupFiles: start");
  const char* basePath = SDL_AndroidGetInternalStoragePath();
  int extState = SDL_AndroidGetExternalStorageState();
  if (extState & SDL_ANDROID_EXTERNAL_STORAGE_WRITE) {
    const char* external = SDL_AndroidGetExternalStoragePath();
    if (external && external[0] != '\0') {
      basePath = external;
    }
  }
  if (!basePath || basePath[0] == '\0') {
    LogError("Storage path not available");
    return out;
  }
  LogInfoFmt("SyncSetupFiles: base path %s", basePath);

  std::string base = std::string(basePath) + "/x1box";
  EnsureDirExists(base);
  out.eeprom = base + "/eeprom.bin";
  out.inline_aio_flag_path = base + "/inline_aio_required.flag";

  const std::string mcpxPath = GetPrefString(env, activity, "mcpxPath");
  const std::string flashPath = GetPrefString(env, activity, "flashPath");
  const std::string hddPath = GetPrefString(env, activity, "hddPath");
  const std::string dvdPath = GetPrefString(env, activity, "dvdPath");
  const std::string mcpxUri = GetPrefString(env, activity, "mcpxUri");
  const std::string flashUri = GetPrefString(env, activity, "flashUri");
  const std::string hddUri = GetPrefString(env, activity, "hddUri");
  const std::string dvdUri = GetPrefString(env, activity, "dvdUri");

  LogInfoFmt("Prefs mcpxPath=%s", mcpxPath.c_str());
  LogInfoFmt("Prefs flashPath=%s", flashPath.c_str());
  LogInfoFmt("Prefs hddPath=%s", hddPath.c_str());
  LogInfoFmt("Prefs dvdPath=%s", dvdPath.c_str());
  LogInfoFmt("Prefs mcpxUri=%s", mcpxUri.c_str());
  LogInfoFmt("Prefs flashUri=%s", flashUri.c_str());
  LogInfoFmt("Prefs hddUri=%s", hddUri.c_str());
  LogInfoFmt("Prefs dvdUri=%s", dvdUri.c_str());

  if (!mcpxPath.empty() && FileExists(mcpxPath)) {
    out.mcpx = mcpxPath;
  }
  if (out.mcpx.empty() && !mcpxUri.empty()) {
    out.mcpx = base + "/mcpx.bin";
    if (FileExists(out.mcpx)) {
      LogInfo("MCPX ROM already in app storage, skipping copy");
    } else if (CopyUriToPath(env, activity, mcpxUri, out.mcpx)) {
      LogInfo("MCPX ROM synced to app storage");
    } else {
      LogError("Failed to sync MCPX ROM");
    }
  }
  if (!flashPath.empty() && FileExists(flashPath)) {
    out.flash = flashPath;
  }
  if (out.flash.empty() && !flashUri.empty()) {
    out.flash = base + "/flash.bin";
    if (FileExists(out.flash)) {
      LogInfo("Flash ROM already in app storage, skipping copy");
    } else if (CopyUriToPath(env, activity, flashUri, out.flash)) {
      LogInfo("Flash ROM synced to app storage");
    } else {
      LogError("Failed to sync flash ROM");
    }
  }
  if (!hddPath.empty() && FileExists(hddPath)) {
    out.hdd = hddPath;
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "HDD from pref: %s  size=%" PRId64, hddPath.c_str(), FileSize(hddPath));
  }
  if (out.hdd.empty() && !hddUri.empty()) {
    out.hdd = base + "/hdd.img";
    if (FileExists(out.hdd)) {
      LogInfo("HDD image already in app storage, skipping copy");
    } else if (CopyUriToPath(env, activity, hddUri, out.hdd)) {
      LogInfo("HDD image synced to app storage");
    } else {
      LogError("Failed to sync HDD image");
      unlink(out.hdd.c_str());
      out.hdd.clear();
    }
  }
  if (!out.hdd.empty()) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "HDD resolved: %s  size=%" PRId64, out.hdd.c_str(), FileSize(out.hdd));
    LogQcow2Info(out.hdd);
  }

  if (!dvdPath.empty() && FileExists(dvdPath)) {
    out.dvd = dvdPath;
  }
  if (out.dvd.empty() && !dvdUri.empty()) {
    if (g_dvd_fd >= 0) {
      close(g_dvd_fd);
      g_dvd_fd = -1;
    }
    int fd = OpenUriAsNativeFd(env, activity, dvdUri);
    if (fd >= 0) {
      g_dvd_fd = fd;
      out.dvd = "/dev/fdset/0";
      LogInfoInt("DVD image opened via fd %d (zero-copy, fdset)", fd);
    } else {
      LogError("Failed to open DVD URI as fd, falling back to copy");
      std::string copy_dst = base + "/dvd.iso";
      if (CopyUriToPath(env, activity, dvdUri, copy_dst)) {
        out.dvd = copy_dst;
        LogInfo("DVD image synced to app storage (fallback copy)");
      } else {
        LogError("Failed to sync DVD image");
      }
    }
  }

  out.config_path = base + "/xemu.toml";
  int tbSize = GetPrefInt(env, activity, "tcg_tb_size", 128);

  DisplaySettings ds;
  ds.surface_scale = GetPrefInt(env, activity, "surface_scale", 1);
  if (ds.surface_scale < 1) ds.surface_scale = 1;
  if (ds.surface_scale > 4) ds.surface_scale = 4;
  ds.vsync = GetPrefBool(env, activity, "vsync", false);
  ds.unlock_framerate = GetPrefBool(env, activity, "unlock_framerate", true);
  ds.validation_layers = GetPrefBool(env, activity, "validation_layers", false);

  bool fp_safe = GetPrefBool(env, activity, "fp_safe", true);
  xemu_set_fp_safe(fp_safe);
  __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                      "FP safe (native arithmetic): %s", fp_safe ? "ON" : "OFF");

  bool fp_jit = GetPrefBool(env, activity, "fp_jit", true);
  xemu_set_fp_jit(fp_jit);
  __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                      "FP JIT (native storage + inline ops): %s", fp_jit ? "ON" : "OFF");

  bool fast_fences = GetPrefBool(env, activity, "fast_fences", false);
  xemu_set_fast_fences(fast_fences);
  __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                      "fast fences: %s", fast_fences ? "ON" : "OFF");

  bool draw_reorder = GetPrefBool(env, activity, "draw_reorder", false);
  xemu_set_draw_reorder(draw_reorder);
  __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                      "draw reorder: %s", draw_reorder ? "ON" : "OFF");

  bool bindless_tex = GetPrefBool(env, activity, "bindless_textures", false);
  xemu_set_bindless_textures(bindless_tex);
  __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                      "bindless textures: %s", bindless_tex ? "ON" : "OFF");

  bool async_compile = GetPrefBool(env, activity, "async_compile", false);
  xemu_set_async_compile(async_compile);
  __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                      "async compile: %s", async_compile ? "ON" : "OFF");

  bool frame_skip = GetPrefBool(env, activity, "frame_skip", false);
  xemu_set_frame_skip(frame_skip);
  __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                      "frame skip: %s", frame_skip ? "ON" : "OFF");

  int submit_frames = GetPrefInt(env, activity, "submit_frames", 3);
  xemu_set_submit_frames(submit_frames);
  __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                      "submit frames: %d", submit_frames);
  std::string filterPref = GetPrefString(env, activity, "filtering");
  if (!filterPref.empty()) ds.filtering = filterPref;
  std::string arPref = GetPrefString(env, activity, "aspect_ratio");
  if (!arPref.empty()) ds.aspect_ratio = arPref;

  WriteConfigToml(out.config_path, out.mcpx, out.flash, out.hdd, out.dvd, out.eeprom, tbSize, ds);
  LogInfoFmt("SyncSetupFiles: config %s", out.config_path.c_str());
  LogInfoFmt("Resolved mcpx=%s", out.mcpx.c_str());
  LogInfoFmt("Resolved flash=%s", out.flash.c_str());
  LogInfoFmt("Resolved hdd=%s", out.hdd.c_str());
  LogInfoFmt("Resolved dvd=%s", out.dvd.c_str());
  LogInfoFmt("Resolved eeprom=%s", out.eeprom.c_str());

  {
    FILE *f = fopen(out.config_path.c_str(), "r");
    if (f) {
      char buf[4096];
      size_t n = fread(buf, 1, sizeof(buf) - 1, f);
      buf[n] = '\0';
      fclose(f);
      __android_log_print(ANDROID_LOG_INFO, "xemu-config",
                          "--- xemu.toml ---\n%s\n--- end ---", buf);
    }
  }
  return out;
}
}

extern "C" int xemu_android_main(int argc, char** argv);
extern "C" void qemu_init(int argc, char** argv);
extern "C" int (*qemu_main)(void);
extern "C" void xemu_android_display_preinit(void);
extern "C" void xemu_android_display_wait_ready(void);
extern "C" void xemu_android_display_loop(void);
extern "C" void xemu_android_set_inline_aio_crash_flag_path(const char* path);

#ifndef XEMU_OPT_THREAD_AFFINITY
#define XEMU_OPT_THREAD_AFFINITY 0
#endif

#if XEMU_OPT_THREAD_AFFINITY
#include <sys/syscall.h>
#include <sys/resource.h>

static void xemu_pin_to_big_cores_cpp(const char *label) {
  int ncpus = sysconf(_SC_NPROCESSORS_CONF);
  if (ncpus <= 0 || ncpus > 64) return;

  unsigned long max_freq = 0;
  unsigned long freqs[64];
  for (int i = 0; i < ncpus; i++) {
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
    FILE *f = fopen(path, "r");
    if (f) {
      if (fscanf(f, "%lu", &freqs[i]) != 1) freqs[i] = 0;
      fclose(f);
    } else {
      freqs[i] = 0;
    }
    if (freqs[i] > max_freq) max_freq = freqs[i];
  }
  if (max_freq == 0) return;

  unsigned long threshold = max_freq * 9 / 10;
  unsigned long mask = 0;
  int big_count = 0;
  for (int i = 0; i < ncpus && i < (int)(sizeof(mask) * 8); i++) {
    if (freqs[i] >= threshold) {
      mask |= (1UL << i);
      big_count++;
    }
  }
  if (big_count > 0 && big_count < ncpus) {
    if (syscall(__NR_sched_setaffinity, 0, sizeof(mask), &mask) == 0) {
      __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                          "%s: pinned to %d big cores (max_freq=%lu)",
                          label, big_count, max_freq);
    }
  }
  setpriority(PRIO_PROCESS, 0, -10);
}
#endif

struct QemuLaunchContext {
  int argc;
  char** argv;
};

static int SDLCALL QemuThreadMain(void* data) {
#if XEMU_OPT_THREAD_AFFINITY
  xemu_pin_to_big_cores_cpp("qemu_cpu_thread");
#endif
  auto* ctx = static_cast<QemuLaunchContext*>(data);
  LogInfoInt("QemuThreadMain: show_welcome=%d", g_config.general.show_welcome ? 1 : 0);
  LogInfoFmt("QemuThreadMain: bootrom=%s", g_config.sys.files.bootrom_path ? g_config.sys.files.bootrom_path : "(null)");
  LogInfo("QemuThreadMain: starting");
  return xemu_android_main(ctx->argc, ctx->argv);
}

#ifndef XEMU_OPT_TB_CACHE_HINTS
#define XEMU_OPT_TB_CACHE_HINTS 1
#endif

#if XEMU_OPT_TB_CACHE_HINTS
extern "C" void tb_cache_save(const char *path, uint32_t game_hash);
extern "C" int  tb_cache_load(const char *path, uint32_t game_hash);
extern "C" uint32_t tb_cache_compute_game_hash(const char *bootrom_path,
                                               const char *flashrom_path);
extern "C" void tb_cache_cleanup(void);
#endif

extern "C" int xemu_android_main(int argc, char** argv) {
  if (!qemu_main) {
    LogError("xemu core not linked; qemu_main missing");
    return 1;
  }
  LogInfo("xemu_android_main: qemu_init");
  auto t_init_start = SDL_GetTicks();
  qemu_init(argc, argv);
  auto t_init_end = SDL_GetTicks();
  __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                      "qemu_init took %u ms", t_init_end - t_init_start);

  /* qemu_init's cleanup_add_fd already closed the original fd */
  g_dvd_fd = -1;

#if XEMU_OPT_TB_CACHE_HINTS
  /* Load translation block cache hints for pre-warming */
  const char *storage_load = SDL_AndroidGetInternalStoragePath();

  const char *dump_storage = NULL;
  if (SDL_AndroidGetExternalStorageState() & SDL_ANDROID_EXTERNAL_STORAGE_WRITE) {
    dump_storage = SDL_AndroidGetExternalStoragePath();
  }
  if (!dump_storage || !dump_storage[0]) {
    dump_storage = storage_load;
  }
  if (dump_storage) {
    char dump_dir[PATH_MAX];
    snprintf(dump_dir, sizeof(dump_dir), "%s/rt_dumps", dump_storage);
    nv2a_dbg_set_rt_dump_path(dump_dir);
  }

  /* Load translation block cache hints for pre-warming */
  if (storage_load) {
    char cache_path[PATH_MAX];
    snprintf(cache_path, sizeof(cache_path), "%s/x1box/tb_cache.bin", storage_load);
    uint32_t game_hash = tb_cache_compute_game_hash(
        g_config.sys.files.bootrom_path, g_config.sys.files.flashrom_path);
    int nhints = tb_cache_load(cache_path, game_hash);
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "TB cache: loaded %d hints from %s", nhints, cache_path);
  }
#endif

  LogInfo("xemu_android_main: qemu_main");
  int rc = qemu_main();
  LogErrorInt("xemu_android_main: qemu_main returned %d", rc);

#if XEMU_OPT_TB_CACHE_HINTS
  /* Save translation block cache hints for next launch */
  const char *storage = SDL_AndroidGetInternalStoragePath();
  if (storage) {
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/x1box", storage);
    mkdir(dir_path, 0755);
    char cache_path[PATH_MAX];
    snprintf(cache_path, sizeof(cache_path), "%s/tb_cache.bin", dir_path);
    uint32_t game_hash = tb_cache_compute_game_hash(
        g_config.sys.files.bootrom_path, g_config.sys.files.flashrom_path);
    tb_cache_save(cache_path, game_hash);
  }
  tb_cache_cleanup();
#endif

  if (g_dvd_fd >= 0) {
    close(g_dvd_fd);
    g_dvd_fd = -1;
  }

  return rc;
}

extern "C" int SDL_main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  LogInfo("SDL_main: start");
  // Prefer AAudio on Android, but keep Android AudioTrack as fallback.
  SDL_SetHintWithPriority(SDL_HINT_AUDIODRIVER, "aaudio,android",
                          SDL_HINT_OVERRIDE);
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
  SDL_DisableScreenSaver();

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SDL_Init failed: %s", SDL_GetError());
    return 1;
  }
  SDL_GameControllerEventState(SDL_ENABLE);
  LoadGameControllerMappingsFromAssets();

  auto t_sync_start = SDL_GetTicks();
  SetupFiles setup = SyncSetupFiles();
  auto t_sync_end = SDL_GetTicks();
  __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                      "SyncSetupFiles took %u ms", t_sync_end - t_sync_start);

  xemu_android_set_inline_aio_crash_flag_path(setup.inline_aio_flag_path.empty()
                                                   ? nullptr
                                                   : setup.inline_aio_flag_path.c_str());

  if (!SDL_getenv("XEMU_ANDROID_INLINE_AIO")) {
    const bool use_inline_aio =
        ShouldEnableInlineAioWorkaround(setup.inline_aio_flag_path);
    setenv("XEMU_ANDROID_INLINE_AIO", use_inline_aio ? "1" : "0", 1);
    LogInfoFmt("XEMU_ANDROID_INLINE_AIO=%s", use_inline_aio ? "1" : "0");
  }

  if (!setup.config_path.empty()) {
    LogInfo("SDL_main: loading config");
    xemu_settings_set_path(setup.config_path.c_str());
    if (!xemu_settings_load()) {
      const char* err = xemu_settings_get_error_message();
      if (!err) {
        err = "Failed to load config file";
      }
      LogError(err);
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                               "Failed to load xemu config file",
                               err,
                               nullptr);
      SDL_Quit();
      return 1;
    }
    LogInfo("SDL_main: config loaded");
    LogInfoInt("Config show_welcome=%d", g_config.general.show_welcome ? 1 : 0);
    LogInfoFmt("Config bootrom=%s", g_config.sys.files.bootrom_path ? g_config.sys.files.bootrom_path : "(null)");
    LogInfoFmt("Config flashrom=%s", g_config.sys.files.flashrom_path ? g_config.sys.files.flashrom_path : "(null)");
    LogInfoFmt("Config hdd=%s", g_config.sys.files.hdd_path ? g_config.sys.files.hdd_path : "(null)");
    LogInfoFmt("Config dvd=%s", g_config.sys.files.dvd_path ? g_config.sys.files.dvd_path : "(null)");
    LogInfoFmt("Config eeprom=%s", g_config.sys.files.eeprom_path ? g_config.sys.files.eeprom_path : "(null)");

    // Ensure config strings are non-null and aligned with Android setup paths.
    if (!setup.mcpx.empty()) {
      xemu_settings_set_string(&g_config.sys.files.bootrom_path, setup.mcpx.c_str());
    } else if (!g_config.sys.files.bootrom_path) {
      xemu_settings_set_string(&g_config.sys.files.bootrom_path, "");
    }
    if (!setup.flash.empty()) {
      xemu_settings_set_string(&g_config.sys.files.flashrom_path, setup.flash.c_str());
    } else if (!g_config.sys.files.flashrom_path) {
      xemu_settings_set_string(&g_config.sys.files.flashrom_path, "");
    }
    if (!setup.hdd.empty()) {
      xemu_settings_set_string(&g_config.sys.files.hdd_path, setup.hdd.c_str());
    } else if (!g_config.sys.files.hdd_path) {
      xemu_settings_set_string(&g_config.sys.files.hdd_path, "");
    }
    if (!setup.dvd.empty()) {
      xemu_settings_set_string(&g_config.sys.files.dvd_path, setup.dvd.c_str());
    } else {
      xemu_settings_set_string(&g_config.sys.files.dvd_path, "");
    }
    if (!setup.eeprom.empty()) {
      xemu_settings_set_string(&g_config.sys.files.eeprom_path, setup.eeprom.c_str());
    } else if (!g_config.sys.files.eeprom_path) {
      xemu_settings_set_string(&g_config.sys.files.eeprom_path, "");
    }
    setenv("XEMU_ANDROID_FORCE_CPU_BLIT", "0", 1);
    g_config.general.show_welcome = false;
    g_config.perf.cache_shaders = true;
    LogInfoInt("Config final show_welcome=%d", g_config.general.show_welcome ? 1 : 0);
    LogInfoInt("Config final cache_shaders=%d", g_config.perf.cache_shaders ? 1 : 0);
    LogInfoInt("Config final renderer=%d", (int)g_config.display.renderer);
    LogInfoFmt("Config final bootrom=%s", g_config.sys.files.bootrom_path ? g_config.sys.files.bootrom_path : "(null)");
    LogInfoFmt("Config final flashrom=%s", g_config.sys.files.flashrom_path ? g_config.sys.files.flashrom_path : "(null)");
    LogInfoFmt("Config final hdd=%s", g_config.sys.files.hdd_path ? g_config.sys.files.hdd_path : "(null)");
    LogInfoFmt("Config final dvd=%s", g_config.sys.files.dvd_path ? g_config.sys.files.dvd_path : "(null)");
    LogInfoFmt("Config final eeprom=%s", g_config.sys.files.eeprom_path ? g_config.sys.files.eeprom_path : "(null)");

    std::vector<std::string> arg_storage;
    arg_storage.emplace_back("xemu");
    if (IsTcgTuningEnabled()) {
      const char* tcg_thread = GetTcgThreadFromEnv();
      int tcg_tb_size = GetTcgTbSizeFromEnv();
      char accel_opts[64];
      snprintf(accel_opts, sizeof(accel_opts), "tcg,thread=%s,tb-size=%d",
               tcg_thread, tcg_tb_size);
      arg_storage.emplace_back("-accel");
      arg_storage.emplace_back(accel_opts);
      LogInfoFmt("SDL_main: using accel %s", accel_opts);
    } else {
      LogInfo("SDL_main: TCG tuning disabled");
    }

    if (g_dvd_fd >= 0) {
      int flags = fcntl(g_dvd_fd, F_GETFD);
      if (flags != -1 && (flags & FD_CLOEXEC)) {
        fcntl(g_dvd_fd, F_SETFD, flags & ~FD_CLOEXEC);
      }
      char add_fd_arg[64];
      snprintf(add_fd_arg, sizeof(add_fd_arg), "fd=%d,set=0", g_dvd_fd);
      arg_storage.emplace_back("-add-fd");
      arg_storage.emplace_back(add_fd_arg);
      LogInfoInt("SDL_main: passing DVD fd %d via -add-fd", g_dvd_fd);
    }

    std::vector<char*> xemu_argv;
    xemu_argv.reserve(arg_storage.size() + 1);
    for (auto& arg : arg_storage) {
      xemu_argv.push_back(const_cast<char*>(arg.c_str()));
    }
    xemu_argv.push_back(nullptr);
    LogInfo("SDL_main: launching xemu core");
    xemu_android_display_preinit();

    QemuLaunchContext launch_ctx{
      static_cast<int>(arg_storage.size()),
      xemu_argv.data(),
    };
    SDL_Thread* qemu_thread = SDL_CreateThread(QemuThreadMain, "qemu_main", &launch_ctx);
    if (!qemu_thread) {
      LogErrorFmt("Failed to start xemu thread: %s", SDL_GetError());
      return 1;
    }
    LogInfo("SDL_main: qemu thread started");
    (void)qemu_thread;
    xemu_android_display_wait_ready();
    LogInfo("SDL_main: display ready, entering render loop");
    xemu_android_display_loop();
    return 0;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

  SDL_Window* window = SDL_CreateWindow(
    "xemu (Android bootstrap)",
    SDL_WINDOWPOS_CENTERED,
    SDL_WINDOWPOS_CENTERED,
    1280,
    720,
    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
  );

  if (!window) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SDL_CreateWindow failed: %s", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_GLContext gl = SDL_GL_CreateContext(window);
  if (!gl) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SDL_GL_CreateContext failed: %s", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SDL_GL_MakeCurrent(window, gl);
  SDL_GL_SetSwapInterval(1);

  LogInfo("xemu Android bootstrap running (core not wired yet)");

  bool running = true;
  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) {
        running = false;
      } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_AC_BACK) {
        running = false;
      }
    }

    int w = 0;
    int h = 0;
    SDL_GL_GetDrawableSize(window, &w, &h);
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    glViewport(0, 0, w, h);
    glClearColor(0.05f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    SDL_GL_SwapWindow(window);
  }

  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_rfandango_xemuandroid_MainActivity_nativeGetFps(JNIEnv *, jobject)
{
    return static_cast<jint>(g_nv2a_stats.increment_fps);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_rfandango_xemuandroid_MainActivity_nativeGetFramePacing(JNIEnv *env, jobject)
{
    char buf[256];
    nv2a_profile_get_pacing_str(buf, sizeof(buf));
    return env->NewStringUTF(buf);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_rfandango_xemuandroid_MainActivity_nativeGetShaderStats(JNIEnv *env, jobject)
{
    char buf[256];
    nv2a_profile_get_shader_stats_str(buf, sizeof(buf));
    return env->NewStringUTF(buf);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rfandango_xemuandroid_MainActivity_nativeCaptureFrame(JNIEnv *, jobject)
{
#ifdef CONFIG_RENDERDOC
    if (nv2a_dbg_renderdoc_available()) {
        nv2a_dbg_renderdoc_capture_frames(1, false);
        return JNI_TRUE;
    }
#endif
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rfandango_xemuandroid_MainActivity_nativeDumpRenderTarget(JNIEnv *env, jobject)
{
    nv2a_dbg_trigger_rt_dump();
}

extern "C" JNIEXPORT void JNICALL
Java_com_rfandango_xemuandroid_MainActivity_nativeDumpDiagFrame(JNIEnv *, jobject)
{
    nv2a_dbg_trigger_diag_frame();
}

extern "C" char g_vulkan_driver_info[256];

extern "C" JNIEXPORT jstring JNICALL
Java_com_rfandango_xemuandroid_MainActivity_nativeGetDriverInfo(JNIEnv *env, jobject)
{
    return env->NewStringUTF(g_vulkan_driver_info);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeGetFpSafe(JNIEnv *, jobject)
{
    return xemu_get_fp_safe() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeSetFpSafe(JNIEnv *, jobject, jboolean enable)
{
    xemu_set_fp_safe(enable == JNI_TRUE);
    if (!enable) {
        const char *storage = SDL_AndroidGetInternalStoragePath();
        if (storage) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/x1box/tb_cache.bin", storage);
            remove(path);
        }
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeGetFastFences(JNIEnv *, jobject)
{
    return xemu_get_fast_fences() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeSetFastFences(JNIEnv *, jobject, jboolean enable)
{
    xemu_set_fast_fences(enable == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeGetDrawReorder(JNIEnv *, jobject)
{
    return xemu_get_draw_reorder() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeSetDrawReorder(JNIEnv *, jobject, jboolean enable)
{
    xemu_set_draw_reorder(enable == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeGetBindlessTextures(JNIEnv *, jobject)
{
    return xemu_get_bindless_textures() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeSetBindlessTextures(JNIEnv *, jobject, jboolean enable)
{
    xemu_set_bindless_textures(enable == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeGetAsyncCompile(JNIEnv *, jobject)
{
    return xemu_get_async_compile() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeSetAsyncCompile(JNIEnv *, jobject, jboolean enable)
{
    xemu_set_async_compile(enable == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeGetFrameSkip(JNIEnv *, jobject)
{
    return xemu_get_frame_skip() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeSetFrameSkip(JNIEnv *, jobject, jboolean enable)
{
    xemu_set_frame_skip(enable == JNI_TRUE);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeGetSubmitFrames(JNIEnv *, jobject)
{
    return static_cast<jint>(xemu_get_submit_frames());
}

extern "C" JNIEXPORT void JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeSetSubmitFrames(JNIEnv *, jobject, jint count)
{
    xemu_set_submit_frames(static_cast<int>(count));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeGetFpJit(JNIEnv *, jobject)
{
    return xemu_get_fp_jit() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rfandango_xemuandroid_SettingsActivity_nativeSetFpJit(JNIEnv *, jobject, jboolean enable)
{
    xemu_set_fp_jit(enable == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rfandango_xemuandroid_MainActivity_nativeTogglePause(JNIEnv *, jobject)
{
    xemu_android_toggle_pause();
    return runstate_is_running() ? JNI_FALSE : JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rfandango_xemuandroid_MainActivity_nativeIsPaused(JNIEnv *, jobject)
{
    return runstate_is_running() ? JNI_FALSE : JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rfandango_xemuandroid_MainActivity_nativeExitEmulation(JNIEnv *, jobject)
{
    xemu_android_request_exit();
}

#ifdef CONFIG_VULKAN
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rfandango_xemuandroid_GpuDriverHelper_nativeSupportsCustomDriverLoading(JNIEnv *, jclass)
{
    return access("/dev/kgsl-3d0", F_OK) == 0 ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_rfandango_xemuandroid_GpuDriverHelper_nativeInitializeDriver(
    JNIEnv *env, jclass,
    jstring hookLibDir, jstring customDriverDir,
    jstring customDriverName)
{
    const char *hook_dir = hookLibDir ? env->GetStringUTFChars(hookLibDir, nullptr) : nullptr;
    const char *driver_dir = customDriverDir ? env->GetStringUTFChars(customDriverDir, nullptr) : nullptr;
    const char *driver_name = customDriverName ? env->GetStringUTFChars(customDriverName, nullptr) : nullptr;

    void *handle = nullptr;

    if (driver_name && driver_name[0] != '\0') {
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "Loading custom Vulkan driver: %s from %s",
                            driver_name, driver_dir ? driver_dir : "(null)");
        handle = adrenotools_open_libvulkan(
            RTLD_NOW,
            ADRENOTOOLS_DRIVER_CUSTOM,
            nullptr,
            hook_dir,
            driver_dir,
            driver_name,
            nullptr,
            nullptr);

        if (handle) {
            g_custom_vulkan_library = handle;
            __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                                "Custom Vulkan driver loaded successfully via adrenotools");
        } else {
            __android_log_print(ANDROID_LOG_WARN, "xemu-android",
                                "adrenotools failed to load custom driver, will fall back to system default");
        }
    } else {
        __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                            "No custom driver specified, using system Vulkan driver");
    }

    if (driver_name) env->ReleaseStringUTFChars(customDriverName, driver_name);
    if (driver_dir) env->ReleaseStringUTFChars(customDriverDir, driver_dir);
    if (hook_dir) env->ReleaseStringUTFChars(hookLibDir, hook_dir);
}
#endif
