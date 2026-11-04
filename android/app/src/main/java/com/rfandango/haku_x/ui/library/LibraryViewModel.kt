package com.rfandango.haku_x.ui.library

import android.app.Application
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.SystemClock
import android.util.Log
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.rfandango.haku_x.data.AppPrefs
import com.rfandango.haku_x.data.DiscInfo
import com.rfandango.haku_x.data.DiscInfoCache
import com.rfandango.haku_x.data.Game
import com.rfandango.haku_x.data.GameScanner
import com.rfandango.haku_x.data.Storage
import com.rfandango.haku_x.data.XisoConverter
import com.rfandango.haku_x.data.hdd.Dashboard
import com.rfandango.haku_x.data.hdd.HddStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.isActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.Locale

/** The dashboard's install from the library: done of total bytes, what it is at; finished, with error or not. */
data class DashboardInstall(
  val done: Long = 0,
  val total: Long = 1,
  val what: String = "",
  val finished: Boolean = false,
  val error: String? = null,
)

data class LibraryState(
  val folderReady: Boolean = false,
  /** Refreshing, behind the loading dialog: the list stays empty until it's done. */
  val loading: Boolean = false,
  /** While loading: the discs not read before (0 while the folder is listed), and how many are read. */
  val toRead: Int = 0,
  val read: Int = 0,
  /** By title: the disc's own, else the file's name. */
  val games: List<Game> = emptyList(),
  val query: String = "",
  /** What the discs read so far say (ID, regions, icon), by the game's URI. */
  val info: Map<Uri, DiscInfo> = emptyMap(),
  /** The discs' icons, decoded (LibraryViewModel.loadIcons), by the game's URI. */
  val icons: Map<Uri, ImageBitmap> = emptyMap(),
  /** A conversion to XISO: asked, under way, or its outcome. */
  val conversion: Conversion? = null,
  /** The dashboard is on the hard disk's C: (Dashboard): its button starts it, else installs it. */
  val dashboard: Boolean = false,
  /** The dashboard's install from its zip: under way (done of total), or its outcome. */
  val dashboardInstall: DashboardInstall? = null,
) {
  val visibleGames: List<Game>
    get() {
      val shown = distinctTitles(onePerDisc())
      return if (query.isBlank()) shown else shown.filter { it.title.contains(query.trim(), ignoreCase = true) }
    }

  /**
   * One card per disc (its title ID and certificate version: one title's
   * discs, like Dead or Alive Ultimate's, share the ID): an XISO over a full
   * dump of the same disc, and among several, the first by file name. Images
   * not read yet (or without an ID) all show.
   */
  private fun onePerDisc(): List<Game> {
    val chosen = games
      .filter { (info[it.uri]?.titleId ?: 0L) != 0L }
      .groupBy { info.getValue(it.uri).release }
      .values
      .mapTo(HashSet()) { same ->
        val xisos = same.filter { !info.getValue(it.uri).isFullDump }
        xisos.ifEmpty { same }.minBy { it.relativePath.substringAfterLast('/').lowercase(Locale.ROOT) }.uri
      }
    return games.filter { (info[it.uri]?.titleId ?: 0L) == 0L || it.uri in chosen }
  }

  /**
   * Cards that would share a title show their files' names instead: the
   * certificate names the title, not the disc (both of Dead or Alive
   * Ultimate's say DEAD OR ALIVE ULTIMATE). Without the brackets first; with
   * them when that still leaves two the same ("Halo (USA)", "Halo (Europe)").
   */
  private fun distinctTitles(shown: List<Game>): List<Game> {
    var out = shown
    for (brackets in listOf(false, true)) {
      val same = out.groupingBy { it.title.lowercase(Locale.ROOT) }.eachCount().filterValues { it > 1 }.keys
      if (same.isEmpty()) break
      out = out.map {
        if (it.title.lowercase(Locale.ROOT) in same) it.copy(title = GameScanner.fileTitle(it.relativePath, brackets)) else it
      }
    }
    return if (out === shown) shown else out.sortedBy { it.title.lowercase(Locale.ROOT) }
  }
}

data class Conversion(
  val game: Game,
  val phase: Phase,
  /** The XISO's file name. */
  val output: String,
  val done: Long = 0,
  val total: Long = 0,
  /** Done: whether the original went into XisoConverter.ORIGINALS_FOLDER. */
  val originalMoved: Boolean = true,
  /** Failed: why. */
  val error: String? = null,
) {
  enum class Phase { CONFIRM, RUNNING, DONE, FAILED }
}

class LibraryViewModel(app: Application) : AndroidViewModel(app) {
  private val prefs = AppPrefs(app)
  private val discInfo = DiscInfoCache(app)
  private val hddRoot = HddStore(app).root
  private var scanJob: Job? = null

  private val _state = MutableStateFlow(LibraryState())
  val state: StateFlow<LibraryState> = _state.asStateFlow()

  init {
    refresh()
  }

  /** Back on screen: the games folders may have changed in the settings, the dashboard come or gone (Hard disk). */
  fun onResume() {
    if (prefs.gamesFolders != scannedFolders) {
      refresh()
    }
    _state.update { it.copy(dashboard = Dashboard.installed(hddRoot)) }
  }

  /** The dashboard's files from the zip at uri onto C: (the library's dashboard button, none installed). */
  fun installDashboard(uri: Uri) {
    if (_state.value.dashboardInstall?.finished == false) return
    _state.update { it.copy(dashboardInstall = DashboardInstall()) }
    val app = getApplication<Application>()
    viewModelScope.launch {
      val r = withContext(Dispatchers.IO) {
        runCatching {
          Dashboard.install({ app.contentResolver.openInputStream(uri)!! }, hddRoot) { d, t, w ->
            _state.update { s -> s.copy(dashboardInstall = s.dashboardInstall?.copy(done = d, total = t, what = w)) }
          }
        }
      }
      _state.update {
        it.copy(
          dashboard = Dashboard.installed(hddRoot),
          dashboardInstall = it.dashboardInstall?.copy(
            finished = true,
            error = r.exceptionOrNull()?.let { e -> e.message ?: e.javaClass.simpleName },
          ),
        )
      }
    }
  }

  fun dismissDashboardInstall() {
    if (_state.value.dashboardInstall?.finished == true) _state.update { it.copy(dashboardInstall = null) }
  }

  private var scannedFolders: List<Uri> = emptyList()

  /**
   * Lists the games folders again, behind the loading dialog: no game shows
   * until every disc is known. Discs read before come from the cache; the
   * others are read one by one. A folder that can't be read now (its card
   * out) is left out until it can.
   */
  fun refresh() {
    val app = getApplication<Application>()
    val folders = prefs.gamesFolders
    scannedFolders = folders
    scanJob?.cancel()
    scanJob = viewModelScope.launch {
      _state.update {
        it.copy(loading = true, toRead = 0, read = 0, games = emptyList(), info = emptyMap(), icons = emptyMap())
      }
      val ready = withContext(Dispatchers.IO) { folders.filter { Storage.isFolderReady(app, it) } }
      if (ready.isEmpty()) {
        _state.update { it.copy(folderReady = false, loading = false) }
        return@launch
      }
      _state.update { it.copy(folderReady = true) }
      val t0 = SystemClock.elapsedRealtime()
      val games = withContext(Dispatchers.IO) { GameScanner.scan(app, ready) }
      Log.i("hakuX-library", "scan: ${games.size} games in ${ready.size} folders, ${SystemClock.elapsedRealtime() - t0} ms")
      val info = withContext(Dispatchers.IO) {
        games.mapNotNull { g -> discInfo.cached(g)?.let { g.uri to it } }.toMap(HashMap())
      }
      if (games.isNotEmpty()) {
        withContext(Dispatchers.IO) { discInfo.retainOnly(games) }
      }
      val toRead = games.filter { it.uri !in info }
      _state.update { it.copy(toRead = toRead.size) }
      toRead.forEachIndexed { i, game ->
        withContext(Dispatchers.IO) { discInfo.read(game) }?.let { info[game.uri] = it }
        _state.update { it.copy(read = i + 1) }
      }
      val icons = withContext(Dispatchers.IO) { loadIcons(info) }
      _state.update { s ->
        s.copy(
          loading = false,
          games = sorted(games.map { g -> info[g.uri]?.title?.let { g.copy(title = it) } ?: g }),
          info = info.toMap(),
          icons = icons,
        )
      }
    }
  }

  @Volatile private var cancelConversion = false

  /** Asks to convert a full dump to an XISO (the long press menu). */
  fun askConvert(game: Game) = _state.update {
    val name = XisoConverter.outputName(game.relativePath.substringAfterLast('/'))
    it.copy(conversion = Conversion(game, Conversion.Phase.CONFIRM, name))
  }

  /** Closes the conversion dialog (not while one runs: that's cancelConversion). */
  fun dismissConversion() = _state.update {
    if (it.conversion?.phase == Conversion.Phase.RUNNING) it else it.copy(conversion = null)
  }

  fun cancelConversion() {
    cancelConversion = true
  }

  /**
   * Converts the asked game. The XISO inherits the dump's disc info (the
   * cache entry moves to it), then the folder is listed again.
   */
  fun startConversion() {
    val c = _state.value.conversion?.takeIf { it.phase == Conversion.Phase.CONFIRM } ?: return
    val folder = c.game.folder
    val app = getApplication<Application>()
    val info = _state.value.info[c.game.uri]
    cancelConversion = false
    _state.update { it.copy(conversion = c.copy(phase = Conversion.Phase.RUNNING)) }
    viewModelScope.launch {
      val result = withContext(Dispatchers.IO) {
        val context = currentCoroutineContext()
        var shown = 0L
        XisoConverter.convert(
          app, folder, c.game,
          progress = { done, total ->
            val now = SystemClock.uptimeMillis()
            if (now - shown >= 250 || done == total) {
              shown = now
              _state.update { s -> s.copy(conversion = s.conversion?.copy(done = done, total = total)) }
            }
          },
          cancelled = { cancelConversion || !context.isActive },
        )
      }
      when (result) {
        is XisoConverter.Result.Done -> {
          if (info != null) {
            withContext(Dispatchers.IO) {
              val dir = c.game.relativePath.substringBeforeLast('/', "")
              val path = if (dir.isEmpty()) c.output else "$dir/${c.output}"
              discInfo.converted(c.game, info, Game(c.game.title, result.xiso.uri, path, result.xiso.length(), folder))
            }
          }
          _state.update {
            it.copy(conversion = c.copy(phase = Conversion.Phase.DONE, originalMoved = result.originalMoved))
          }
          refresh()
        }
        is XisoConverter.Result.Failed ->
          _state.update { it.copy(conversion = c.copy(phase = Conversion.Phase.FAILED, error = result.message)) }
        XisoConverter.Result.Cancelled -> _state.update { it.copy(conversion = null) }
      }
    }
  }

  fun setQuery(query: String) = _state.update { it.copy(query = query) }

  private fun sorted(games: List<Game>) = games.sortedBy { it.title.lowercase(Locale.ROOT) }

  /**
   * The disc icons (small PNGs in the disc info cache), decoded once off the
   * main thread and uploaded ahead of drawing. A card that scrolls in then
   * draws a bitmap it already has: an image request per card, set up on the
   * main thread as each one composed, was part of what made fast scrolls
   * stutter. Blocking.
   */
  private fun loadIcons(info: Map<Uri, DiscInfo>): Map<Uri, ImageBitmap> {
    val icons = HashMap<Uri, ImageBitmap>()
    for ((uri, disc) in info) {
      val bitmap = disc.icon?.let { BitmapFactory.decodeFile(it.path) } ?: continue
      bitmap.prepareToDraw()
      icons[uri] = bitmap.asImageBitmap()
    }
    return icons
  }
}
