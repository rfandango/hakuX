package com.rfandango.haku_x.data

import android.content.Context
import android.content.res.AssetManager
import android.net.Uri
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
  /** The regions its "region:" line lists (ModRegions); 0: no such line, it fits every disc. */
  val regions: Int = 0,
  /** Added by the user in the app (filesDir/user_mods), not packed in the APK. */
  val user: Boolean = false,
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
 * 54430006.6, or a title ID alone), a cheats.txt and a folder per mod in it.
 */
object ModCatalog {
  private const val ROOT = "mods"
  /** A game key of a disc release: its title ID and its certificate version (54430006.6). */
  private val RELEASE_KEY = Regex("[0-9A-F]{8}\\.\\d+")
  private const val TAG = "hakuX-mods"
  /** Game key (upper case) -> its folder's name as the repository has it. */
  @Volatile private var folders: Map<String, String>? = null

  private fun folders(context: Context): Map<String, String> = folders ?: run {
    val map = context.assets.list(ROOT).orEmpty().associateBy { it.uppercase(Locale.ROOT) }
    folders = map
    map
  }

  /**
   * The packed folders of a game key, the title's first and then the release's:
   * 54430006 holds the title's mods that each say the regions they fit (their
   * "region:" line), 54430006.6 what fits that release only.
   */
  private fun gameFolders(context: Context, gameKey: String): List<String> {
    val key = gameKey.uppercase(Locale.ROOT)
    val all = folders(context)
    val title = if (RELEASE_KEY.matches(key)) all[key.substringBefore('.')] else null
    return listOfNotNull(title, all[key])
  }

  /** The user's own mods and cheats for a game: filesDir/user_mods/<GAME KEY>/<mod>/mod.txt, cheats.txt. */
  private fun userDir(context: Context, gameKey: String) =
    File(context.filesDir, "user_mods/${gameKey.uppercase(Locale.ROOT)}")

  /** Whether the disc with this game key has mods or cheats (packed, or the user's). */
  fun has(context: Context, gameKey: String) =
    gameFolders(context, gameKey).isNotEmpty() || userDir(context, gameKey).list().orEmpty().isNotEmpty()

  /**
   * The disc's mods (the packed ones by folder name, then the user's) and cheats (in file order).
   * A mod that lists regions is the disc's only if the disc is made for one of them: discRegions
   * are the disc's (XboxDisc.regions, 0 when not known: only the mods that list none).
   */
  fun load(context: Context, gameKey: String, discRegions: Int): GameMods {
    val am = context.assets
    val dirs = gameFolders(context, gameKey)
    val packed = dirs.flatMap { dir ->
      am.list("$ROOT/$dir").orEmpty().sorted().mapNotNull { id ->
        text(am, "$ROOT/$dir/$id/mod.txt")?.let { parseMod(id, it) }
      }
    }.associateBy { it.id }.values.sortedBy { it.id }.filter { ModRegions.fits(it.regions, discRegions) }
    val packedCheats = dirs.flatMap { dir -> text(am, "$ROOT/$dir/cheats.txt")?.let { parseCheats(it) }.orEmpty() }
    val user = userDir(context, gameKey)
    val userMods = user.listFiles().orEmpty().filter { it.isDirectory }.sortedBy { it.name }.mapNotNull { d ->
      runCatching { File(d, "mod.txt").takeIf { it.isFile }?.readText() }.getOrNull()
        ?.let { parseMod("user-${d.name}", it).copy(user = true) }
    }.filter { ModRegions.fits(it.regions, discRegions) }
    val userCheats = runCatching { File(user, "cheats.txt").takeIf { it.isFile }?.readText() }.getOrNull()
      ?.let { parseCheats(it) }.orEmpty()
    return GameMods(packed + userMods, packedCheats + userCheats)
  }

  /**
   * Saves a mod the user pasted: its patch lines ("0xADDRESS: <bytes>", "offset 0xPOSITION: <bytes>"
   * or "search <bytes> -> <bytes>") on their own, which go under [default.xbe], or a whole mod.txt.
   * Returns its id.
   */
  fun saveUserMod(context: Context, gameKey: String, name: String, pasted: String): String {
    val title = name.trim().ifEmpty { "Custom mod" }
    val lines = pasted.lines()
    val hasSection = lines.any { val t = it.trim(); t.startsWith("[") && t.endsWith("]") }
    val hasName = lines.any { it.trim().lowercase(Locale.ROOT).startsWith("name:") }
    val body = buildString {
      if (!hasName) append("name: ").append(title).append('\n')
      if (!hasSection) append("[default.xbe]\n")
      append(pasted.trim()).append('\n')
    }
    val root = userDir(context, gameKey)
    val slug = title.lowercase(Locale.ROOT).replace(Regex("[^a-z0-9]+"), "-").trim('-').ifEmpty { "mod" }
    var folder = slug
    var n = 2
    while (File(root, folder).exists()) folder = "$slug-${n++}"
    val dir = File(root, folder)
    dir.mkdirs()
    File(dir, "mod.txt").writeText(body)
    return "user-$folder"
  }

  /** Removes a mod the user added (its id as load gives it). */
  fun deleteUserMod(context: Context, gameKey: String, id: String) {
    if (!id.startsWith("user-")) return
    val dir = File(userDir(context, gameKey), id.removePrefix("user-"))
    if (dir.parentFile?.name.equals(gameKey, ignoreCase = true)) dir.deleteRecursively()
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
      regions = fields["region"]?.let { ModRegions.parse(it) } ?: 0,
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
    val am = context.assets
    val dir = gameFolders(context, gameKey).lastOrNull { am.list("$ROOT/$it").orEmpty().contains(mod) } ?: return emptyList()
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

  /**
   * The address patches of the "offset" and "search" lines of a mod, by (XBE, line): each becomes
   * "0xADDRESS: bytes" lines that the core takes, from default.xbe as the disc has it. A line that
   * doesn't fit the disc (a position the file doesn't have, bytes that are not there: another build
   * of the game) is an empty list. Other lines aren't in the map.
   */
  private fun resolveXbePatches(context: Context, disc: Uri?, mods: List<ModInfo>): Map<Pair<String, String>, List<String>> {
    val out = HashMap<Pair<String, String>, List<String>>()
    val todo = LinkedHashMap<Pair<String, String>, Triple<String, String, XbePatch>>()
    for (mod in mods) {
      for ((xbe, line) in mod.patches) {
        val key = xbe to line
        if (key in out || key in todo) continue
        val patch = XbePatch.parse(line) ?: continue
        if (xbe.equals("default.xbe", ignoreCase = true)) {
          todo[key] = Triple(mod.id, line, patch)
        } else {
          Log.w(TAG, "${mod.id}: [$xbe] offset and search lines work in [default.xbe] only")
          out[key] = emptyList()
        }
      }
    }
    if (todo.isEmpty()) return out
    val image = disc?.let { XboxDisc.open(context, it) { d -> d.xbeImage() } }
    if (image == null) Log.w(TAG, "default.xbe can't be read from the disc: its offset and search lines are skipped")
    for ((key, v) in todo) {
      val (id, line, patch) = v
      val lines = image?.let { patch.resolve(it) }.orEmpty()
      if (image != null && lines.isEmpty()) {
        val why = if (patch is XbePatch.Search) "no match on this disc" else "not in this disc's XBE file"
        Log.w(TAG, "$id: \"${line.take(60)}\": $why")
      }
      out[key] = lines
    }
    return out
  }

  /**
   * The manifest written (null: the game has none), and the run's cheats. disc is the game's
   * image: its regions say which mods are the game's, and its default.xbe is where the "offset"
   * and "search" lines land. A mod with one that doesn't fit the disc is left out whole (a patch
   * made for another build of the game, half done, could break it).
   */
  fun write(context: Context, gameKey: String, disc: Uri? = null): Pair<File, List<CheatInfo>>? {
    val regions = disc?.let { XboxDisc.open(context, it) { d -> d.regions } } ?: 0
    val game = ModCatalog.load(context, gameKey, regions)
    if (game.isEmpty) return null
    val choices = ModChoices(context, gameKey)
    val lines = mutableListOf<String>()
    val on = game.mods.filter { choices.mod(it.id) }
    for (mod in on.filter { !it.user }) {
      for ((onDisc, file) in ModCatalog.extract(context, gameKey, mod.id)) {
        lines += "file\t${onDisc.replace('/', '\\')}\t${file.absolutePath}"
      }
    }
    val resolved = resolveXbePatches(context, disc, game.mods)
    for (mod in game.mods) {
      if (mod.patches.any { (xbe, line) -> resolved[xbe to line]?.isEmpty() == true }) {
        Log.w(TAG, "${mod.id}: doesn't fit this disc, left out")
        continue
      }
      // a mod that is off too: a savestate saved with it on gets the disc's bytes back
      val kind = if (mod in on) "patch" else "patchoff"
      for ((xbe, line) in mod.patches) {
        for (p in resolved[xbe to line] ?: listOf(line)) lines += "$kind\t$xbe\t${mod.id}\t$p"
      }
    }
    on.maxOfOrNull { it.refresh }?.takeIf { it > 0 }?.let { lines += "refresh\t$it" }
    for (c in game.cheats) {
      lines += "cheat\t${if (choices.cheat(c.name)) 1 else 0}\t${if (c.once) 1 else 0}\t${c.name}"
      for (l in c.code) lines += "code\t$l"
    }
    val file = File(context.filesDir, "native/mods.txt")
    file.parentFile?.mkdirs()
    file.writeText(lines.joinToString("\n", postfix = "\n"))
    Log.i(TAG, "$gameKey: ${on.size} of ${game.mods.size} mods on, ${game.cheats.size} cheats")
    return file to game.cheats
  }
}
