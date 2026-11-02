package com.rfandango.haku_x.data

import android.content.Context
import android.net.Uri
import androidx.core.content.edit
import com.rfandango.haku_x.NativeCoreActivity
import java.util.Locale

/**
 * The app's settings, in the shared preferences file the core's activity
 * reads too (x1box_prefs: the name predates the native core).
 */
class AppPrefs(context: Context) {
  private val sp = context.getSharedPreferences(NAME, Context.MODE_PRIVATE)

  /**
   * The folders the library lists, in the order they were added (one line
   * each). Before there could be several, the one folder was
   * KEY_GAMES_FOLDER, which still holds the first.
   */
  var gamesFolders: List<Uri>
    get() {
      val list = sp.getString(KEY_GAMES_FOLDERS, null)
        ?: return listOfNotNull(sp.getString(KEY_GAMES_FOLDER, null)?.let(Uri::parse))
      return list.lines().filter { it.isNotBlank() }.map(Uri::parse)
    }
    set(value) = sp.edit {
      val folders = value.distinct()
      putString(KEY_GAMES_FOLDERS, folders.joinToString("\n"))
      putString(KEY_GAMES_FOLDER, folders.firstOrNull()?.toString())
    }

  /** The first games folder; setting it replaces the first (the setup's choice). */
  var gamesFolderUri: Uri?
    get() = gamesFolders.firstOrNull()
    set(value) {
      gamesFolders = listOfNotNull(value) + gamesFolders.drop(1)
    }

  var setupComplete: Boolean
    get() = sp.getBoolean(KEY_SETUP_COMPLETE, false)
    set(value) = sp.edit { putBoolean(KEY_SETUP_COMPLETE, value) }

  /** The previous release's data brought over, or declined (PreviousVersion): the migration screen ran. */
  var previousMigrated: Boolean
    get() = sp.getBoolean(KEY_PREVIOUS_MIGRATED, false)
    set(value) = sp.edit { putBoolean(KEY_PREVIOUS_MIGRATED, value) }

  /** Every frame presented at once, no vblank wait (read at game start). */
  var uncapped: Boolean
    get() = sp.getBoolean(NativeCoreActivity.PREF_UNCAP, false)
    set(value) = sp.edit { putBoolean(NativeCoreActivity.PREF_UNCAP, value) }

  /** Shaders compiled off the render thread, draws needing one skipped meanwhile (read at game start). */
  var asyncShaders: Boolean
    get() = sp.getBoolean(NativeCoreActivity.PREF_ASYNC_SHADERS, true)
    set(value) = sp.edit { putBoolean(NativeCoreActivity.PREF_ASYNC_SHADERS, value) }

  /** The game's cached shaders built before it starts (read at game start). */
  var prebuildShaders: Boolean
    get() = sp.getBoolean(NativeCoreActivity.PREF_PREBUILD_SHADERS, true)
    set(value) = sp.edit { putBoolean(NativeCoreActivity.PREF_PREBUILD_SHADERS, value) }

  /** Reads take a quarter of the console's drives' time, the game's other threads running meanwhile (read at game start). */
  var consoleTiming: Boolean
    get() = sp.getBoolean(KEY_CONSOLE_TIMING, true)
    set(value) = sp.edit { putBoolean(KEY_CONSOLE_TIMING, value) }

  var showFps: Boolean
    get() = sp.getBoolean(KEY_SHOW_FPS, true)
    set(value) = sp.edit { putBoolean(KEY_SHOW_FPS, value) }

  /** The library's note recommending a Turnip driver was shown (once: GpuDriverHelper.turnipRecommended). */
  var turnipNoteShown: Boolean
    get() = sp.getBoolean(KEY_TURNIP_NOTE, false)
    set(value) = sp.edit { putBoolean(KEY_TURNIP_NOTE, value) }

  /** The app's look: the phone's (light or dark as it is), light or dark (ui/theme AppTheme). */
  var theme: String
    get() = sp.getString(KEY_THEME, THEME_SYSTEM) ?: THEME_SYSTEM
    set(value) = sp.edit { putString(KEY_THEME, value) }

  /** Debug builds: the HLE GPU's stutter and hang monitor over the game (NativeCoreActivity). */
  var showHitches: Boolean
    get() = sp.getBoolean(NativeCoreActivity.PREF_SHOW_HITCHES, true)
    set(value) = sp.edit { putBoolean(NativeCoreActivity.PREF_SHOW_HITCHES, value) }

  /** The HLE GPU's render scale, 1-4x the title's resolution (read at game start). */
  var renderScale: Int
    get() = sp.getInt(NativeCoreActivity.PREF_SCALE, NativeCoreActivity.DEFAULT_SCALE)
    set(value) = sp.edit { putInt(NativeCoreActivity.PREF_SCALE, value) }

  /** The picture's shape: auto, 4:3, 16:9 or stretch (read at game start). */
  var aspectRatio: String
    get() = sp.getString(NativeCoreActivity.PREF_ASPECT, NativeCoreActivity.ASPECT_AUTO) ?: NativeCoreActivity.ASPECT_AUTO
    set(value) = sp.edit { putString(NativeCoreActivity.PREF_ASPECT, value) }

  /** FXAA over each frame before it's scaled to the screen (read at game start). */
  var fxaa: Boolean
    get() = sp.getBoolean(NativeCoreActivity.PREF_FXAA, false)
    set(value) = sp.edit { putBoolean(NativeCoreActivity.PREF_FXAA, value) }

  /** How a picture smaller than the screen is scaled up: bilinear or sgsr (read at game start). */
  var upscaleFilter: String
    get() = sp.getString(NativeCoreActivity.PREF_UPSCALE, NativeCoreActivity.UPSCALE_BILINEAR)
      ?: NativeCoreActivity.UPSCALE_BILINEAR
    set(value) = sp.edit { putString(NativeCoreActivity.PREF_UPSCALE, value) }

  // The console's video settings, as its dashboard keeps them (EEPROM video
  // flags and the AV cable): games choose their display modes from these.

  /** normal (4:3), letterbox or widescreen. */
  var screenFormat: String
    get() = sp.getString(KEY_SCREEN_FORMAT, FORMAT_NORMAL) ?: FORMAT_NORMAL
    set(value) = sp.edit { putString(KEY_SCREEN_FORMAT, value) }

  var video480p: Boolean
    get() = sp.getBoolean(KEY_480P, true)
    set(value) = sp.edit { putBoolean(KEY_480P, value) }

  var video720p: Boolean
    get() = sp.getBoolean(KEY_720P, false)
    set(value) = sp.edit { putBoolean(KEY_720P, value) }

  var video1080i: Boolean
    get() = sp.getBoolean(KEY_1080I, false)
    set(value) = sp.edit { putBoolean(KEY_1080I, value) }

  /** PAL titles at 60 Hz where they offer it. */
  var pal60: Boolean
    get() = sp.getBoolean(KEY_PAL60, false)
    set(value) = sp.edit { putBoolean(KEY_PAL60, value) }

  /** The AV cable: hdtv (component: HD modes), standard (composite) or svideo. */
  var avPack: String
    get() = sp.getString(KEY_AV_PACK, AV_HDTV) ?: AV_HDTV
    set(value) = sp.edit { putString(KEY_AV_PACK, value) }

  /**
   * The console's network: off, lan (system link with consoles on this network), kai (XLink Kai) or
   * insignia (the Internet through NAT: Xbox Live via Insignia).
   */
  var networkMode: String
    get() = sp.getString(KEY_NETWORK, NET_OFF) ?: NET_OFF
    set(value) = sp.edit { putString(KEY_NETWORK, value) }

  /** Voice chat: player 1's controller has an Xbox Live Communicator (the phone's microphone, or the
   *  headset's), when the microphone may be used. Read at the next game start. */
  var voiceChat: Boolean
    get() = sp.getBoolean(KEY_VOICE_CHAT, false)
    set(value) = sp.edit { putBoolean(KEY_VOICE_CHAT, value) }

  /** LAN: consoles reached directly as well (another subnet, a VPN): "ip[:port], ...". */
  var lanPeers: String
    get() = sp.getString(KEY_LAN_PEERS, "") ?: ""
    set(value) = sp.edit { putString(KEY_LAN_PEERS, value) }

  /** XLink Kai: the engine's address on this network, "ip[:port]". */
  var kaiEngine: String
    get() = sp.getString(KEY_KAI_ENGINE, "") ?: ""
    set(value) = sp.edit { putString(KEY_KAI_ENGINE, value) }

  /** Insignia: the DNS server the console is given, "" Insignia's own (another: insigniaDNS on this network). */
  var insigniaDns: String
    get() = sp.getString(KEY_INSIGNIA_DNS, "") ?: ""
    set(value) = sp.edit { putString(KEY_INSIGNIA_DNS, value) }

  /** Insignia's setup wizard: the user said the Setup Assistant registered this console. */
  var insigniaRegistered: Boolean
    get() = sp.getBoolean(KEY_INSIGNIA_REGISTERED, false)
    set(value) = sp.edit { putBoolean(KEY_INSIGNIA_REGISTERED, value) }

  /** Insignia's setup wizard: the Setup Assistant ISO the user chose (a document URI), "" none. */
  var insigniaSetupUri: String
    get() = sp.getString(KEY_INSIGNIA_SETUP_URI, "") ?: ""
    set(value) = sp.edit { putString(KEY_INSIGNIA_SETUP_URI, value) }

  /** Insignia's setup wizard started the Setup Assistant or the dashboard: the app opens it again after. */
  var insigniaWizardResume: Boolean
    get() = sp.getBoolean(KEY_INSIGNIA_RESUME, false)
    set(value) = sp.edit { putBoolean(KEY_INSIGNIA_RESUME, value) }

  /** The console's language: auto (the phone's, where the Xbox has it) or one of XBOX_LANGUAGES. */
  var language: String
    get() = sp.getString(KEY_LANGUAGE, LANG_AUTO) ?: LANG_AUTO
    set(value) = sp.edit { putString(KEY_LANGUAGE, value) }

  /**
   * The console's clock: null, the phone's time; set by hand, its distance from the phone's (ms),
   * so that it runs on from what was set, as a console's does.
   */
  var clockOffset: Long?
    get() = if (sp.contains(KEY_CLOCK_OFFSET)) sp.getLong(KEY_CLOCK_OFFSET, 0L) else null
    set(value) = sp.edit { if (value == null) remove(KEY_CLOCK_OFFSET) else putLong(KEY_CLOCK_OFFSET, value) }

  /** The game about to run: NativeCoreActivity falls back to dvdUri. */
  fun rememberLaunch(uri: Uri, relativePath: String?, title: String?) {
    sp.edit(commit = true) {
      putString(KEY_DVD_URI, uri.toString())
      remove(KEY_DVD_PATH)
      putString("current_game_relative_path", relativePath)
      putString("current_game_title", title)
    }
  }

  /**
   * Startup housekeeping: the old core's BIOS, flash and disk image keys
   * (once its data is brought over: PreviousVersion reads them) and the cover
   * grid's go, and a last game or games folders whose access was revoked are
   * forgotten (with none left, the setup runs again).
   */
  fun dropStale(lastGameUsable: Boolean, usableFolders: List<Uri>) {
    val folders = gamesFolders
    if (usableFolders.size != folders.size) {
      gamesFolders = folders.filter { it in usableFolders }
    }
    val oldFiles = if (previousMigrated) listOf("mcpxUri", "mcpxPath", "flashUri", "flashPath", "hddUri", "hddPath") else emptyList()
    sp.edit {
      for (key in oldFiles + listOf("library_cover_grid", "library_box_art_lookup")) {
        remove(key)
      }
      if (!lastGameUsable) {
        remove(KEY_DVD_URI)
        remove(KEY_DVD_PATH)
      }
      if (usableFolders.isEmpty() && folders.isNotEmpty()) {
        remove(KEY_GAMES_FOLDER)
        remove(KEY_GAMES_FOLDERS)
        putBoolean(KEY_SETUP_COMPLETE, false)
      }
    }
  }

  val lastGameUri: Uri? get() = sp.getString(KEY_DVD_URI, null)?.let(Uri::parse)
  val lastGamePath: String? get() = sp.getString(KEY_DVD_PATH, null)

  companion object {
    const val NAME = "x1box_prefs"
    private const val KEY_GAMES_FOLDER = "gamesFolderUri"
    private const val KEY_GAMES_FOLDERS = "gamesFolders"
    private const val KEY_SETUP_COMPLETE = "setup_complete"
    private const val KEY_PREVIOUS_MIGRATED = "previous_version_migrated"
    private const val KEY_SHOW_FPS = "show_fps"
    private const val KEY_CONSOLE_TIMING = "console_timing"
    private const val KEY_THEME = "app_theme"
    private const val KEY_TURNIP_NOTE = "turnip_note_shown"
    const val THEME_SYSTEM = "system"
    const val THEME_LIGHT = "light"
    const val THEME_DARK = "dark"
    private const val KEY_DVD_URI = "dvdUri"
    private const val KEY_DVD_PATH = "dvdPath"
    private const val KEY_SCREEN_FORMAT = "video_screen_format"
    private const val KEY_480P = "video_480p"
    private const val KEY_720P = "video_720p"
    private const val KEY_1080I = "video_1080i"
    private const val KEY_PAL60 = "video_pal60"
    private const val KEY_AV_PACK = "video_av_pack"
    const val FORMAT_NORMAL = "normal"
    const val FORMAT_LETTERBOX = "letterbox"
    const val FORMAT_WIDESCREEN = "widescreen"
    const val AV_HDTV = "hdtv"
    const val AV_STANDARD = "standard"
    const val AV_SVIDEO = "svideo"
    private const val KEY_NETWORK = "console_network"
    private const val KEY_VOICE_CHAT = "voice_chat"
    private const val KEY_LAN_PEERS = "network_lan_peers"
    private const val KEY_KAI_ENGINE = "network_kai_engine"
    private const val KEY_INSIGNIA_DNS = "network_insignia_dns"
    private const val KEY_INSIGNIA_REGISTERED = "insignia_registered"
    private const val KEY_INSIGNIA_SETUP_URI = "insignia_setup_uri"
    private const val KEY_INSIGNIA_RESUME = "insignia_wizard_resume"
    const val NET_OFF = "off"
    const val NET_LAN = "lan"
    const val NET_KAI = "kai"
    const val NET_INSIGNIA = "insignia"
    /** Insignia's DNS server (insignia.live/connect): the console's primary. */
    const val INSIGNIA_DNS = "46.101.64.175"
    private const val KEY_LANGUAGE = "console_language"
    const val LANG_AUTO = "auto"
    private const val KEY_CLOCK_OFFSET = "console_clock_offset"

    /** The Xbox's languages, in XC_LANGUAGE order (1 English ... 9 Portuguese). */
    val XBOX_LANGUAGES = listOf("en", "ja", "de", "fr", "es", "it", "ko", "zh", "pt")

    /** The EEPROM's video flags (XC_VIDEO_FLAGS) for these video settings. */
    fun videoFlags(format: String, p480: Boolean, p720: Boolean, i1080: Boolean, pal60: Boolean): Int {
      var f = 0
      when (format) {
        FORMAT_WIDESCREEN -> f = f or 0x00010000
        FORMAT_LETTERBOX -> f = f or 0x00100000
      }
      if (p720) f = f or 0x00020000
      if (i1080) f = f or 0x00040000
      if (p480) f = f or 0x00080000
      if (pal60) f = f or 0x00400000
      return f
    }

    /** The language auto stands for: the phone's, or English when the Xbox lacks it. */
    fun autoLanguage(): String =
      Locale.getDefault().language.takeIf { it in XBOX_LANGUAGES } ?: XBOX_LANGUAGES[0]

    /** The EEPROM's language (XC_LANGUAGE: 1 English ... 9 Portuguese) for a language setting. */
    fun xboxLanguage(language: String): Int {
      val code = if (language == LANG_AUTO) autoLanguage() else language
      return XBOX_LANGUAGES.indexOf(code).coerceAtLeast(0) + 1
    }
  }
}
