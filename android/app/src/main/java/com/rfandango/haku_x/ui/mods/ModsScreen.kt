package com.rfandango.haku_x.ui.mods

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.rfandango.haku_x.R
import com.rfandango.haku_x.data.ModCatalog
import com.rfandango.haku_x.data.ModChoices
import com.rfandango.haku_x.ui.common.ActionItem
import com.rfandango.haku_x.ui.common.EmptyText
import com.rfandango.haku_x.ui.common.Hint
import com.rfandango.haku_x.ui.common.Section
import com.rfandango.haku_x.ui.common.SubScreen
import com.rfandango.haku_x.ui.common.SwitchItem

/**
 * One disc's mods and cheats (the repository's mods/ folder, and the ones the
 * user pasted in): each on or off. Mods take the next start; cheats also turn
 * on and off in the game's menu while it runs.
 */
@Composable
fun ModsScreen(gameKey: String, title: String, regions: Int, onBack: () -> Unit) {
  val context = LocalContext.current
  // bumped when a mod is added or removed: the lists are read again
  var version by remember { mutableIntStateOf(0) }
  var adding by remember { mutableStateOf(false) }
  val game = remember(gameKey, version) { ModCatalog.load(context, gameKey, regions) }
  val choices = remember(gameKey) { ModChoices(context, gameKey) }
  val mods = remember(gameKey, version) {
    mutableStateMapOf<String, Boolean>().apply { game.mods.forEach { put(it.id, choices.mod(it.id)) } }
  }
  val cheats = remember(gameKey, version) {
    mutableStateMapOf<String, Boolean>().apply { game.cheats.forEach { put(it.name, choices.cheat(it.name)) } }
  }

  SubScreen(title = stringResource(R.string.mods_title), subtitle = title, onBack = onBack) { padding ->
    Column(Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState())) {
      if (game.isEmpty) {
        EmptyText(stringResource(R.string.mods_none))
      }
      if (game.mods.isNotEmpty()) {
        Section(stringResource(R.string.mods_section_mods))
        Hint(stringResource(R.string.mods_mods_desc))
        for (mod in game.mods) {
          val by = if (mod.author.isNotEmpty()) stringResource(R.string.mods_by, mod.author) else ""
          SwitchItem(
            title = mod.name,
            summary = listOf(mod.description, by).filter { it.isNotEmpty() }.joinToString("\n").ifEmpty { null },
            checked = mods[mod.id] == true,
            onChange = { v ->
              choices.setMod(mod.id, v)
              mods[mod.id] = v
            },
          )
          if (mod.user) {
            TextButton(
              onClick = {
                choices.setMod(mod.id, false)
                ModCatalog.deleteUserMod(context, gameKey, mod.id)
                version++
              },
              modifier = Modifier.padding(start = 8.dp),
            ) { Text(stringResource(R.string.mods_remove)) }
          }
        }
      }
      if (game.cheats.isNotEmpty()) {
        Section(stringResource(R.string.mods_section_cheats))
        Hint(stringResource(R.string.mods_cheats_desc))
        for (cheat in game.cheats) {
          SwitchItem(
            title = cheat.name,
            summary = cheat.note.ifEmpty { null },
            checked = cheats[cheat.name] == true,
            onChange = { v ->
              choices.setCheat(cheat.name, v)
              cheats[cheat.name] = v
            },
          )
        }
      }
      Section(stringResource(R.string.mods_section_yours))
      ActionItem(
        title = stringResource(R.string.mods_add),
        summary = stringResource(R.string.mods_add_summary),
        icon = Icons.Filled.Add,
        onClick = { adding = true },
      )
    }
  }

  if (adding) {
    AddModDialog(
      onSave = { name, text ->
        val id = ModCatalog.saveUserMod(context, gameKey, name, text)
        choices.setMod(id, true)
        adding = false
        version++
      },
      onDismiss = { adding = false },
    )
  }
}

/** A mod pasted in: its name and its patch lines (or a whole mod.txt). */
@Composable
private fun AddModDialog(onSave: (String, String) -> Unit, onDismiss: () -> Unit) {
  var name by remember { mutableStateOf("") }
  var text by remember { mutableStateOf("") }
  AlertDialog(
    onDismissRequest = onDismiss,
    title = { Text(stringResource(R.string.mods_add)) },
    text = {
      Column(Modifier.verticalScroll(rememberScrollState())) {
        OutlinedTextField(
          value = name,
          onValueChange = { name = it },
          label = { Text(stringResource(R.string.mods_add_name)) },
          singleLine = true,
          modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
          value = text,
          onValueChange = { text = it },
          label = { Text(stringResource(R.string.mods_add_patches)) },
          minLines = 6,
          maxLines = 12,
          modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
        )
        Hint(stringResource(R.string.mods_add_help))
      }
    },
    confirmButton = {
      TextButton(enabled = text.isNotBlank(), onClick = { onSave(name, text) }) {
        Text(stringResource(R.string.mods_add_save))
      }
    },
    dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(android.R.string.cancel)) } },
  )
}
