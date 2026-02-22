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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#include "xemu-settings.h"
#include "hw/xbox/nv2a/debug.h"

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
    return true;
  }

  return false;
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

  const int kBufferSize = 64 * 1024;
  jbyteArray buffer = env->NewByteArray(kBufferSize);
  while (true) {
    jint read = env->CallIntMethod(inputStream, readMethod, buffer);
    if (HasException(env, "InputStream.read")) break;
    if (read <= 0) break;
    env->CallVoidMethod(outputStream, writeMethod, buffer, 0, read);
    if (HasException(env, "OutputStream.write")) break;
  }
  env->DeleteLocalRef(buffer);
  env->CallVoidMethod(inputStream, closeInput);
  env->CallVoidMethod(outputStream, closeOutput);
  HasException(env, "close streams");
  return true;
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

static bool WriteConfigToml(const std::string& config_path,
                            const std::string& mcpx,
                            const std::string& flash,
                            const std::string& hdd,
                            const std::string& dvd,
                            const std::string& eeprom) {
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
  if (!display->contains("filtering")) {
    display->insert_or_assign("filtering", "nearest");
  }
  if (!display_window->contains("vsync")) {
    display_window->insert_or_assign("vsync", false);
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
  if (!android->contains("tcg_tb_size")) {
    android->insert_or_assign("tcg_tb_size", 128);
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
    if (CopyUriToPath(env, activity, mcpxUri, out.mcpx)) {
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
    if (CopyUriToPath(env, activity, flashUri, out.flash)) {
      LogInfo("Flash ROM synced to app storage");
    } else {
      LogError("Failed to sync flash ROM");
    }
  }
  if (!hddPath.empty() && FileExists(hddPath)) {
    out.hdd = hddPath;
  }
  if (out.hdd.empty() && !hddUri.empty()) {
    out.hdd = base + "/hdd.img";
    if (CopyUriToPath(env, activity, hddUri, out.hdd)) {
      LogInfo("HDD image synced to app storage");
    } else {
      LogError("Failed to sync HDD image");
    }
  }

  if (!dvdPath.empty() && FileExists(dvdPath)) {
    out.dvd = dvdPath;
  }
  if (out.dvd.empty() && !dvdUri.empty()) {
    out.dvd = base + "/dvd.iso";
    if (CopyUriToPath(env, activity, dvdUri, out.dvd)) {
      LogInfo("DVD image synced to app storage");
    } else {
      LogError("Failed to sync DVD image");
    }
  }

  out.config_path = base + "/xemu.toml";
  WriteConfigToml(out.config_path, out.mcpx, out.flash, out.hdd, out.dvd, out.eeprom);
  LogInfoFmt("SyncSetupFiles: config %s", out.config_path.c_str());
  LogInfoFmt("Resolved mcpx=%s", out.mcpx.c_str());
  LogInfoFmt("Resolved flash=%s", out.flash.c_str());
  LogInfoFmt("Resolved hdd=%s", out.hdd.c_str());
  LogInfoFmt("Resolved dvd=%s", out.dvd.c_str());
  LogInfoFmt("Resolved eeprom=%s", out.eeprom.c_str());
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

struct QemuLaunchContext {
  int argc;
  char** argv;
};

static int SDLCALL QemuThreadMain(void* data) {
  auto* ctx = static_cast<QemuLaunchContext*>(data);
  LogInfoInt("QemuThreadMain: show_welcome=%d", g_config.general.show_welcome ? 1 : 0);
  LogInfoFmt("QemuThreadMain: bootrom=%s", g_config.sys.files.bootrom_path ? g_config.sys.files.bootrom_path : "(null)");
  LogInfo("QemuThreadMain: starting");
  return xemu_android_main(ctx->argc, ctx->argv);
}

extern "C" void tb_cache_save(const char *path, uint32_t game_hash);
extern "C" int  tb_cache_load(const char *path, uint32_t game_hash);
extern "C" uint32_t tb_cache_compute_game_hash(const char *bootrom_path,
                                               const char *flashrom_path);
extern "C" void tb_cache_cleanup(void);

extern "C" int xemu_android_main(int argc, char** argv) {
  if (!qemu_main) {
    LogError("xemu core not linked; qemu_main missing");
    return 1;
  }
  LogInfo("xemu_android_main: qemu_init");
  qemu_init(argc, argv);

  /* Load translation block cache hints for pre-warming */
  const char *storage_load = SDL_AndroidGetInternalStoragePath();
  if (storage_load) {
    char cache_path[PATH_MAX];
    snprintf(cache_path, sizeof(cache_path), "%s/x1box/tb_cache.bin", storage_load);
    uint32_t game_hash = tb_cache_compute_game_hash(
        g_config.sys.files.bootrom_path, g_config.sys.files.flashrom_path);
    int nhints = tb_cache_load(cache_path, game_hash);
    __android_log_print(ANDROID_LOG_INFO, "xemu-android",
                        "TB cache: loaded %d hints from %s", nhints, cache_path);
  }

  LogInfo("xemu_android_main: qemu_main");
  int rc = qemu_main();
  LogErrorInt("xemu_android_main: qemu_main returned %d", rc);

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

  SetupFiles setup = SyncSetupFiles();

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
    } else if (!g_config.sys.files.dvd_path) {
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
Java_com_izzy2lost_x1box_MainActivity_nativeGetFps(JNIEnv *, jobject)
{
    return static_cast<jint>(g_nv2a_stats.increment_fps);
}

extern "C" char g_vulkan_driver_info[256];

extern "C" JNIEXPORT jstring JNICALL
Java_com_izzy2lost_x1box_MainActivity_nativeGetDriverInfo(JNIEnv *env, jobject)
{
    return env->NewStringUTF(g_vulkan_driver_info);
}

#ifdef CONFIG_VULKAN
extern "C" JNIEXPORT jboolean JNICALL
Java_com_izzy2lost_x1box_GpuDriverHelper_nativeSupportsCustomDriverLoading(JNIEnv *, jclass)
{
    return access("/dev/kgsl-3d0", F_OK) == 0 ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_izzy2lost_x1box_GpuDriverHelper_nativeInitializeDriver(
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
