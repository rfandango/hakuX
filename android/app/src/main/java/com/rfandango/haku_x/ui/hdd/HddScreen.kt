package com.rfandango.haku_x.ui.hdd

import android.net.Uri
import android.text.format.DateFormat
import android.text.format.DateUtils
import android.text.format.Formatter
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.rfandango.haku_x.R
import com.rfandango.haku_x.data.hdd.ConsoleFiles
import com.rfandango.haku_x.data.hdd.HddStore
import com.rfandango.haku_x.ui.common.ActionItem
import com.rfandango.haku_x.ui.common.CheckItem
import com.rfandango.haku_x.ui.common.ConfirmDialog
import com.rfandango.haku_x.ui.common.DeleteButton
import com.rfandango.haku_x.ui.common.EmptyText
import com.rfandango.haku_x.ui.common.Hint
import com.rfandango.haku_x.ui.common.NoticeEffect
import com.rfandango.haku_x.ui.common.ProgressBlock
import com.rfandango.haku_x.ui.common.Section
import com.rfandango.haku_x.ui.common.SubScreen
import com.rfandango.haku_x.ui.common.ThumbnailShape
import com.rfandango.haku_x.ui.common.fractionOf
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The Xbox's hard disk as the native core keeps it: the space each kind of
 * data takes, the game saves (each title's, each save exportable as a zip
 * and deletable; zips of saves imported), the
 * titles' data, the cache drives (cleared: the game fills them again), the
 * savestates, old copies of the disk, a whole backup (made and restored, for
 * another hakuX or to keep), and a console's or xemu's data brought in: its
 * disk image (qcow2 or raw) or its saves, re-signed for hakuX with keys read
 * from the user's dumps (the console files).
 */
@Composable
fun HddScreen(onBack: () -> Unit, vm: HddViewModel = viewModel()) {
  val context = LocalContext.current
  val c by vm.contents.collectAsStateWithLifecycle()
  val job by vm.job.collectAsStateWithLifecycle()
  var confirm by remember { mutableStateOf<Confirm?>(null) }
  var expanded by remember { mutableStateOf<String?>(null) }
  var backupOptions by remember { mutableStateOf(false) }
  var backupStates by rememberSaveable { mutableStateOf(true) }
  var backupCaches by rememberSaveable { mutableStateOf(false) }
  var consoleKind by rememberSaveable { mutableStateOf<String?>(null) }   // ConsoleFiles.Kind being picked
  var exportSaves by rememberSaveable { mutableStateOf<String?>(null) }   // HddViewModel.exportSaves' which
  val size = { b: Long -> Formatter.formatShortFileSize(context, b) }
  val snackbar = remember { SnackbarHostState() }
  val notice by vm.notice.collectAsStateWithLifecycle()
  val saveImport by vm.saveImport.collectAsStateWithLifecycle()
  val restore by vm.restore.collectAsStateWithLifecycle()
  NoticeEffect(snackbar, notice, vm::noticeShown)

  val pickImage = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
    if (uri != null) {
      confirm = Confirm(R.string.hdd_import_confirm_title, context.getString(R.string.hdd_import_confirm),
                        R.string.hdd_import_replace) { vm.import(uri) }
    }
  }
  val createBackup = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri: Uri? ->
    if (uri != null) vm.backup(uri, backupStates, backupCaches)
  }
  val pickBackup = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
    if (uri != null) vm.planRestore(uri)
  }
  val pickConsole = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
    val k = consoleKind?.let(ConsoleFiles.Kind::valueOf)
    consoleKind = null
    if (uri != null && k != null) vm.setConsoleFile(k, uri)
  }
  val createZip = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri: Uri? ->
    val which = exportSaves
    exportSaves = null
    if (uri != null && which != null) vm.exportSaves(uri, which)
  }
  val pickSaves = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
    if (uri != null) vm.planSaveImport(uri)
  }
  val pickDashboard = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
    if (uri != null) vm.installDashboard(uri)
  }
  val saveZip = { which: String, name: String ->
    exportSaves = which
    createZip.launch(name.replace(Regex("[\\\\/:*?\"<>|]"), "_") + ".zip")
  }

  SubScreen(title = stringResource(R.string.hdd_title), onBack = onBack, snackbar = snackbar) { padding ->
    LazyColumn(Modifier.padding(padding).fillMaxSize()) {
      item { Hint(stringResource(R.string.hdd_desc)) }
      c.usage?.let { u ->
        item { Section(stringResource(R.string.hdd_space)) }
        item { UsageRow(stringResource(R.string.hdd_space_saves), size(u.saves)) }
        item { UsageRow(stringResource(R.string.hdd_space_title_data), size(u.titleData)) }
        item { UsageRow(stringResource(R.string.hdd_space_caches), size(u.caches)) }
        item { UsageRow(stringResource(R.string.hdd_space_system), size(u.system)) }
        item { UsageRow(stringResource(R.string.hdd_space_states), size(u.states)) }
        c.previousImage?.let { img ->
          item {
            ListItem(
              headlineContent = { Text(stringResource(R.string.hdd_previous)) },
              supportingContent = { Text(stringResource(R.string.hdd_previous_desc, size(img.length()))) },
              trailingContent = {
                DeleteButton(onClick = {
                  confirm = Confirm(R.string.hdd_previous_delete, context.getString(R.string.hdd_previous_delete_desc)) {
                    vm.deletePreviousImage()
                  }
                })
              },
            )
          }
        }
      }

      item { Section(stringResource(R.string.hdd_backup)) }
      item {
        ListItem(
          headlineContent = { Text(stringResource(R.string.hdd_backup_make)) },
          supportingContent = { Text(stringResource(R.string.hdd_backup_make_desc)) },
          modifier = Modifier.clickable { backupOptions = true },
        )
      }
      item {
        ListItem(
          headlineContent = { Text(stringResource(R.string.hdd_restore)) },
          supportingContent = { Text(stringResource(R.string.hdd_restore_desc)) },
          modifier = Modifier.clickable {
            pickBackup.launch(arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
          },
        )
      }

      item { Section(stringResource(R.string.hdd_saves)) }
      if (c.saves.isEmpty() && !c.loading) item { EmptyText(stringResource(R.string.hdd_saves_none)) }
      item {
        ActionItem(
          title = stringResource(R.string.hdd_saves_import),
          summary = stringResource(R.string.hdd_saves_import_desc),
          icon = Icons.Filled.Add,
        ) {
          pickSaves.launch(arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
        }
      }
      if (c.saves.isNotEmpty()) {
        item {
          ActionItem(
            title = stringResource(R.string.hdd_saves_export_all),
            summary = stringResource(R.string.hdd_saves_export_all_desc),
            icon = Icons.Filled.Share,
          ) { saveZip("", context.getString(R.string.hdd_saves_export_all_file)) }
        }
      }
      items(c.saves, key = { "s" + it.id }) { t ->
        val open = expanded == t.id
        ListItem(
          leadingContent = { XbxIcon(t.icon) },
          headlineContent = { Text(t.name ?: stringResource(R.string.hdd_title_id, t.id), maxLines = 1, overflow = TextOverflow.Ellipsis) },
          supportingContent = { Text(pluralStringResource(R.plurals.hdd_saves_count, t.saves.size, t.saves.size, size(t.size))) },
          trailingContent = {
            Icon(if (open) Icons.Filled.KeyboardArrowUp else Icons.Filled.KeyboardArrowDown, contentDescription = null)
          },
          modifier = Modifier.clickable { expanded = if (open) null else t.id },
        )
        if (open) {
          Column(Modifier.padding(start = 56.dp)) {
            for (s in t.saves) {
              ListItem(
                leadingContent = { XbxIcon(s.icon, small = true) },
                headlineContent = { Text(s.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                supportingContent = {
                  Text(DateFormat.getMediumDateFormat(context).format(Date(s.modified)) + " · " + size(s.size))
                },
                trailingContent = {
                  Row {
                    IconButton(onClick = { saveZip("${t.id}/${s.dir.name}", "${t.name ?: t.id} - ${s.name}") }) {
                      Icon(Icons.Filled.Share, contentDescription = stringResource(R.string.hdd_save_export))
                    }
                    DeleteButton(onClick = {
                      confirm = Confirm(R.string.hdd_save_delete, context.getString(R.string.hdd_save_delete_desc, s.name)) {
                        vm.delete(s.dir)
                      }
                    })
                  }
                },
              )
            }
            Row(Modifier.padding(start = 8.dp)) {
              if (t.saves.isNotEmpty()) {
                TextButton(onClick = { saveZip(t.id, t.name ?: t.id) }) { Text(stringResource(R.string.hdd_saves_export)) }
              }
              TextButton(
                onClick = {
                  confirm = Confirm(R.string.hdd_saves_delete_all,
                                    context.getString(R.string.hdd_saves_delete_all_desc, t.name ?: t.id)) {
                    vm.delete(t.dir)
                  }
                },
              ) { Text(stringResource(R.string.hdd_saves_delete_all)) }
            }
          }
        }
      }

      if (c.titleData.isNotEmpty()) {
        item { Section(stringResource(R.string.hdd_title_data)) }
        items(c.titleData, key = { "t" + it.id }) { t ->
          ListItem(
            headlineContent = { Text(t.name ?: stringResource(R.string.hdd_title_id, t.id), maxLines = 1, overflow = TextOverflow.Ellipsis) },
            supportingContent = { Text(size(t.size)) },
            trailingContent = {
              DeleteButton(onClick = {
                confirm = Confirm(R.string.hdd_title_data_delete,
                                  context.getString(R.string.hdd_title_data_delete_desc, t.name ?: t.id)) { vm.delete(t.dir) }
              })
            },
          )
        }
      }

      item { Section(stringResource(R.string.hdd_caches)) }
      item { Hint(stringResource(R.string.hdd_caches_desc)) }
      items(c.caches, key = { "c" + it.part }) { d ->
        ListItem(
          headlineContent = { Text(stringResource(R.string.hdd_cache_drive, d.drive)) },
          supportingContent = {
            Text(if (d.formatted) pluralStringResource(R.plurals.hdd_cache_used, d.files, size(d.size), d.files)
                 else stringResource(R.string.hdd_cache_empty))
          },
          trailingContent = {
            if (d.formatted) {
              TextButton(onClick = {
                confirm = Confirm(R.string.hdd_cache_clear_title, context.getString(R.string.hdd_cache_clear_desc, d.drive),
                                  R.string.hdd_cache_clear) { vm.clearCache(d.part) }
              }) { Text(stringResource(R.string.hdd_cache_clear)) }
            }
          },
        )
      }

      if (c.states.isNotEmpty()) {
        item { Section(stringResource(R.string.hdd_states)) }
        val names = c.saves.associate { it.id to it.name }
        items(c.states, key = { "x" + it.key }) { s ->
          val id = s.key.substringBefore('.').uppercase()
          ListItem(
            headlineContent = { Text(names[id] ?: s.key, maxLines = 1, overflow = TextOverflow.Ellipsis) },
            supportingContent = { Text(pluralStringResource(R.plurals.hdd_states_count, s.slots, s.slots, size(s.size))) },
            trailingContent = {
              DeleteButton(onClick = {
                confirm = Confirm(R.string.hdd_states_delete,
                                  context.getString(R.string.hdd_states_delete_desc, names[id] ?: s.key)) { vm.delete(s.dir) }
              })
            },
          )
        }
      }

      item { Section(stringResource(R.string.hdd_xbox)) }
      item { Hint(stringResource(R.string.hdd_xbox_desc)) }
      val cs = c.console
      items(ConsoleFiles.Kind.entries, key = { "k" + it.name }) { k ->
        val set = cs != null && k in cs.set
        ListItem(
          headlineContent = {
            Text(stringResource(when (k) {
              ConsoleFiles.Kind.MCPX -> R.string.hdd_console_mcpx
              ConsoleFiles.Kind.BIOS -> R.string.hdd_console_bios
              ConsoleFiles.Kind.EEPROM -> R.string.hdd_console_eeprom
            }))
          },
          supportingContent = {
            Text(stringResource(when {
              !set -> R.string.hdd_console_unset
              k == ConsoleFiles.Kind.BIOS && cs!!.certKey -> R.string.hdd_console_read
              k == ConsoleFiles.Kind.BIOS && ConsoleFiles.Kind.MCPX in cs!!.set -> R.string.hdd_console_bios_bad
              k == ConsoleFiles.Kind.EEPROM && cs!!.hdKey -> R.string.hdd_console_read
              k == ConsoleFiles.Kind.EEPROM -> R.string.hdd_console_eeprom_bad
              else -> R.string.hdd_console_set
            }))
          },
          modifier = Modifier.clickable {
            consoleKind = k.name
            pickConsole.launch(arrayOf("*/*"))
          },
        )
      }
      item {
        val ready = cs?.ready == true
        ListItem(
          headlineContent = { Text(stringResource(R.string.hdd_import)) },
          supportingContent = { Text(stringResource(if (ready) R.string.hdd_import_desc else R.string.hdd_import_needs)) },
          modifier = Modifier.clickable(enabled = ready) { pickImage.launch(arrayOf("*/*")) },
        )
      }

      item { Section(stringResource(R.string.hdd_dashboard)) }
      item {
        ListItem(
          headlineContent = { Text(stringResource(R.string.hdd_dashboard_install)) },
          supportingContent = {
            Text(stringResource(if (c.dashboard) R.string.hdd_dashboard_installed else R.string.hdd_dashboard_desc))
          },
          modifier = Modifier.clickable { pickDashboard.launch(arrayOf("application/zip", "application/x-zip-compressed")) },
        )
      }
      item { Spacer(Modifier.height(24.dp)) }
    }
  }

  confirm?.let { cf ->
    ConfirmDialog(
      title = stringResource(cf.title),
      text = cf.text,
      confirm = stringResource(cf.confirm),
      onConfirm = {
        confirm = null
        cf.action()
      },
      onDismiss = { confirm = null },
    )
  }

  job?.let { j -> JobDialog(j, size, vm::titleNames, vm::dismissJob) }

  if (backupOptions) {
    val u = c.usage
    AlertDialog(
      onDismissRequest = { backupOptions = false },
      title = { Text(stringResource(R.string.hdd_backup_make)) },
      text = {
        Column {
          CheckItem(stringResource(R.string.hdd_backup_states, size(u?.states ?: 0)), null, backupStates) { backupStates = it }
          CheckItem(stringResource(R.string.hdd_backup_caches, size(u?.caches ?: 0)),
                    stringResource(R.string.hdd_backup_caches_desc), backupCaches) { backupCaches = it }
        }
      },
      confirmButton = {
        TextButton(onClick = {
          backupOptions = false
          val day = SimpleDateFormat("yyyy-MM-dd", Locale.US).format(Date())
          createBackup.launch(context.getString(R.string.hdd_backup_file, day) + ".zip")
        }) { Text(stringResource(R.string.hdd_backup_go)) }
      },
      dismissButton = { TextButton(onClick = { backupOptions = false }) { Text(stringResource(android.R.string.cancel)) } },
    )
  }

  restore?.let { r ->
    val made = DateUtils.formatDateTime(context, r.info.created, DateUtils.FORMAT_SHOW_DATE or DateUtils.FORMAT_SHOW_TIME)
    AlertDialog(
      onDismissRequest = vm::cancelRestore,
      title = { Text(stringResource(R.string.hdd_restore_title)) },
      text = {
        Column {
          Text(stringResource(R.string.hdd_restore_confirm, made))
          Spacer(Modifier.height(8.dp))
          Text(stringResource(if (r.info.states) R.string.hdd_restore_states else R.string.hdd_restore_no_states))
          if (!r.info.caches) {
            Spacer(Modifier.height(8.dp))
            Text(stringResource(R.string.hdd_restore_caches))
          }
        }
      },
      confirmButton = { TextButton(onClick = vm::restore) { Text(stringResource(R.string.hdd_restore_go)) } },
      dismissButton = { TextButton(onClick = vm::cancelRestore) { Text(stringResource(android.R.string.cancel)) } },
    )
  }

  saveImport?.let { s ->
    val p = s.plan
    val known = c.saves.associate { it.id to it.name }
    val names = p.titles.joinToString(", ") { p.names[it] ?: known[it.uppercase()] ?: it.uppercase() }
    AlertDialog(
      onDismissRequest = vm::cancelSaveImport,
      title = { Text(stringResource(R.string.hdd_saves_import_title)) },
      text = {
        Column {
          Text(
            if (p.saveCount > 0) pluralStringResource(R.plurals.hdd_saves_import_confirm, p.saveCount, p.saveCount, names)
            else pluralStringResource(R.plurals.hdd_saves_import_confirm_files, p.files, p.files, names)
          )
          if (p.replaced > 0) {
            Spacer(Modifier.height(8.dp))
            Text(pluralStringResource(R.plurals.hdd_saves_import_replace, p.replaced, p.replaced))
          }
          if (p.skipped.isNotEmpty()) {
            Spacer(Modifier.height(8.dp))
            Text(pluralStringResource(R.plurals.hdd_saves_import_skipped, p.skipped.size, p.skipped.size))
          }
          if (s.foreign) {
            Spacer(Modifier.height(8.dp))
            Text(
              stringResource(if (s.canResign) R.string.hdd_saves_import_foreign else R.string.hdd_saves_import_foreign_nokeys),
              style = MaterialTheme.typography.bodySmall,
            )
          }
        }
      },
      confirmButton = { TextButton(onClick = vm::importSaves) { Text(stringResource(R.string.hdd_saves_import_go)) } },
      dismissButton = { TextButton(onClick = vm::cancelSaveImport) { Text(stringResource(android.R.string.cancel)) } },
    )
  }
}

/** A question before action: its title, its text, its button (what it does: Delete, by default). */
private class Confirm(val title: Int, val text: String, val confirm: Int = R.string.common_delete, val action: () -> Unit)

@Composable
private fun UsageRow(label: String, value: String) {
  Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp)) {
    Text(label, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
    Text(value, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
  }
}

/** A title's or a save's .xbx image, or a blank square. */
@Composable
private fun XbxIcon(f: File?, small: Boolean = false) {
  val px = if (small) 32.dp else 40.dp
  val bmp = remember(f?.path) { HddStore.icon(f)?.asImageBitmap() }
  if (bmp != null) {
    Image(bmp, contentDescription = null, modifier = Modifier.size(px).clip(ThumbnailShape))
  } else {
    Box(Modifier.size(px).clip(ThumbnailShape).background(MaterialTheme.colorScheme.surfaceVariant))
  }
}

@Composable
private fun JobDialog(j: HddViewModel.Job, size: (Long) -> String, names: (Collection<String>) -> String, onDismiss: () -> Unit) {
  AlertDialog(
    onDismissRequest = { if (j.finished) onDismiss() },
    properties = DialogProperties(dismissOnBackPress = j.finished, dismissOnClickOutside = j.finished),
    title = {
      Text(stringResource(
        when (j.kind) {
          HddViewModel.Kind.IMAGE_IMPORT ->
            if (j.error != null) R.string.hdd_import_failed else if (j.finished) R.string.hdd_import_done else R.string.hdd_importing
          HddViewModel.Kind.BACKUP ->
            if (j.error != null) R.string.hdd_backup_failed else if (j.finished) R.string.hdd_backup_done else R.string.hdd_backing_up
          HddViewModel.Kind.RESTORE ->
            if (j.error != null) R.string.hdd_restore_failed else if (j.finished) R.string.hdd_restore_done else R.string.hdd_restoring
          HddViewModel.Kind.DASHBOARD ->
            if (j.error != null) R.string.hdd_dashboard_failed else if (j.finished) R.string.hdd_dashboard_done
            else R.string.hdd_dashboard_installing
        }
      ))
    },
    text = {
      Column {
        when {
          j.error != null -> Text(j.error)
          j.result != null -> {
            val r = j.result
            Text(pluralStringResource(R.plurals.hdd_job_summary, r.files, r.files, size(r.bytes)))
            if (r.skipped.isNotEmpty()) {
              Spacer(Modifier.height(8.dp))
              Text(pluralStringResource(R.plurals.hdd_job_skipped, r.skipped.size, r.skipped.size,
                                   r.skipped.take(5).joinToString("\n")))
            }
            j.resign?.let { rep ->
              Spacer(Modifier.height(8.dp))
              Text(pluralStringResource(R.plurals.hdd_resigned, rep.resigned, rep.resigned))
              if (rep.unsigned.isNotEmpty()) {
                Text(pluralStringResource(R.plurals.hdd_resign_unsigned, rep.unsigned.size, rep.unsigned.size),
                     style = MaterialTheme.typography.bodySmall)
              }
              if (rep.noKey.isNotEmpty()) {
                Text(stringResource(R.string.hdd_resign_nokey, names(rep.noKey)), style = MaterialTheme.typography.bodySmall)
              }
            }
          }
          else -> ProgressBlock(
            fractionOf(j.done, j.total),
            if (j.what.isNotEmpty()) j.what else if (j.total > 0) "${size(j.done)} / ${size(j.total)}" else null,
          )
        }
      }
    },
    confirmButton = {
      if (j.finished) TextButton(onClick = onDismiss) { Text(stringResource(android.R.string.ok)) }
    },
  )
}
