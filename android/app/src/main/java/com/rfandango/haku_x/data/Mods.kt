package com.rfandango.haku_x.data

import android.content.Context
import android.content.res.AssetManager
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.util.Locale

/**
 * A mod (mods/README.md): its folder name, what it says about itself, its XBE
 * patches (file, line) and the vblank rate it asks for (0: the video mode's).
 */
data class ModInfo(
  val id: String,
  val name: String,
  val description: String,
  val author: String,
  val patches: List<Pair<String, String>>,
  val refresh: Int = 0,
)

/** A cheat (mods/README.md): its name, note, once, and its code lines as written. */
data class CheatInfo(val name: String, val note: String, val once: Boolean, val code: List<String>)

/** A disc's mods and cheats. */
class GameMods(val mods: List<ModInfo>, val cheats: List<CheatInfo>) {
  val isEmpty get() = mods.isEmpty() && cheats.isEmpty()
}

/**
 * The repository's mods/ folder, packed as the APK's assets/mods: a folder
 * per game key (GameOverrides.gameKey: title ID and certificate version,
 * 54430006.6), a cheats.txt and a folder per mod in it.
 */
object ModCatalog {
  private const val ROOT = "mods"
  private const val TAG = "hakuX-mods"
  /** Game key (upper case) -> its folder's name as the repository has it. */
  @Volatile private var folders: Map<String, String>? = null

  private fun folders(context: Context): Map<String, String> = folders ?: run {
    val map = context.assets.list(ROOT).orEmpty().associateBy { it.uppercase(Locale.ROOT) }
    folders = map
    map
  }

  private fun folder(context: Context, gameKey: String) = folders(context)[gameKey.uppercase(Locale.ROOT)]

  /** Whether the disc with this game key has mods or cheats. */
  fun has(context: Context, gameKey: String) = folder(context, gameKey) != null

  /** The disc's mods (by folder name) and cheats (in file order). */
  fun load(context: Context, gameKey: String): GameMods {
    val dir = folder(context, gameKey) ?: return GameMods(emptyList(), emptyList())
    val am = context.assets
    val base = "$ROOT/$dir"
    val mods = am.list(base).orEmpty().sorted().mapNotNull { id ->
      text(am, "$base/$id/mod.txt")?.let { parseMod(id, it) }
    }
    val cheats = text(am, "$base/cheats.txt")?.let { parseCheats(it) }.orEmpty()
    return GameMods(mods, cheats)
  }

  private fun text(am: AssetManager, path: String): String? =
    runCatching { am.open(path).use { it.readBytes().toString(Charsets.UTF_8) } }.getOrNull()

  private fun content(line: String) = line.trim().takeUnless { it.isEmpty() || it.startsWith("#") }

  private fun header(line: String): Pair<String, String>? {
    val colon = line.indexOf(':')
    if (colon <= 0) return null
    val key = line.substring(0, colon).trim().lowercase(Locale.ROOT)
    return if (key.all { it.isLetter() }) key to line.substring(colon + 1).trim() else null
  }

  private fun parseMod(id: String, text: String): ModInfo {
    val fields = mutableMapOf<String, String>()
    val patches = mutableListOf<Pair<String, String>>()
    var xbe: String? = null
    for (raw in text.lines()) {
      val line = content(raw) ?: continue
      if (line.startsWith("[") && line.endsWith("]")) {
        xbe = line.substring(1, line.length - 1).trim()
        continue
      }
      val x = xbe
      if (x != null) {
        patches += x to line
      } else {
        header(line)?.let { (k, v) -> fields[k] = v }
      }
    }
    return ModInfo(
      id = id,
      name = fields["name"]?.takeIf { it.isNotEmpty() } ?: id,
      description = fields["description"].orEmpty(),
      author = fields["author"].orEmpty(),
      patches = patches,
      refresh = fields["refresh"]?.toIntOrNull() ?: 0,
    )
  }

  private fun parseCheats(text: String): List<CheatInfo> {
    val out = mutableListOf<CheatInfo>()
    var name: String? = null
    var note = ""
    var once = false
    val code = mutableListOf<String>()
    fun flush() {
      name?.let { out += CheatInfo(it, note, once, code.toList()) }
      note = ""
      once = false
      code.clear()
    }
    for (raw in text.lines()) {
      val line = content(raw) ?: continue
      if (line.startsWith("[") && line.endsWith("]")) {
        flush()
        name = line.substring(1, line.length - 1).trim()
        continue
      }
      if (name == null) continue
      val h = header(line)
      when {
        h?.first == "note" -> note = h.second
        line.equals("once", ignoreCase = true) -> once = true
        else -> code += line
      }
    }
    flush()
    return out
  }

  /**
   * A mod's files (assets mods/<game>/<mod>/files/...) in the app's storage
   * for the core to read, extracted again after the app changed: their disc
   * paths and host files.
   */
  fun extract(context: Context, gameKey: String, mod: String): List<Pair<String, File>> {
    val dir = folder(context, gameKey) ?: return emptyList()
    val am = context.assets
    val src = "$ROOT/$dir/$mod/files"
    val dst = File(context.filesDir, "mods/${gameKey.uppercase(Locale.ROOT)}/$mod")
    val files = File(dst, "files")
    val stamp = File(dst, ".stamp")
    val app = context.packageManager.getPackageInfo(context.packageName, 0).lastUpdateTime.toString()
    if (stamp.isFile && stamp.readText() == app) {
      return files.walkTopDown().filter { it.isFile }.map { it.relativeTo(files).path to it }.toList()
    }
    dst.deleteRecursively()
    val out = mutableListOf<Pair<String, File>>()
    fun copy(asset: String, rel: String) {
      val children = am.list(asset).orEmpty()
      if (children.isEmpty()) {
        val target = File(files, rel)
        target.parentFile?.mkdirs()
        runCatching { am.open(asset).use { i -> target.outputStream().use { i.copyTo(it) } } }
          .onSuccess { out += rel to target }
          .onFailure { Log.w(TAG, "$asset: ${it.message}") }
        return
      }
      for (c in children) copy("$asset/$c", if (rel.isEmpty()) c else "$rel/$c")
    }
    if (am.list(src).orEmpty().isNotEmpty()) copy(src, "")
    dst.mkdirs()
    stamp.writeText(app)
    return out
  }
}

/**
 * One disc's choices: its mods on or off, its cheats on or off, by name. In
 * one JSON file ("<game>/mod/<id>" or "<game>/cheat/<name>" -> true) both
 * processes read afresh and write whole, renamed into place, as Controllers:
 * the game's menu toggles cheats in its own process, and shared preferences
 * served the Mods screen its cached copy, whose next toggle wrote the
 * game's changes away.
 */
class ModChoices(context: Context, private val game: String) {
  private val file = File(context.filesDir, FILE)
  private val app = context.applicationContext

  fun mod(id: String) = read().optBoolean("$game/mod/$id", false)
  fun setMod(id: String, on: Boolean) = write("$game/mod/$id", on)
  fun cheat(name: String) = read().optBoolean("$game/cheat/$name", false)
  fun setCheat(name: String, on: Boolean) = write("$game/cheat/$name", on)

  private fun read(): JSONObject = synchronized(LOCK) {
    if (!file.exists()) {
      // the choices the shared preferences (per_game_mods) held before; the file is left as it was
      val old = app.getSharedPreferences(NAME, Context.MODE_PRIVATE).all
      return JSONObject().apply { for ((k, v) in old) if (v is Boolean) put(k, v) }
    }
    try {
      JSONObject(file.readText())
    } catch (_: Exception) {
      JSONObject()
    }
  }

  private fun write(key: String, on: Boolean) {
    synchronized(LOCK) {
      val o = read()
      if (on) o.put(key, true) else o.remove(key)
      val tmp = File(file.parentFile, "$FILE.${android.os.Process.myPid()}.tmp")
      tmp.writeText(o.toString())
      tmp.renameTo(file)
    }
  }

  companion object {
    const val NAME = "per_game_mods"
    private const val FILE = "mod_choices.json"
    private val LOCK = Any()
  }
}

/**
 * The run's manifest for the core (native/core/mods.h): the enabled mods'
 * files and XBE patches, every cheat with its state. The cheats in their
 * order here are the in-game menu's (nativeSetCheat takes the index).
 */
object ModRun {
  private const val TAG = "hakuX-mods"

  /** The manifest written (null: the game has none), and the run's cheats. */
  fun write(context: Context, gameKey: String): Pair<File, List<CheatInfo>>? {
    val game = ModCatalog.load(context, gameKey)
    if (game.isEmpty) return null
    val choices = ModChoices(context, gameKey)
    val lines = mutableListOf<String>()
    val on = game.mods.filter { choices.mod(it.id) }
    for (mod in on) {
      for ((disc, file) in ModCatalog.extract(context, gameKey, mod.id)) {
        lines += "file\t${disc.replace('/', '\\')}\t${file.absolutePath}"
      }
    }
    for (mod in game.mods) {
      // a mod that is off too: a savestate saved with it on gets the disc's bytes back
      val kind = if (mod in on) "patch" else "patchoff"
      for ((xbe, line) in mod.patches) lines += "$kind\t$xbe\t${mod.id}\t$line"
    }
    on.maxOfOrNull { it.refresh }?.takeIf { it > 0 }?.let { lines += "refresh\t$it" }
    for (c in game.cheats) {
      lines += "cheat\t${if (choices.cheat(c.name)) 1 else 0}\t${if (c.once) 1 else 0}\t${c.name}"
      for (l in c.code) lines += "code\t$l"
    }
    val file = File(DataRoot.native(context), "mods.txt")
    file.parentFile?.mkdirs()
    file.writeText(lines.joinToString("\n", postfix = "\n"))
    Log.i(TAG, "$gameKey: ${on.size} of ${game.mods.size} mods on, ${game.cheats.size} cheats")
    return file to game.cheats
  }
}
