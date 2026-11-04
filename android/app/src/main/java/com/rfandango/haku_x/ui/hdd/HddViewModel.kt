package com.rfandango.haku_x.ui.hdd

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.rfandango.haku_x.R
import com.rfandango.haku_x.data.AppPrefs
import com.rfandango.haku_x.data.PreviousVersion
import com.rfandango.haku_x.data.hdd.Backup
import com.rfandango.haku_x.data.hdd.ConsoleFiles
import com.rfandango.haku_x.data.hdd.Dashboard
import com.rfandango.haku_x.data.hdd.DiskImport
import com.rfandango.haku_x.data.hdd.HddStore
import com.rfandango.haku_x.data.hdd.LibraryKeys
import com.rfandango.haku_x.data.hdd.SaveArchive
import com.rfandango.haku_x.data.hdd.SaveResigner
import com.rfandango.haku_x.data.hdd.XboxHdd
import java.io.File
import java.io.IOException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** The hard disk screen's state: what the disk holds, the console files a console's or xemu's data
 *  comes in with, a long job (image import, backup, restore), and a notice to show. */
class HddViewModel(app: Application) : AndroidViewModel(app) {
  val store = HddStore(app)
  private val console = ConsoleFiles(app)

  data class Contents(
    val usage: HddStore.Usage? = null,
    val saves: List<HddStore.Title> = emptyList(),
    val titleData: List<HddStore.Title> = emptyList(),
    val caches: List<HddStore.Cache> = emptyList(),
    val states: List<HddStore.States> = emptyList(),
    val loading: Boolean = true,
    val console: ConsoleFiles.Status? = null,
    /** The xemu-based release's disk image, brought over already: there to delete. */
    val previousImage: File? = null,
    /** The dashboard on C: (Dashboard): Xbox Live's account sign-up. */
    val dashboard: Boolean = false,
  )

  /** A long job: a disk image's import, a backup, a restore, the dashboard's install. */
  enum class Kind { IMAGE_IMPORT, BACKUP, RESTORE, DASHBOARD }

  /** A long job: running (done of total), or finished (result or error). */
  data class Job(
    val kind: Kind,
    val done: Long = 0,
    val total: Long = 1,
    val what: String = "",
    val result: XboxHdd.Result? = null,
    /** An image import's saves re-signed for hakuX. */
    val resign: SaveResigner.Report? = null,
    val error: String? = null,
  ) {
    val finished get() = result != null || error != null
  }

  private val _contents = MutableStateFlow(Contents())
  val contents: StateFlow<Contents> = _contents
  private val _job = MutableStateFlow<Job?>(null)
  val job: StateFlow<Job?> = _job
  private val _notice = MutableStateFlow<String?>(null)
  val notice: StateFlow<String?> = _notice

  /**
   * A save import waiting for the user's yes: the zip and what it holds. foreign: not a hakuX zip (a
   * console's or xemu's), its saves to re-sign; canResign: the console files give the keys for it.
   */
  class SaveImport(val uri: Uri, val plan: SaveArchive.Plan, val canResign: Boolean) {
    val foreign get() = plan.kind == null
  }

  private val _saveImport = MutableStateFlow<SaveImport?>(null)
  val saveImport: StateFlow<SaveImport?> = _saveImport

  /** A restore waiting for the user's yes: the backup and its marker. */
  class Restore(val uri: Uri, val info: Backup.Info)

  private val _restore = MutableStateFlow<Restore?>(null)
  val restore: StateFlow<Restore?> = _restore

  init {
    refresh()
  }

  fun refresh() {
    viewModelScope.launch {
      val c = withContext(Dispatchers.IO) {
        Contents(store.usage(), store.saves(), store.titleData(), store.caches(), store.states(), false,
                 console.status(), previousImage(), Dashboard.installed(store.root))
      }
      _contents.value = c
    }
  }

  fun delete(dir: File) = io { store.delete(dir) }

  fun clearCache(part: Int) = io { store.clearCache(part) }

  fun clearAllCaches() = io { (3..5).forEach(store::clearCache) }

  private fun io(work: () -> Unit) {
    viewModelScope.launch {
      withContext(Dispatchers.IO) { runCatching(work) }
      refresh()
    }
  }

  /**
   * An Xbox disk image (raw or qcow2) at uri imported over the native core's disk, its saves re-signed
   * for hakuX with the keys the console files give (the image's console: its EEPROM).
   */
  fun import(uri: Uri) {
    if (_job.value?.finished == false) return
    _job.value = Job(Kind.IMAGE_IMPORT)
    val app = getApplication<Application>()
    viewModelScope.launch {
      var report: SaveResigner.Report? = null
      val r = withContext(Dispatchers.IO) {
        runCatching {
          val resigner = console.resigner() ?: throw IOException(app.getString(R.string.hdd_console_needed))
          val o = DiskImport.open(app, null, uri) { image ->
            DiskImport.run(app, image, resigner, { d, t, w -> _job.update { it?.copy(done = d, total = t, what = w) } }) {
              _job.update { it?.copy(what = app.getString(R.string.hdd_resigning)) }
            }
          }
          report = o.resign
          o.result
        }
      }
      _job.update { it?.copy(result = r.getOrNull(), resign = report, error = r.exceptionOrNull()?.let(::message)) }
      refresh()
    }
  }

  /** The dashboard's files from the zip at uri into C: (Dashboard.install). */
  fun installDashboard(uri: Uri) {
    if (_job.value?.finished == false) return
    _job.value = Job(Kind.DASHBOARD)
    val app = getApplication<Application>()
    viewModelScope.launch {
      val r = withContext(Dispatchers.IO) {
        runCatching {
          Dashboard.install({ app.contentResolver.openInputStream(uri)!! }, store.root) { d, t, w ->
            _job.update { it?.copy(done = d, total = t, what = w) }
          }
        }
      }
      _job.update { it?.copy(result = r.getOrNull(), error = r.exceptionOrNull()?.let(::message)) }
      refresh()
    }
  }

  /** A console file (MCPX boot ROM, BIOS, EEPROM) picked by the user. */
  fun setConsoleFile(kind: ConsoleFiles.Kind, uri: Uri) {
    val app = getApplication<Application>()
    viewModelScope.launch {
      val r = withContext(Dispatchers.IO) {
        runCatching { app.contentResolver.openInputStream(uri)!!.use { console.set(kind, it) } }
      }
      r.exceptionOrNull()?.let { _notice.value = app.getString(R.string.hdd_console_bad, message(it)) }
      refresh()
    }
  }

  /** Everything the native core keeps of the console backed up into uri (Backup). */
  fun backup(uri: Uri, states: Boolean, caches: Boolean) {
    if (_job.value?.finished == false) return
    _job.value = Job(Kind.BACKUP)
    val app = getApplication<Application>()
    viewModelScope.launch {
      val r = withContext(Dispatchers.IO) {
        runCatching {
          app.contentResolver.openOutputStream(uri, "wt")!!.use { out ->
            Backup.write(out, store.native, states, caches) { d, t, w -> _job.update { it?.copy(done = d, total = t, what = w) } }
          }
        }
      }
      _job.update { it?.copy(result = r.getOrNull(), error = r.exceptionOrNull()?.let(::message)) }
    }
  }

  /** A backup at uri read for its marker; the screen asks before restore() puts it in place. */
  fun planRestore(uri: Uri) {
    val app = getApplication<Application>()
    viewModelScope.launch {
      val info = withContext(Dispatchers.IO) {
        runCatching { app.contentResolver.openInputStream(uri)!!.use { Backup.info(it) } }.getOrNull()
      }
      if (info?.kind == "backup") _restore.value = Restore(uri, info)
      else _notice.value = app.getString(R.string.hdd_backup_not)
    }
  }

  fun restore() {
    val s = _restore.value ?: return
    _restore.value = null
    if (_job.value?.finished == false) return
    _job.value = Job(Kind.RESTORE)
    val app = getApplication<Application>()
    viewModelScope.launch {
      val r = withContext(Dispatchers.IO) {
        runCatching {
          val size = runCatching { app.contentResolver.openAssetFileDescriptor(s.uri, "r")!!.use { it.length } }.getOrDefault(-1L)
          app.contentResolver.openInputStream(s.uri)!!.use { input ->
            Backup.restore(input, size, store.native) { d, t, w -> _job.update { it?.copy(done = d, total = t, what = w) } }
          }
        }
      }
      _job.update { it?.copy(result = r.getOrNull(), error = r.exceptionOrNull()?.let(::message)) }
      refresh()
    }
  }

  fun cancelRestore() {
    _restore.value = null
  }

  /**
   * Game saves exported into uri as a zip (SaveArchive): which = "" for every title's, a title id for
   * that title's, "<title id>/<save folder>" for one save.
   */
  fun exportSaves(uri: Uri, which: String) {
    val id = which.substringBefore('/')
    val save = which.substringAfter('/', "")
    val picks = _contents.value.saves.filter { which.isEmpty() || it.id == id }.map { t ->
      SaveArchive.Pick(t.dir, if (save.isEmpty()) null else t.saves.filter { it.dir.name == save }.map { it.dir })
    }
    val app = getApplication<Application>()
    viewModelScope.launch {
      val r = withContext(Dispatchers.IO) {
        runCatching { app.contentResolver.openOutputStream(uri, "wt")!!.use { SaveArchive.write(it, picks) } }
      }
      _notice.value = r.fold(
        { app.resources.getQuantityString(R.plurals.hdd_saves_exported, it, it) },
        { app.getString(R.string.hdd_saves_export_failed, message(it)) },
      )
    }
  }

  /** A zip of saves at uri read for what it holds; the screen asks before importSaves() writes it. */
  fun planSaveImport(uri: Uri) {
    val app = getApplication<Application>()
    viewModelScope.launch {
      val r = withContext(Dispatchers.IO) {
        runCatching {
          app.contentResolver.openInputStream(uri)!!.use { SaveArchive.plan(it, partition1()) } to console.status().ready
        }
      }
      r.fold(
        { (p, keys) ->
          if (p.files == 0) _notice.value = app.getString(R.string.hdd_saves_import_none)
          else _saveImport.value = SaveImport(uri, p, keys)
        },
        { _notice.value = app.getString(R.string.hdd_saves_import_failed, message(it)) },
      )
    }
  }

  fun importSaves() {
    val s = _saveImport.value ?: return
    _saveImport.value = null
    val app = getApplication<Application>()
    viewModelScope.launch {
      val r = withContext(Dispatchers.IO) {
        runCatching {
          val e = partition1()
          val res = app.contentResolver.openInputStream(s.uri)!!.use { SaveArchive.extract(it, e) }
          // a console's or xemu's saves: re-signed for hakuX, the files written only
          val rep = if (!s.foreign) null else console.resigner()?.let { rs ->
            val ids = res.written.mapNotNull { f ->
              f.relativeTo(e).path.replace('\\', '/').split('/').getOrNull(1)?.toLongOrNull(16)
            }.toSet()
            rs.resign(e, res.written, LibraryKeys.load(app, ids)::get)
          }
          res to rep
        }
      }
      _notice.value = r.fold(
        { (res, rep) ->
          buildString {
            append(app.resources.getQuantityString(R.plurals.hdd_saves_imported, res.files, res.files))
            if (rep != null) {
              append(' ').append(app.resources.getQuantityString(R.plurals.hdd_resigned, rep.resigned, rep.resigned))
              if (rep.noKey.isNotEmpty()) append(' ').append(app.getString(R.string.hdd_resign_nokey, titleNames(rep.noKey)))
            }
          }
        },
        { app.getString(R.string.hdd_saves_import_failed, message(it)) },
      )
      refresh()
    }
  }

  fun cancelSaveImport() {
    _saveImport.value = null
  }

  /** The xemu-based release's disk image, once its data came over (PreviousVersion). */
  private fun previousImage(): File? {
    val app = getApplication<Application>()
    return if (AppPrefs(app).previousMigrated) PreviousVersion(app).leftoverImage() else null
  }

  fun deletePreviousImage() = io { previousImage()?.delete() }

  /** Titles by name where the disk has them (TitleMeta.xbx), else by ID. */
  fun titleNames(ids: Collection<String>): String {
    val names = _contents.value.saves.associate { it.id to it.name }
    return ids.joinToString(", ") { names[it.uppercase()] ?: it.uppercase() }
  }

  /** E:, where the saves are (UDATA) and the titles' data (TDATA). */
  private fun partition1() = XboxHdd.folder(store.root, 1)

  fun noticeShown() {
    _notice.value = null
  }

  fun dismissJob() {
    if (_job.value?.finished != false) _job.value = null
  }

  private fun message(t: Throwable) = t.message ?: t.javaClass.simpleName
}
