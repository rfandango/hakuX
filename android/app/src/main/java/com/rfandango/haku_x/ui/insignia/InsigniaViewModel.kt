package com.rfandango.haku_x.ui.insignia

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.rfandango.haku_x.data.AppPrefs
import com.rfandango.haku_x.data.Insignia
import com.rfandango.haku_x.data.Storage
import com.rfandango.haku_x.data.hdd.Dashboard
import com.rfandango.haku_x.data.hdd.HddStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** What Insignia's setup has done: each step's state, read from the settings and the hard disk. */
data class InsigniaStatus(
  /** The console's network is Insignia's (AppPrefs.NET_INSIGNIA). */
  val network: Boolean = false,
  /** The dashboard is on C:. */
  val dashboard: Boolean = false,
  /** The Setup Assistant is at hand (downloaded or chosen). */
  val setup: Boolean = false,
  /** The user said it registered the console. */
  val registered: Boolean = false,
  /** The Insignia accounts on the console (gamertags). */
  val accounts: List<String> = emptyList(),
)

/** A long job of the wizard (the dashboard's install, the Setup Assistant's download). */
data class WizardJob(
  val done: Long = 0,
  val total: Long = 0,
  val what: String = "",
  val finished: Boolean = false,
  val error: String? = null,
)

/** Insignia's setup wizard: its steps' state and its jobs. */
class InsigniaViewModel(app: Application) : AndroidViewModel(app) {
  private val prefs = AppPrefs(app)
  private val hddRoot = HddStore(app).root

  private val _status = MutableStateFlow(read())
  val status: StateFlow<InsigniaStatus> = _status
  private val _dashboardJob = MutableStateFlow<WizardJob?>(null)
  val dashboardJob: StateFlow<WizardJob?> = _dashboardJob
  private val _downloadJob = MutableStateFlow<WizardJob?>(null)
  val downloadJob: StateFlow<WizardJob?> = _downloadJob

  private fun read() = InsigniaStatus(
    network = prefs.networkMode == AppPrefs.NET_INSIGNIA,
    dashboard = Dashboard.installed(hddRoot),
    setup = Insignia.setupUri(getApplication()) != null,
    registered = prefs.insigniaRegistered,
    accounts = Insignia.accounts(hddRoot),
  )

  /** Back on screen (from the Setup Assistant, the dashboard, the settings): the state read again. */
  fun refresh() {
    _status.value = read()
  }

  fun useInsignia() {
    prefs.networkMode = AppPrefs.NET_INSIGNIA
    refresh()
  }

  fun setRegistered(on: Boolean) {
    prefs.insigniaRegistered = on
    refresh()
  }

  fun installDashboard(uri: Uri) {
    if (_dashboardJob.value?.finished == false) return
    _dashboardJob.value = WizardJob()
    val app = getApplication<Application>()
    viewModelScope.launch {
      val r = withContext(Dispatchers.IO) {
        runCatching {
          Dashboard.install({ app.contentResolver.openInputStream(uri)!! }, hddRoot) { d, t, w ->
            _dashboardJob.update { it?.copy(done = d, total = t, what = w) }
          }
        }
      }
      _dashboardJob.update { it?.copy(finished = true, error = r.exceptionOrNull()?.let(::message)) }
      refresh()
    }
  }

  fun downloadSetup() {
    if (_downloadJob.value?.finished == false) return
    _downloadJob.value = WizardJob()
    val app = getApplication<Application>()
    viewModelScope.launch {
      val r = withContext(Dispatchers.IO) {
        runCatching { Insignia.download(app) { d, t -> _downloadJob.update { it?.copy(done = d, total = t) } } }
      }
      _downloadJob.update { it?.copy(finished = true, error = r.exceptionOrNull()?.let(::message)) }
      refresh()
    }
  }

  /** The Setup Assistant ISO the user chose instead of the download. */
  fun chooseSetup(uri: Uri) {
    Storage.persistPermission(getApplication(), uri, write = false)
    prefs.insigniaSetupUri = uri.toString()
    refresh()
  }

  private fun message(t: Throwable) = t.message ?: t.javaClass.simpleName
}
