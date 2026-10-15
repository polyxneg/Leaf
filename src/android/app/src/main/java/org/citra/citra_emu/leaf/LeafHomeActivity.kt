// Copyright 2026 Poly
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

package org.citra.citra_emu.leaf

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.drawable.BitmapDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.view.KeyEvent
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import androidx.documentfile.provider.DocumentFile
import androidx.preference.PreferenceManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.R
import org.citra.citra_emu.activities.EmulationActivity
import org.citra.citra_emu.databinding.ActivityLeafHomeBinding
import org.citra.citra_emu.features.settings.model.BooleanSetting
import org.citra.citra_emu.features.settings.model.IntSetting
import org.citra.citra_emu.features.settings.model.Settings
import org.citra.citra_emu.features.settings.ui.SettingsActivity
import org.citra.citra_emu.features.settings.utils.SettingsFile
import org.citra.citra_emu.model.GameInfo
import org.citra.citra_emu.utils.BuildUtil
import org.citra.citra_emu.utils.CitraDirectoryHelper
import org.citra.citra_emu.utils.DirectoryInitialization
import org.citra.citra_emu.utils.FileUtil
import org.citra.citra_emu.utils.PermissionsHandler
import org.citra.citra_emu.utils.ThemeUtil
import java.io.File
import java.nio.IntBuffer

/** Launcher: pick the save folder and the user's own game dump, adjust options, play. */
class LeafHomeActivity : AppCompatActivity() {
    private lateinit var binding: ActivityLeafHomeBinding

    private val chooseFolder =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
            if (uri == null) return@registerForActivityResult
            contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            )
            CitraDirectoryHelper.initializeCitraDirectory(uri)
            PreferenceManager.getDefaultSharedPreferences(this)
                .edit { putBoolean(Settings.PREF_FIRST_APP_LAUNCH, false) }
            refresh()
        }

    private val allFilesAccess =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { refresh() }

    private val legacyStoragePermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { refresh() }

    private val chooseRom =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri != null) onRomChosen(uri)
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        installSplashScreen()
        ThemeUtil.setTheme(this)
        super.onCreate(savedInstanceState)
        binding = ActivityLeafHomeBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.leafChooseFolder.setOnClickListener {
            PermissionsHandler.compatibleSelectDirectory(chooseFolder)
        }
        binding.leafChooseRom.setOnClickListener {
            chooseRom.launch(arrayOf("*/*"))
        }
        binding.leafPlay.setOnClickListener { play() }
        binding.leafAdvanced.setOnClickListener {
            SettingsActivity.launch(this, SettingsFile.FILE_NAME_CONFIG, "")
        }

        binding.leafDisplayGroup.check(
            when (LeafSettings.displayMode) {
                LeafSettings.DISPLAY_16_9 -> R.id.leaf_display_16_9
                LeafSettings.DISPLAY_4_3 -> R.id.leaf_display_4_3
                LeafSettings.DISPLAY_ORIGINAL -> R.id.leaf_display_original
                else -> R.id.leaf_display_auto
            }
        )
        binding.leafDisplayGroup.addOnButtonCheckedListener { _, id, checked ->
            if (!checked) return@addOnButtonCheckedListener
            LeafSettings.displayMode = when (id) {
                R.id.leaf_display_16_9 -> LeafSettings.DISPLAY_16_9
                R.id.leaf_display_4_3 -> LeafSettings.DISPLAY_4_3
                R.id.leaf_display_original -> LeafSettings.DISPLAY_ORIGINAL
                else -> LeafSettings.DISPLAY_AUTO
            }
        }

        binding.leafResolutionGroup.check(
            when (LeafSettings.resolution) {
                1 -> R.id.leaf_resolution_1x
                3 -> R.id.leaf_resolution_3x
                else -> R.id.leaf_resolution_2x
            }
        )
        binding.leafResolutionGroup.addOnButtonCheckedListener { _, id, checked ->
            if (!checked) return@addOnButtonCheckedListener
            LeafSettings.resolution = when (id) {
                R.id.leaf_resolution_1x -> 1
                R.id.leaf_resolution_3x -> 3
                else -> 2
            }
        }

        binding.leafTouchGroup.check(
            when (LeafSettings.touchControls) {
                LeafSettings.TOUCH_CONTROLS_ON -> R.id.leaf_touch_on
                LeafSettings.TOUCH_CONTROLS_OFF -> R.id.leaf_touch_off
                else -> R.id.leaf_touch_auto
            }
        )
        binding.leafTouchGroup.addOnButtonCheckedListener { _, id, checked ->
            if (!checked) return@addOnButtonCheckedListener
            LeafSettings.touchControls = when (id) {
                R.id.leaf_touch_on -> LeafSettings.TOUCH_CONTROLS_ON
                R.id.leaf_touch_off -> LeafSettings.TOUCH_CONTROLS_OFF
                else -> LeafSettings.TOUCH_CONTROLS_AUTO
            }
        }

        binding.leafAutoMap.isChecked = LeafSettings.autoMapControllers
        binding.leafAutoMap.setOnCheckedChangeListener { _, checked ->
            LeafSettings.autoMapControllers = checked
            if (checked) LeafSession.autoMapControllerIfNeeded(this, force = true)
        }
        binding.leafNintendoLayout.isChecked = LeafSettings.nintendoLayout
        binding.leafNintendoLayout.setOnCheckedChangeListener { _, checked ->
            LeafSettings.nintendoLayout = checked
            LeafSession.autoMapControllerIfNeeded(this, force = true)
        }
        binding.leafDetectLayout.setOnClickListener { detectLayout() }

        if (savedInstanceState == null) {
            DirectoryInitialization.start()
        }
        if (LeafSettings.romUri.isNotEmpty() && !romIconFile().exists()) {
            try {
                contentResolver.openFileDescriptor(Uri.parse(LeafSettings.romUri), "r")?.use {
                    cacheRomIcon("fd://" + it.fd)
                }
            } catch (e: Exception) {
                // The ROM may have moved; the header simply keeps the app icon.
            }
        }
        binding.leafPlay.requestFocus()
        refresh()
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun hasStorageAccess(): Boolean = when {
        BuildUtil.isGooglePlayBuild -> true
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.R -> Environment.isExternalStorageManager()
        else -> ContextCompat.checkSelfPermission(
            this,
            Manifest.permission.WRITE_EXTERNAL_STORAGE
        ) == PackageManager.PERMISSION_GRANTED
    }

    /**
     * The emulator core reads and writes the save folder directly (much faster than going
     * through Android's document APIs), which needs "All files access" on Android 11+.
     */
    private fun requestStorageAccess() {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.leaf_storage_title)
            .setMessage(R.string.leaf_storage_message)
            .setPositiveButton(R.string.leaf_storage_grant) { _, _ ->
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    allFilesAccess.launch(
                        Intent(
                            android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                            Uri.fromParts("package", packageName, null)
                        )
                    )
                } else {
                    legacyStoragePermission.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun refresh() {
        val folderReady = PermissionsHandler.hasWriteAccess(this) &&
            DirectoryInitialization.areCitraDirectoriesReady()
        binding.leafSaveFolderValue.text = if (folderReady) {
            DocumentFile.fromTreeUri(this, PermissionsHandler.citraDirectory)?.name
                ?: PermissionsHandler.citraDirectory.toString()
        } else {
            getString(R.string.leaf_save_folder_missing)
        }
        binding.leafRomValue.text = LeafSettings.romTitle.ifEmpty {
            getString(R.string.leaf_rom_missing)
        }
        val icon = romIconFile()
        if (LeafSettings.romUri.isNotEmpty() && icon.exists()) {
            BitmapFactory.decodeFile(icon.path)?.let { bitmap ->
                binding.leafRomIcon.setImageDrawable(
                    BitmapDrawable(resources, bitmap).apply { isFilterBitmap = false }
                )
            }
        } else {
            binding.leafRomIcon.setImageResource(R.drawable.ic_leaf)
        }
        binding.leafPlay.isEnabled = true
    }

    /** Asks for the right-hand face button: Nintendo-style pads report it as BUTTON_A. */
    private fun detectLayout() {
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(R.string.leaf_detect_layout)
            .setMessage(R.string.leaf_detect_layout_message)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
        dialog.setOnKeyListener { _, keyCode, event ->
            if (event.action != KeyEvent.ACTION_UP) return@setOnKeyListener false
            val nintendo = when (keyCode) {
                KeyEvent.KEYCODE_BUTTON_A -> true
                KeyEvent.KEYCODE_BUTTON_B -> false
                else -> return@setOnKeyListener false
            }
            LeafSettings.nintendoLayout = nintendo
            binding.leafNintendoLayout.isChecked = nintendo
            LeafSession.autoMapControllerIfNeeded(this, force = true)
            dialog.dismiss()
            true
        }
        dialog.show()
    }

    /** Writes the Leaf choices that live in Azahar's own config before the core reads it. */
    private fun applyEmulatorSettings() {
        val settings = Settings()
        settings.loadSettings()
        IntSetting.RESOLUTION_FACTOR.int = LeafSettings.resolution
        settings.saveSetting(IntSetting.RESOLUTION_FACTOR, SettingsFile.FILE_NAME_CONFIG)
        if (!LeafSettings.sharpUiRemoved) {
            BooleanSetting.CUSTOM_TEXTURES.boolean = false
            settings.saveSetting(BooleanSetting.CUSTOM_TEXTURES, SettingsFile.FILE_NAME_CONFIG)
            LeafSettings.sharpUiRemoved = true
        }
        // New Leaf slows down by itself in busy scenes (many villagers, weather) when the
        // emulated 3DS CPU runs out of time within a frame. A faster emulated clock gives it
        // headroom; it costs nothing while the game is idle.
        IntSetting.CPU_CLOCK_SPEED.int = LeafSettings.cpuClock
        settings.saveSetting(IntSetting.CPU_CLOCK_SPEED, SettingsFile.FILE_NAME_CONFIG)
        // Compile new shaders in the background instead of pausing the game (walking into new
        // areas otherwise stutters until the disk shader cache has seen everything).
        BooleanSetting.ASYNC_SHADERS.boolean = true
        settings.saveSetting(BooleanSetting.ASYNC_SHADERS, SettingsFile.FILE_NAME_CONFIG)
    }

    private fun romIconFile() = File(filesDir, "leaf_rom_icon.png")

    /** Caches the game's HOME-menu icon (48x48, from the user's own ROM) for the header. */
    private fun cacheRomIcon(path: String) {
        val pixels = try {
            GameInfo(path).getIcon()
        } catch (e: Exception) {
            null
        } ?: return
        val bitmap = Bitmap.createBitmap(48, 48, Bitmap.Config.RGB_565)
        bitmap.copyPixelsFromBuffer(IntBuffer.wrap(pixels))
        romIconFile().outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    private fun onRomChosen(uri: Uri) {
        val titleId = try {
            contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
                val path = "fd://" + pfd.fd
                NativeLibrary.getTitleId(path).also { titleId ->
                    if (LeafNative.isLeafTitle(titleId)) cacheRomIcon(path)
                }
            } ?: 0L
        } catch (e: Exception) {
            0L
        }
        if (titleId == 0L) {
            Toast.makeText(this, R.string.leaf_rom_unreadable, Toast.LENGTH_LONG).show()
            return
        }
        if (!LeafNative.isLeafTitle(titleId)) {
            Toast.makeText(this, R.string.leaf_not_leaf_rom, Toast.LENGTH_LONG).show()
            return
        }
        contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        LeafSettings.romUri = uri.toString()
        LeafSettings.romTitle = FileUtil.getFilename(uri)
        refresh()
    }

    private fun play() {
        val folderReady = PermissionsHandler.hasWriteAccess(this) &&
            DirectoryInitialization.areCitraDirectoriesReady()
        val rom = LeafSettings.romUri
        if (!hasStorageAccess()) {
            requestStorageAccess()
            return
        }
        if (!folderReady || rom.isEmpty()) {
            Toast.makeText(this, R.string.leaf_play_needs_setup, Toast.LENGTH_LONG).show()
            if (!folderReady) {
                PermissionsHandler.compatibleSelectDirectory(chooseFolder)
            } else {
                chooseRom.launch(arrayOf("*/*"))
            }
            return
        }
        applyEmulatorSettings()
        startActivity(
            Intent(this, EmulationActivity::class.java).apply {
                action = Intent.ACTION_VIEW
                data = Uri.parse(rom)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
        )
    }
}
