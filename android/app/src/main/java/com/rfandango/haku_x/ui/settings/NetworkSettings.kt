package com.rfandango.haku_x.ui.settings

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ListItem
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.rfandango.haku_x.R
import com.rfandango.haku_x.data.AppPrefs
import com.rfandango.haku_x.ui.common.ChoiceDialog
import com.rfandango.haku_x.ui.common.Hint
import com.rfandango.haku_x.ui.common.Section
import com.rfandango.haku_x.ui.common.SwitchItem

/**
 * The console's network (native/net): off; Xbox Live through Insignia (the Internet through NAT, its DNS
 * server or an insigniaDNS one); system link with the consoles on this network (LAN), others reached
 * directly too (another subnet, a VPN); or system link over the Internet through an XLink Kai engine on
 * this network. Read at the next game start.
 */
@Composable
fun NetworkSettings(prefs: AppPrefs, onInsignia: () -> Unit) {
  var mode by remember { mutableStateOf(prefs.networkMode) }
  var peers by remember { mutableStateOf(prefs.lanPeers) }
  var kai by remember { mutableStateOf(prefs.kaiEngine) }
  var dns by remember { mutableStateOf(prefs.insigniaDns) }
  var modeDialog by remember { mutableStateOf(false) }
  var peersDialog by remember { mutableStateOf(false) }
  var kaiDialog by remember { mutableStateOf(false) }
  var dnsDialog by remember { mutableStateOf(false) }
  val context = LocalContext.current
  fun micAllowed() =
    ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
  // on only with the microphone allowed: asked for as it is turned on
  var voice by remember { mutableStateOf(prefs.voiceChat && micAllowed()) }
  val askMic = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
    voice = granted
    prefs.voiceChat = granted
  }
  // back from Insignia's setup, which may have chosen the connection
  LifecycleResumeEffect(Unit) {
    mode = prefs.networkMode
    onPauseOrDispose {}
  }

  Section(stringResource(R.string.settings_section_network))
  Hint(stringResource(R.string.settings_network_desc))
  // Insignia's setup wizard: everything online play needs, step by step
  ListItem(
    headlineContent = { Text(stringResource(R.string.insignia_title)) },
    supportingContent = { Text(stringResource(R.string.insignia_settings_desc)) },
    modifier = Modifier.clickable(onClick = onInsignia),
  )
  ListItem(
    headlineContent = { Text(stringResource(R.string.settings_network_mode)) },
    supportingContent = {
      Text(stringResource(modeLabel(mode)) + ". " + stringResource(R.string.settings_applies_next_start))
    },
    modifier = Modifier.clickable { modeDialog = true },
  )
  if (mode == AppPrefs.NET_INSIGNIA) {
    ListItem(
      headlineContent = { Text(stringResource(R.string.settings_network_insignia_dns)) },
      supportingContent = {
        Text(dns.ifBlank { stringResource(R.string.settings_network_insignia_dns_default, AppPrefs.INSIGNIA_DNS) } +
          ". " + stringResource(R.string.settings_network_insignia_dns_desc))
      },
      modifier = Modifier.clickable { dnsDialog = true },
    )
  }
  SwitchItem(
    title = stringResource(R.string.settings_network_voice),
    summary = stringResource(R.string.settings_network_voice_desc) + " " + stringResource(R.string.settings_applies_next_start),
    checked = voice,
    onChange = { on ->
      if (on && !micAllowed()) {
        askMic.launch(Manifest.permission.RECORD_AUDIO)
      } else {
        voice = on
        prefs.voiceChat = on
      }
    },
  )
  if (mode == AppPrefs.NET_LAN) {
    ListItem(
      headlineContent = { Text(stringResource(R.string.settings_network_peers)) },
      supportingContent = {
        Text(peers.ifBlank { stringResource(R.string.settings_network_peers_none) } + ". " +
          stringResource(R.string.settings_network_peers_desc))
      },
      modifier = Modifier.clickable { peersDialog = true },
    )
  }
  if (mode == AppPrefs.NET_KAI) {
    ListItem(
      headlineContent = { Text(stringResource(R.string.settings_network_kai)) },
      supportingContent = {
        Text(kai.ifBlank { stringResource(R.string.settings_network_kai_none) } + ". " +
          stringResource(R.string.settings_network_kai_desc))
      },
      modifier = Modifier.clickable { kaiDialog = true },
    )
  }

  if (modeDialog) {
    ChoiceDialog(
      title = stringResource(R.string.settings_network_mode),
      options = MODES.map { it to stringResource(modeLabel(it)) },
      current = mode,
      onPick = { mode = it; prefs.networkMode = it; modeDialog = false },
      onDismiss = { modeDialog = false },
    )
  }
  if (peersDialog) {
    TextDialog(
      title = stringResource(R.string.settings_network_peers),
      hint = stringResource(R.string.settings_network_peers_hint),
      value = peers,
      onDone = { peers = it.trim(); prefs.lanPeers = peers; peersDialog = false },
      onDismiss = { peersDialog = false },
    )
  }
  if (kaiDialog) {
    TextDialog(
      title = stringResource(R.string.settings_network_kai),
      hint = stringResource(R.string.settings_network_kai_hint),
      value = kai,
      onDone = { kai = it.trim(); prefs.kaiEngine = kai; kaiDialog = false },
      onDismiss = { kaiDialog = false },
    )
  }
  if (dnsDialog) {
    TextDialog(
      title = stringResource(R.string.settings_network_insignia_dns),
      hint = AppPrefs.INSIGNIA_DNS,
      value = dns,
      onDone = { dns = it.trim(); prefs.insigniaDns = dns; dnsDialog = false },
      onDismiss = { dnsDialog = false },
    )
  }
}

private val MODES = listOf(AppPrefs.NET_OFF, AppPrefs.NET_INSIGNIA, AppPrefs.NET_LAN, AppPrefs.NET_KAI)

private fun modeLabel(value: String) = when (value) {
  AppPrefs.NET_INSIGNIA -> R.string.settings_network_insignia
  AppPrefs.NET_LAN -> R.string.settings_network_lan
  AppPrefs.NET_KAI -> R.string.settings_network_kai_mode
  else -> R.string.settings_network_off
}

/** One line of text to edit (addresses). */
@Composable
private fun TextDialog(title: String, hint: String, value: String, onDone: (String) -> Unit, onDismiss: () -> Unit) {
  var text by remember { mutableStateOf(value) }
  AlertDialog(
    onDismissRequest = onDismiss,
    title = { Text(title) },
    text = {
      OutlinedTextField(
        value = text,
        onValueChange = { text = it },
        singleLine = true,
        placeholder = { Text(hint) },
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
      )
    },
    confirmButton = { TextButton(onClick = { onDone(text) }) { Text(stringResource(android.R.string.ok)) } },
    dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(android.R.string.cancel)) } },
  )
}
