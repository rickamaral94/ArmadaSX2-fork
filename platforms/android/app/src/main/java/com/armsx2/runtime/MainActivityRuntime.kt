Warning: truncated output (original token count: 71631)
Total output lines: 5008

package com.armsx2.runtime

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.res.Configuration
import android.content.pm.ActivityInfo
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.os.Process
import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.addCallback
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.ActivityResult
import androidx.activity.result.contract.ActivityResultContracts.StartActivityForResult
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Text
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.ViewCompat
import androidx.lifecycle.lifecycleScope
import com.armsx2.BuildConfig
import com.armsx2.EmuState
import com.armsx2.FilenameParser
import com.armsx2.GameInfo
import com.armsx2.PlayTime
import com.armsx2.input.ControllerMappings
import com.armsx2.input.SoftKeyboard
import com.armsx2.runtime.MainActivityRuntime.Companion.internalBiosDir
import com.armsx2.runtime.MainActivityRuntime.Companion.romsDirs
import com.armsx2.ui.Colors
import com.armsx2.ui.InGameOverlay
import com.armsx2.ui.WindowImpl
import compose.icons.LineAwesomeIcons
import compose.icons.lineawesomeicons.Android
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import kotlinx.coroutines.launch
import kr.co.iefriends.pcsx2.MainActivity
import kr.co.iefriends.pcsx2.NativeApp
import org.libsdl.app.HIDDeviceManager
import org.libsdl.app.SDLControllerManager
import java.io.File
import java.io.IOException
import java.util.concurrent.Executors
import kotlin.math.abs
import kotlin.math.min
import androidx.core.net.toUri
import androidx.core.content.edit

private const val LIGHT_NAVIGATION_BAR_SCRIM = 0x04000000
private const val DARK_NAVIGATION_BAR_SCRIM = 0x0A000000

private const val STICK_DEAD = 0.15f
// Trigger (L2/R2) dead-low: much smaller than the stick deadzone — triggers want fine
// control and full range. Just enough to swallow resting-axis noise on cheaper / non-Xbox
// pads; the value is re-normalized past it (see sendTrigger) so pressure ramps smoothly
// from 0 instead of flickering on/off at a hard threshold — the jitter those pads showed.
private const val TRIGGER_DEAD = 0.06f
// Travel past which a trigger counts as the L2/R2 BUTTON being held rather than a pressure
// value — what the bind capture records, what fires a trigger-bound hotkey, and what makes a
// held trigger a combo modifier. Well above TRIGGER_DEAD: pressure ramps from a brush, but
// "pressed" should mean a deliberate press.
private const val TRIGGER_DIGITAL_THRESHOLD = 0.5f
// Threshold past which a stick remapped to D-pad / face buttons registers as a
// digital press. Higher than STICK_DEAD so a resting/wobbling stick doesn't fire.
private const val STICK_DIGITAL_THRESHOLD = 0.5f
// Off-axis bleed gate for the RADIAL analog path (accumStickRadial): the minor axis is
// dropped when it's below this fraction of the major axis, so a near-cardinal push on a
// stick that isn't perfectly centered on the other axis doesn't leak a phantom second
// direction ("up also presses right"). 0.15 ≈ snaps only ~<9° diagonals to the cardinal;
// genuine diagonals (minor axis well above this) pass through untouched.
private const val STICK_CROSS_GATE = 0.15f
private const val UI_NAV_DEAD = 0.20f
private const val UI_NAV_RELEASE_DEAD = 0.06f
private const val UI_HAT_DEAD = 0.50f
private const val UI_NAV_DOMINANCE = 1.35f
private const val UI_OVERLAY_RELEASE_MS = 80L
private const val UI_KEY_AXIS_SUPPRESS_MS = 220L
// Hold-to-repeat cadence for controller menu navigation: first auto-repeat
// after the initial hold, then steady repeats while the stick/dpad is held.
private const val NAV_REPEAT_INITIAL_MS = 340L
private const val NAV_REPEAT_INTERVAL_MS = 110L

// During a hotkey capture, a 2nd keycode arriving within this window of the
// first DOWN is treated as part of the SAME physical press (some controllers
// emit two codes per button) rather than a deliberate modifier+key combo.
// A real combo is a held first button + a later second press, well past this.
private const val COMBO_MIN_GAP_MS = 40L

open class MainActivityRuntime : ComponentActivity() {
    private var lastUiNavCode = 0
    private var lastUiNavAt = 0L
    private var lastUiNavWasAxis = false
    private var overlayAxisX = 0
    private var overlayAxisY = 0
    private var overlayHorizontalReleaseAt = 0L
    private var libraryAxisX = 0
    private var libraryAxisY = 0

    companion object {
        var instance: MainActivityRuntime? = null
        lateinit var prefs: SharedPreferences
        val setupComplete = mutableStateOf(false)
        // Set at launch when a restored-but-unusable setup is detected (Auto Backup
        // brought back prefs incl. setupComplete, but the ROMs folder permission
        // didn't survive the reinstall). Drives a one-time explanatory toast; the
        // wizard is re-shown so the user can re-grant folder access.
        val setupRecoveryNeeded = mutableStateOf(false)
        val setupEditorVisible = mutableStateOf(false)
        val nativeReady = mutableStateOf(false)
        // Tree URI of the user-picked PCSX2 system folder (where bios/,
        // memcards/, etc. should live). Persisted as `systemDir` pref.
        // When unset, emucore falls back to getExternalFilesDir(null)
        // (Android/data/<package>/files).
        val systemDir = mutableStateOf<String?>(null)
        val bios = mutableStateOf<String?>(null)
        // Tree URI of the folder the user picked their BIOS from. Persisted
        // separately from `bios` (the path of the copied private file) so
        // re-entering setup can re-scan the original folder without
        // forcing the user to re-pick.
        val biosDir = mutableStateOf<String?>(null)

        /** Persisted list of ROM-folder tree URIs. Replaces the legacy
         *  single-folder `romsDir` pref (kept readable as a one-element
         *  list at load time). The setup wizard's ROMs page lets the user
         *  add/remove entries; the library scans every entry and merges
         *  results de-duplicated by URI. Empty list = no library. */
        val romsDirs = mutableStateOf<List<String>>(emptyList())

        /** Update [romsDirs] state and persist as JSON. Drops the legacy
         *  single-string pref so we don't keep two views in sync forever. */
        fun setRomsDirs(dirs: List<String>) {
            romsDirs.value = dirs
            val arr = org.json.JSONArray()
            for (d in dirs) arr.put(d)
            prefs.edit {
                putString("romsDirs", arr.toString())
                    .remove("roms")
            }
        }

        // Default backend is "auto" — emucore's GSUtil::GetPreferredRenderer
        // picks at runtime per device. The setup wizard no longer asks; the
        // in-game overlay's Renderer tab is where users override (OpenGL /
        // Software cycle, plus Mali/Adreno-specific paths once those land).
        // `upscale` (1.0..5.0) still persists; it's exposed in the in-game
        // overlay's Renderer tab.
        val renderer = mutableStateOf("auto")
        val upscale = androidx.compose.runtime.mutableFloatStateOf(1.0f)

        /** Active custom Vulkan driver id (matches `CustomDriver.InstalledDriver.id`).
         *  Null = system Vulkan loader. Set from the setup wizard's driver
         *  chip. Applied to native via CustomDriver.applyToNative inside
         *  applyRendererPrefs BEFORE runVMThread enters MTGS::Open, which
         *  is when Vulkan::LoadVulkanLibrary reads the pinned path. */
        val customDriverId = mutableStateOf<String?>(null)

        private val eDispatcher = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
        private val eScope = CoroutineScope(eDispatcher)

        /**
         * Scope for boot-time synthetic input that must run WHILE the VM is booting.
         *
         * It cannot be [eScope]: [eDispatcher] is a single thread, and start()'s `invoke { }` block
         * occupies it with the BLOCKING `NativeApp.runVMThread()` for the entire game session.
         * Coroutine dispatch is cooperative, so a job launched on [eScope] during boot is starved
         * until the game EXITS and the thread frees — which is exactly why the Auto-Progressive-Scan
         * Triangle+Cross hold never fired for anyone (it "released" 15 s after a long-dead VM). Run it
         * on an independent pool instead — the same shape EmuCoreX uses (`Dispatchers.Default`). Every
         * JNI it touches (setPadButton/hasActiveVM/getGameCRC) is already thread-safe.
         */
        private val auxScope = CoroutineScope(kotlinx.coroutines.SupervisorJob() + Dispatchers.Default)

        /**
         * Resolve the user-chosen system folder (a SAF tree URI persisted
         * as `systemDir`) to a POSIX path emucore can use as
         * `EmuFolders::DataRoot`. Memcards / savestates / configs land
         * under it.
         *
         * Tree URIs from OpenDocumentTree look like
         *   content://com.android.externalstorage.documents/tree/primary%3APCSX2
         * The "primary:" prefix means the volume is the primary external
         * storage (`/storage/emulated/0`); other prefixes are SD-card or
         * removable volume IDs which mount under `/storage/<volumeId>`.
         *
         * Returns null when systemDir is unset, malformed, or this
         * Android build can't translate the tree URI (rare). Caller
         * falls back to the app's externalFilesDir in that case.
         *
         * Caveat: emucore's POSIX FileSystem APIs require the resolved path to
         * be writable without broad shared-storage privileges. On modern
         * Android, that generally means app-private storage.
         */
        fun systemDirPosix(): String? {
            val v = systemDir.value ?: return null
            // Volume-choice model stores an absolute app-specific path directly
            // (e.g. the SD card's Android/data/<pkg>/files). Legacy installs may
            // still hold a SAF tree-URI string; resolve those the old way.
            return if (v.startsWith("content://")) resolveTreeUriToPosix(v) else v
        }

        /** `<DataRoot>/inputprofiles/` — the portable folder both touch-layout and
         *  controller-mapping profiles mirror themselves into, so they survive a
         *  data-folder move and can be shared or hand-dropped. Null when no system
         *  dir is configured yet; created on demand.
         *
         *  Lives here rather than in either profile store because BOTH need it and
         *  the fallback below is the subtle part: systemDirPosix() is null for the
         *  DEFAULT (private app folder), so it falls back to getExternalFilesDir,
         *  which is exactly where the native core puts EmuFolders::InputProfiles.
         *  Two copies of that reasoning would be one copy too many. */
        fun inputProfilesDir(): File? {
            val root = systemDirPosix()
                ?: instance?.applicationContext?.getExternalFilesDir(null)?.absolutePath
                ?: return null
            val dir = File(root, "inputprofiles")
            if (!dir.exists()) runCatching { dir.mkdirs() }
            return if (dir.isDirectory) dir else null
        }

        /** App-specific data dir on a removable/secondary volume (SD card),
         *  e.g. /storage/<volId>/Android/data/<pkg>/files. Always raw-writable
         *  by the native core with NO permission under scoped storage, which is
         *  why it works on the Play build where arbitrary folders cannot.
         *  getExternalFilesDirs()[0] is primary/internal; [1..] are removable
         *  volumes (entries may be null while a card is unmounting). Returns the
         *  first usable secondary path, or null when no SD card is present. */
        fun sdCardDataDir(context: Context): String? {
            val dirs = context.getExternalFilesDirs(null)
            for (i in 1 until dirs.size) {
                val d = dirs[i] ?: continue
                return d.absolutePath
            }
            return null
        }

        /** Directory holding the configured BIOS file, used by
         *  NativeApp.initializeOnce to point EmuFolders::Bios at the real
         *  BIOS location. Null when no BIOS is configured yet —
         *  initializeOnce then falls back to [internalBiosDir]. */
        fun biosFolderPosix(): String? =
            bios.value?.takeIf { it.isNotEmpty() }?.let { File(it).parent }

        /** App-private BIOS folder, ALWAYS readable by the native core regardless
         *  of the chosen data root. The BIOS must live here (NOT under a custom /
         *  SD systemDir): on Android 11+ the native FileSystem APIs can't reliably
         *  open a BIOS that sits on a removable volume or a SAF-picked folder, so a
         *  game booted with the data root on SD failed VM init (BIOS load) and
         *  bounced back to the library. This mirrors the design documented in
         *  native-lib initialize() ("p_szbiosfolder is always externalFilesDir/bios").
         *  Memcards / saves / configs still follow the chosen data root. */
        fun internalBiosDir(context: Context): File =
            File(context.getExternalFilesDir(null) ?: context.dataDir, "bios")

        /** URI-string-independent POSIX resolver. Pulled out of
         *  systemDirPosix so the setup wizard can probe a freshly-picked
         *  URI for writability before persisting it. Returns null if the
         *  URI is malformed or its volume ID isn't translatable. */
        fun resolveTreeUriToPosix(uriString: String?): String? {
            val raw = uriString ?: return null
            val uri = try {
                raw.toUri() } catch (_: Exception) { return null }
            val docId = try {
                android.provider.DocumentsContract.getTreeDocumentId(uri)
            } catch (_: Exception) { null } ?: return null
            val parts = docId.split(":", limit = 2)
            if (parts.size != 2) return null
            val (volumeId, relPath) = parts
            return when (volumeId) {
                "primary" -> "/storage/emulated/0/$relPath"
                else -> "/storage/$volumeId/$relPath"
            }
        }

        /**
         * Probe the resolved POSIX path for emucore-compatible write
         * access. Creates a `.armsx2-write-probe` file, deletes it,
         * returns true on success.
         *
         * Catches the scoped-storage trap: Android lets the SAF tree-URI
         * permission survive the picker, so reads work, but raw `fopen`/`mkdir`
         * from emucore can still fail with EACCES during memcard / savestate /
         * config generation. We probe up-front so the wizard can refuse to
         * advance and keep writable emulator data in app-private storage.
         */
        fun validateSystemDirWritable(posixPath: String): Boolean {
            return try {
                val dir = File(posixPath)
                if (!dir.exists() && !dir.mkdirs()) return false
                if (!dir.isDirectory) return false
                val probe = File(dir, ".armsx2-write-probe")
                val ok = probe.createNewFile()
                if (ok) probe.delete()
                ok
            } catch (_: Exception) {
                false
            }
        }

        val surface = mutableStateOf<EmulationSurface?>(null)

        @JvmField
        val eState = mutableStateOf(EmuState.STOPPED)

        // Active quick save/load slot (0-9), cycled by the "Cycle Save Slot"
        // hotkey. Quick Save/Load State hotkeys read this so users aren't pinned
        // to slot 0.
        val currentSaveSlot = androidx.compose.runtime.mutableIntStateOf(0)

        // Limiter mode fast-forward engages. Unlimited (3), NOT Turbo (1): Turbo caps at
        // EmulationSpeed.TurboScalar (2.0x) and reporters consistently saw no speed-up from
        // it, while "frame limit off" — which is this same mode 3 — visibly fast-forwarded.
        // Use the path that demonstrably works instead of shipping a second one that doesn't.
        const val FF_LIMITER_MODE = 3

        // Fast-forward SPEED slider (in-game pause menu, under Frame Limit). Stored as an integer
        // multiplier 2..10 (×); FF_SPEED_UNLIMITED = no cap, which reuses the mode-3 uncapped path
        // above and is the default (unchanged behaviour). Below the top, ffLimiterMode() pushes the
        // Turbo scalar to native and engages Turbo (mode 1) so fast-forward runs at the chosen speed.
        const val FF_SPEED_UNLIMITED = 11
        private const val KEY_FF_SPEED = "ff.speed"
        fun fastForwardSpeed(): Int =
            runCatching { prefs.getInt(KEY_FF_SPEED, FF_SPEED_UNLIMITED) }
                .getOrDefault(FF_SPEED_UNLIMITED).coerceIn(2, FF_SPEED_UNLIMITED)

        fun setFastForwardSpeed(v: Int) {
            runCatching { prefs.edit().putInt(KEY_FF_SPEED, v.coerceIn(2, FF_SPEED_UNLIMITED)).apply() }
        }

        /** Limiter mode for engaging fast-forward, honouring the FF-speed slider. Unlimited at the
         *  top (mode 3); otherwise push the Turbo scalar and return Turbo (mode 1). Call only when
         *  actually engaging FF — it has the side effect of setting the scalar. */
        fun ffLimiterMode(): Int {
            val s = fastForwardSpeed()
            if (s >= FF_SPEED_UNLIMITED) return FF_LIMITER_MODE
            runCatching { NativeApp.setTurboScalar(s.toFloat()) }
            return 1 // Turbo, at the scalar just pushed
        }

        // Latched state for the "Fast Forward (toggle)" hotkey: each press flips between
        // fast-forward and the base limiter mode (vs. the hold variant which is momentary).
        // Reset to false whenever a game starts.
        @Volatile var fastForwardToggleActive = false

        /** Read-only view of the fast-forward latch, for UI that only needs to DISPLAY it (the
         *  second-display panel shows a ▶▶ marker while carrying the OSD). */
        fun isFastForwardActive(): Boolean = fastForwardToggleActive

        // Latched state for the "Slow Down (toggle)" hotkey (LimiterModeType::Slomo).
        // Mutually exclusive with the fast-forward latch; blocked in RA hardcore.
        @Volatile var slowDownToggleActive = false

        // Runtime gyro enable (issue #337), driven by the GYRO_TOGGLE / GYRO_HOLD hotkeys.
        // Session-only by design — a mid-game silence, not a persisted preference, so it
        // never contradicts the Gyro Mode setting the user chose. Compose state:
        // TouchControlsOverlay's DisposableEffect keys on it and starts/stops the sensor.
        // Stopping emits (0,0), which releases the gyro's contribution to the merged
        // stick, so the physical stick is left driving on its own.
        val gyroActive = mutableStateOf(true)

        // Bridge to the live AndroidGyroscopeInput's recenter(). The sensor instance is
        // remembered inside TouchControlsOverlay, so the runtime (which owns hotkey
        // dispatch) has no other handle on it. Set while a gyro session is registered and
        // nulled on dispose, so GYRO_RECENTER can tell "no motion running" from a real
        // recenter instead of silently doing nothing.
        @Volatile var gyroRecenterHook: (() -> Unit)? = null

        // #254: whether the emulated USB keyboard is attached for the running
        // game (resolved Settings.usbKeyboard, cached at launch in
        // applyRendererPrefs). Read hot in dispatchKeyEvent to decide whether a
        // physical keyboard's key events should be forwarded to the USB device
        // instead of driving the pad / frontend. Cheap flag so the per-event
        // path doesn't touch ConfigStore.
        @Volatile var usbKeyboardActive = false

        /** TOGGLE_KEYBOARD hotkey: raise or drop the Android IME that feeds the emulated USB
         *  keyboard. Bound to a spare pad button so chat can be opened mid-game without
         *  pausing — which is the whole point, and why this isn't a settings toggle.
         *
         *  Reports instead of silently doing nothing when the USB keyboard isn't attached:
         *  the keystrokes would go nowhere and the user would have no way to tell why. */
        fun toggleSoftKeyboard() {
            val act = instance ?: return
            if (eState.value == EmuState.STOPPED) return
            if (!usbKeyboardActive) {
                act.runOnUiThread {
                    android.widget.Toast.makeText(
                        act,
                        "Turn on Emulate USB Keyboard (Network settings) first",
                        android.widget.Toast.LENGTH_SHORT,
                    ).show()
                }
                return
            }
            act.runOnUiThread { SoftKeyboard.toggle(act) }
        }

        // Cached metadata for the currently-running game. Populated when
        // The library opens a card (so we have title, serial, compatibility,
        // extension and the cover URL ready), cleared when the user
        // launches via paths that don't have a GameInfo handy (Swap/Boot Disc
        // file picker, BIOS-only boot). InGameOverlay reads this for its
        // top-left game info block — falls back to NativeApp.getPause* +
        // a runtime compat lookup when it's null.
        val currentGame = mutableStateOf<GameInfo?>(null)

        val focusRequester = FocusRequester()

        private var m_szGamefile = ""
        private val pendingExternalLaunch = mutableStateOf<String?>(null)
        // A library game tapped before native init finished — deferred and fired once
        // nativeReady. Fixes the first-cold-launch / DeX crash: applyRendererPrefs
        // pushed GS settings before the base settings layer existed → native SIGSEGV.
        private val pendingLaunch = mutableStateOf<Pair<String, GameInfo?>?>(null)

        fun invoke(task: suspend () -> Unit) {
            eScope.launch {
                task()
            }
        }

        private val vmLifecycleLock = Any()
        @Volatile private var vmStopInProgress = false
        @Volatile private var vmRestartAfterStop = false
        @Volatile private var vmRunLoopActive = false

        // Quit-after-the-VM-stops latch — set by the "Close Game & Quit" hotkey, or by
        // a frontend-launched game's Close Game. One-shot: read+cleared by
        // finishToLauncherIfRequested in whichever terminal STOPPED branch fires first.
        @Volatile var quitAfterStop = false
        // True while the CURRENT game was launched from an external frontend intent.
        @Volatile var launchedExternally = false

        /** Terminal (non-restart) STOPPED branches call this: if a quit was requested,
         *  finish the Activity back to the launcher/frontend AFTER the VM has fully
         *  unwound and flushed (memcards/savestate). Marshalled to the UI thread. */
        private fun finishToLauncherIfRequested() {
            if (quitAfterStop) {
                quitAfterStop = false
                instance?.runOnUiThread { instance?.finishAndRemoveTask() }
            }
        }

        /** Close the running game the way the user asked for. When the game came from an
         *  external frontend (ES-DE / Cocoon / Daijishō) and the opt-in is on, finish the
         *  app so the frontend regains focus instead of dropping the user into the ARMSX2
         *  library.
         *
         *  EVERY close route must come through here. The hotkeys used to inline this check
         *  while the in-game menu's Close action called stop() directly, so the menu
         *  silently ignored "Exit to launcher on close" and users had to bind a hotkey to
         *  work around it. One chokepoint means the two can't drift apart again. */
        @JvmStatic
        fun closeGame(saveAutosave: Boolean = false) {
            if (launchedExternally && prefs.getBoolean("ui.exitToLauncherExternal", true))
                quitAfterStop = true
            stop(saveAutosave = saveAutosave)
        }

        /** Fully exit the app (the library Exit button and hold-back gesture route
         *  here). VM-safe: if a game is running, flush it first (quitAfterStop +
         *  async stop(), which finishes once the VM unwinds via the STOPPED branch);
         *  if already stopped, finish immediately. Never finish inline on a running
         *  VM — stop() is async and inline finish would skip the memcard/savestate
         *  flush (the same reason QUIT_APP uses the latch). */
        @JvmStatic
        fun exitApp() {
            if (eState.value == EmuState.STOPPED && !vmStopInProgress && !vmRunLoopActive) {
                instance?.runOnUiThread { instance?.finishAndRemoveTask() }
            } else {
                quitAfterStop = true
                stop()
            }
        }

        @JvmStatic
        fun isVmStopInProgress(): Boolean = vmStopInProgress

        /** True from game/BIOS boot until we are back in the library. This — not currentGame —
         *  decides which rotation tier applyEmulationOrientation() uses: a BIOS boot has no
         *  GameInfo yet is still emulation, so keying on currentGame made the BIOS follow the
         *  LAUNCHER rotation (reported as "BIOS ignores the Renderer rotation and goes portrait"). */
        private var emulationOwnsOrientation = false

        /** The single "we're back in the library" cleanup: drop the current-game pointer (so
         *  Settings reverts to Global scope) and hand the Activity's rotation back to the
         *  launcher preference.
         *
         *  This MUST run on every terminal path out of the VM. It used to live only inside
         *  stop()'s post-shutdown branch, which is guarded on `!vmRunLoopActive` — a flag the
         *  VM thread clears from its own finally. stop() usually evaluates that guard first, so
         *  the block was skipped and the surviving path never reverted anything: the launcher
         *  stayed locked in the game's landscape until the process was killed. Idempotent. */
        private fun onReturnedToLibrary() {
            currentGame.value = null
            emulationOwnsOrientation = false
            // Never leave the device pinned once the game is gone (#425).
            com.armsx2.ui.ScreenPinning.stop()
            stopAutoProgressiveScanHold()
            // Drop pressure-modifier bookkeeping: a button still held when the game exits would
            // otherwise stay in the set and be re-emitted into the NEXT session.
            com.armsx2.ui.touch.TouchControls.clearHeldPressureKeys()
            com.armsx2.BatteryWatcher.resetForNewSession()
            instance?.runOnUiThread { instance?.applyEmulationOrientation() }
        }

        // ---- Auto Progressive Scan -------------------------------------------------------
        // Some PS2 titles (Tekken 4, a number of Criterion games) offer 480p progressive output
        // only if Triangle+Cross are held while the game boots — on real hardware you hold them
        // from power-on. We reproduce that as a synthetic pad hold; games without the prompt
        // simply ignore it. Codes match applyPadButton()'s switch in native-lib.cpp.
        private const val PAD_CODE_TRIANGLE = 100
        private const val PAD_CODE_CROSS = 96

        /** How long to keep the combo held. Titles probe it at very different points — some well
         *  after the PS2 logo — so this deliberately spans the whole boot sequence. */
        private const val AUTO_PROGRESSIVE_HOLD_MS = 30_000L
        /// How often the synthetic Triangle+Cross hold is re-pressed. Must be well under a frame
        /// budget's worth of pad polling so the game never samples a gap, and short enough that a
        /// pad re-init can't swallow the whole hold.
        // Achievement-progress capture cadence. The first wait lets RetroAchievements resolve the
        // set over the network after boot; the poll only exists so a process killed mid-session
        // still leaves a recent figure in the library. Neither is latency-sensitive.
        private const val ACHIEVEMENT_SNAPSHOT_FIRST_MS = 15_000L
        private const val ACHIEVEMENT_SNAPSHOT_POLL_MS = 120_000L

        private const val AUTO_PROGRESSIVE_REASSERT_MS = 200L
        /// Keep holding this long after the game's ELF starts, then let go — the 480p prompt is
        /// checked at game start, and holding into the menus would fight the player. 8 s (up from 4)
        /// gives margin for titles that probe a few seconds into the ELF, once past the intro logos;
        /// a continuously-held button presents no fresh press edge, so it won't drive early menus.
        private const val AUTO_PROGRESSIVE_POST_ELF_MS = 8_000L

        /** Pad writes are dropped while no VM exists (applyPadButton bails on !HasValidVM), so
         *  wait for boot rather than pressing into the void. Bounded so a failed boot can't spin. */
        private const val AUTO_PROGRESSIVE_VM_WAIT_MS = 15_000L

        private var autoProgressiveScanJob: Job? = null

        private fun startAutoProgressiveScanHold() {
            stopAutoProgressiveScanHold()
            // ★ auxScope, NOT eScope. The old eScope.launch was the whole bug: eScope's single thread
            // is held by the blocking runVMThread() for the entire session, so this coroutine never
            // got to run during boot — it did nothing for anyone (the "fix" that added re-assertion
            // couldn't run either). On the independent auxScope it runs alongside the booting VM.
            autoProgressiveScanJob = auxScope.launch {
                var held = false
                try {
                    var waited = 0L
                    while (!NativeApp.hasActiveVM() && waited < AUTO_PROGRESSIVE_VM_WAIT_MS) {
                        delay(100)
                        waited += 100
                    }
                    if (!NativeApp.hasActiveVM())
                        return@launch
                    held = true
                    // Re-assert on a short interval rather than pressing once. The state itself
                    // persists (Pad::SetControllerState), so a single press would mostly work — but
                    // re-pressing cheaply survives the pad (re)init during boot ("Pad: DS2 Config
                    // Finished" lands after the VM goes active) with no gap for the game to sample.
                    //
                    // Release shortly after the game's own ELF starts rather than blocking for the
                    // full timeout: the 480p prompt is checked at game start, and continuing to jam
                    // Triangle+Cross into a booted game would fight the player in the menus. CRC
                    // goes non-zero exactly when the ELF is running, so it is the right edge to
                    // watch. AUTO_PROGRESSIVE_HOLD_MS remains the hard ceiling.
                    var elapsed = 0L
                    var sinceElf = -1L
                    while (elapsed < AUTO_PROGRESSIVE_HOLD_MS) {
                        if (!NativeApp.hasActiveVM())
                            return@launch
                        NativeApp.setPadButton(PAD_CODE_TRIANGLE, 0, true)
                        NativeApp.setPadButton(PAD_CODE_CROSS, 0, true)
                        delay(AUTO_PROGRESSIVE_REASSERT_MS)
                        elapsed += AUTO_PROGRESSIVE_REASSERT_MS
                        val elfRunning = runCatching { NativeApp.getGameCRC() }.getOrNull()
                            ?.let { it.length == 8 && it != "00000000" } ?: false
                        if (elfRunning) {
                            if (sinceElf < 0) sinceElf = 0
                            else sinceElf += AUTO_PROGRESSIVE_REASSERT_MS
                            if (sinceElf >= AUTO_PROGRESSIVE_POST_ELF_MS)
                                break
                        }
                    }
                } finally {
                    // Release on every exit path, cancellation included — a stuck Triangle+Cross
                    // would make the game unplayable. These are plain JNI calls, not suspends, so
                    // they still run in a cancelled coroutine.
                    if (held && NativeApp.hasActiveVM()) {
                        NativeApp.setPadButton(PAD_CODE_TRIANGLE, 0, false)
                        NativeApp.setPadButton(PAD_CODE_CROSS, 0, false)
                    }
                }
            }
        }

        private fun stopAutoProgressiveScanHold() {
            autoProgressiveScanJob?.cancel()
            autoProgressiveScanJob = null
        }

        fun start() {
            synchronized(vmLifecycleLock) {
                if (vmStopInProgress || vmRunLoopActive || eState.value != EmuState.STOPPED) {
                    vmRestartAfterStop = true
                    return
                }
                vmRunLoopActive = true
            }

            invoke {
                try {
                    eState.value = EmuState.RUNNING
                    println("@@ANDROID_START_VM@@ kind=game path=${m_szGamefile.take(240)}")
                    // Local co-op: re-pair controllers each session (first pad = P1,
                    // next = P2) so player slots are deterministic per boot.
                    com.armsx2.input.PadRouter.reset()
                    WindowImpl.showLibrary.value = false
                    WindowImpl.overlayVisible.value = false
                    WindowImpl.toolbarVisible.value = false
                    emulationOwnsOrientation = true
                    // Opt-in only: blocks a controller's Home button from minimising the game,
                    // which the app cannot do any other way — HOME never reaches us (#425).
                    instance?.let { com.armsx2.ui.ScreenPinning.start(it) }
                    applyRendererPrefs()
                    // Both of these are consumed by native when the VM boots, so they must be
                    // pushed BEFORE runVMThread (which blocks until the VM exits). One resolve,
                    // per-game ∘ global.
                    val bootCfg = com.armsx2.config.ConfigStore
                        .resolveForGame(currentGame.value?.settingsKey)
                    // Read by VMManager::SetEmuThreadAffinities during boot.
                    runCatching { NativeApp.setAffinityMode(bootCfg.affinityMode) }
                    // The hold itself waits for the VM to come up. BIOS boots skip it.
                    if (bootCfg.autoProgressiveScan)
                        startAutoProgressiveScanHold()
                    NativeApp.runVMThread(m_szGamefile)
                } finally {
                    // runVMThread blocks until the VM exits (Stopping/Shutdown
                    // observed). Drop back to STOPPED only after native has
                    // actually unwound, so users can't launch the next game
                    // while the previous VM is still tearing down.
                    eState.value = EmuState.STOPPED
                    val restartNow = synchronized(vmLifecycleLock) {
                        vmRunLoopActive = false
                        vmStopInProgress = false
                        if (vmRestartAfterStop) {
                            vmRestartAfterStop = false
                            true
                        } else {
                            false
                        }
                    }
                    if (restartNow) {
                        start()
                    } else {
                        WindowImpl.toolbarVisible.value = true
                        WindowImpl.showLibrary.value = false
                        WindowImpl.overlayVisible.value = false
                        // This is the branch that actually fires on a normal game exit (stop()'s
                        // equivalent block loses the vmRunLoopActive race), so the return-to-library
                        // cleanup has to happen here or the launcher keeps the game's rotation.
                        onReturnedToLibrary()
                        finishToLauncherIfRequested()
                    }
                }
            }
        }

        /** Push setup-wizard renderer/upscale choices into emucore's
         *  EmuConfig before runVMThread. ApplySettings inside Initialize
         *  picks them up; if a VM is already up, the JNI helpers also
         *  call MTGS::ApplySettings inline.
         *
         *  Also resolves and applies the per-game / global Settings from
         *  ConfigStore (MTVU and friends) — currentGame.serial picks the
         *  right override tier; null falls back to global. Resolution
         *  order: per-game JSON overlay → global → hardcoded defaults. */
        /** Number of distinct physical gamepads/joysticks connected right now
         *  (excludes virtual devices). Drives the boot-time PS2-port-2 enable for
         *  local co-op — 2+ pads → connect Player 2's controller at VM init. */
        private fun connectedGamepadCount(): Int {
            var n = 0
            var sawJoyCon = false
            for (id in InputDevice.getDeviceIds()) {
                val dev = InputDevice.getDevice(id) ?: continue
                if (dev.isVirtual) continue
                val s = dev.sources
                val isPad = (s and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                    (s and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                if (!isPad) continue
                // Nintendo Joy-Cons (vendor 0x057E) enumerate as two InputDevices per pair
                // but are combined onto ONE PS2 port (see PadRouter.portForDevice), so count
                // ALL Nintendo pads as a SINGLE logical controller — a lone pair must not
                // auto-enable PS2 port 2. Every other vendor is still counted per device.
                if (dev.vendorId == 0x057E) { sawJoyCon = true; continue }
                n++
            }
            return n + (if (sawJoyCon) 1 else 0)
        }

        private fun applyRendererPrefs() {
            // Resolve per-game (∘ global) settings up front so the renderer backend
            // and internal resolution come from THIS title's tier, not a stale
            // global value. Sync the session state the Renderer UI reads, too.
            // Resolve via settingsKey (serial for discs, filename stem for
            // serial-less ELF/homebrew) so ELF per-game settings survive a reboot
            // instead of falling back to global (issue #253).
            var resolved = com.armsx2.config.ConfigStore.resolveForGame(currentGame.value?.settingsKey)
            // Build immutable input maps before the VM starts so the first ABXY
            // edge never pays SharedPreferences parsing on the UI thread.
            ControllerMappings.warmRuntimeCaches()
            // Per-game memory cards (NetherSX2-style): when the global toggle is
            // on, point Slot 1 at a serial-named card (the core auto-creates +
            // formats it at boot) — but ONLY for games still on the factory-default
            // card. A user who assigned a real card (imported/created, globally OR
            // per-game) has a non-default filename, so we must NOT clobber it with a
            // blank serial card. (Bug: the old guard compared against the current
            // global, which a global assignment always equals, so it wiped it.)
            currentGame.value?.serial?.takeIf { it.isNotBlank() }?.let { serial ->
                if (prefs.getBoolean("memcard.perGame", false) &&
                    resolved.memoryCardSlot1Filename.equals("mcd001.ps2", ignoreCase = true)) {
                    resolved = resolved.copy(
                        memoryCardSlot1Filename = "$serial.ps2",
                        memoryCardSlot1Enabled = true,
                    )
                }
            }
            // Per-game BIOS: boot with the game's chosen BIOS if it set one, else fall
            // back to the global BIOS — so a previous game's per-game pick never sticks.
            // The file is in the same app-private BIOS dir as the global one, so only the
            // Filenames/BIOS *filename* changes; commit before the VM's LoadBIOS runs.
            run {
                val effectiveBios = resolved.biosFilename.takeIf { it.isNotBlank() }
                    ?: bios.value?.takeIf { it.isNotEmpty() }?.let { File(it).name }
                if (!effectiveBios.isNullOrBlank()) {
                    NativeApp.setSetting("Filenames", "BIOS", "string", effectiveBios)
                    NativeApp.commitSettings()
                }
            }
            upscale.value = resolved.upscaleFloat
            renderer.value = resolved.renderer
            NativeApp.renderUpscalemultiplier(upscale.value)
            // Pin custom Vulkan driver (if any) BEFORE the renderer write —
            // the renderer JNI may trigger MTGS::ApplySettings which can
            // re-open the GS device and run Vulkan::LoadVulkanLibrary. The
            // VK loader reads the pinned path lazily so the order matters.
            val ctx = instance?.applicationContext
            // Per-game GPU driver: pin THIS title's resolved driver (blank = system). Keep the
            // session mirror in sync so the picker UI + delete/reselect logic stay correct.
            val pickedId = resolved.customDriverId.takeIf { it.isNotBlank() }
            customDriverId.value = pickedId
            val picked: com.armsx2.CustomDriver.InstalledDriver? =
                if (ctx != null) pickedId?.let { id ->
                    com.armsx2.CustomDriver.listInstalled(ctx).firstOrNull { it.id == id }
                } else null
            if (ctx != null) com.armsx2.CustomDriver.applyToNative(ctx, picked)
            when (renderer.value) {
                "vulkan" -> NativeApp.renderVulkan()
                "opengl" -> NativeApp.renderOpenGL()
                "software" -> NativeApp.renderSoftware()
                else -> NativeApp.renderAuto()
            }
            resolved.applyTo()
            // applyTo() pushed the per-stat OSD flags (= "Custom"); re-assert the stored OSD
            // mode on top so a Full / Min / Off choice from the menu or hotkey survives a
            // relaunch instead of snapping back to the per-stat selection every boot.
            InGameOverlay.applyStoredOsdMode()
            // Per-game screen orientation: apply THIS title's rotation now that currentGame is
            // set. requestedOrientation is an Activity property → hop to the UI thread (we're on
            // the VM launch thread here). Reverts to global on exit-to-library (see stop()).
            instance?.runOnUiThread { instance?.applyEmulationOrientation() }
            // #254: cache whether this title runs with the emulated USB keyboard so
            // dispatchKeyEvent can forward physical-keyboard keys to it. applyTo()
            // already pushed [USB1] Type + the live attach (usbSetKeyboardEnabled).
            usbKeyboardActive = resolved.usbKeyboard

            // Neutralize the NATIVE pad analog deadzone before the VM loads [Pad1].
            // A stale [Pad1]/Deadzone in an existing config (from the old, non-saving
            // deadzone slider) imposed a huge ~0.45 fake deadzone on BOTH physical and
            // on-screen sticks (they share PadDualshock2::Set). The app-side "Stick
            // Deadzone" (ControllerMappings.stickDeadzone, re-normalized in
            // shapeStickMag) is the single authority now, so keep the native radial
            // deadzone off so it can't re-deaden the already-shaped input. AxisScale
            // (1.33, helps small sticks reach full deflection) is left untouched.
            runCatching {
                NativeApp.setSetting("Pad1", "Deadzone", "float", "0")
                NativeApp.setSetting("Pad2", "Deadzone", "float", "0")
                // Local co-op: enable PS2 port 2 at BOOT (here, before runVMThread →
                // Pad::LoadConfig) when a second controller is already connected —
                // NOT by hot-plugging it mid-game, which rebuilt the live pad list and
                // crashed. Single controller → "None" (port 2 off; zero change for
                // solo play). So: connect BOTH controllers before launching the game.
                val twoPads = connectedGamepadCount() >= 2
                NativeApp.setSetting("Pad2", "Type", "string", if (twoPads) "DualShock2" else "None")
                if (twoPads) {
                    NativeApp.setSetting("Pad2", "AxisScale", "float", "1.33")
                    NativeApp.setSetting("Pad2", "ButtonDeadzone", "float", "0")
                }
                // PS2 Multitap: when enabled, arm BOTH ports as 4-slot multitaps at BOOT
                // (before runVMThread -> Pad::LoadConfig) so a game launched with 3-8
                // controllers sees them. Flag keys are off-by-one: [Pad] MultitapPort1 ->
                // physical port 0, MultitapPort2 -> port 1. Unified slots: [Pad2] = port-1
                // main, [Pad3..Pad5] = port-0 taps, [Pad6..Pad8] = port-1 taps. Force all
                // 8 slots on for a complete pair of taps; idle slots are harmless (games
                // ignore unused pads). Unconditional-when-ON so a pad joining after boot
                // still lands on a live slot; the Pad-tab toggle covers mid-session enable.
                if (ControllerMappings.multitapEnabled()) {
                    NativeApp.setSetting("Pad", "MultitapPort1", "bool", "true")
                    NativeApp.setSetting("Pad", "MultitapPort2", "bool", "true")
                    for (s in 2..8) {
                        NativeApp.setSetting("Pad$s", "Type", "string", "DualShock2")
                        NativeApp.setSetting("Pad$s", "Deadzone", "float", "0")
                        NativeApp.setSetting("Pad$s", "AxisScale", "float", "1.33")
                        NativeApp.setSetting("Pad$s", "ButtonDeadzone", "float", "0")
                    }
                }
            }

            // Settings.applyTo() above writes the persisted FrameLimitEnable
            // into the BASE settings layer; override it here with the
            // in-session overlay toggle so the user's runtime choice sticks
            // across game launches within one app run. Both writes are
            // needed: native-lib's runVMThread re-reads FrameLimitEnable
            // from the BASE layer right after VMManager::Initialize and
            // calls SetLimiterMode based on that, so a bare
            // speedhackLimitermode() here gets clobbered by VM init.
            // Mode 0 = Nominal (60fps cap), 3 = Unlimited.
            val limit = InGameOverlay.frameLimitOn.value
            NativeApp.setSetting("EmuCore/GS", "FrameLimitEnable", "bool", limit.toString())
            // Preserve an active fast-forward / slow-down latch across a settings apply.
            // Otherwise this re-application of the limiter clobbers mode 1/2 back to the base
            // limit while the toggle state stays ON — so fast-forward is "forgotten" and the
            // user has to toggle off then on again to resync. Re-assert the latched mode.
            NativeApp.speedhackLimitermode(
                when {
                    fastForwardToggleActive -> ffLimiterMode()
                    slowDownToggleActive -> 2
                    else -> if (limit) 0 else 3
                }
            )
        }

        /**
         * Set the active game path/URI and restart the VM. Used by
         * Library card taps — the URI comes from the user's persisted
         * ROMs tree (already has read perm) so emucore's FileSystem
         * routines can open it via the content:// JNI bridge.
         *
         * `info` is the GameInfo from the library scan when available;
         * stored on MainActivityRuntime.currentGame so the in-game overlay can show
         * cover art / extension badge / pre-resolved compat stars
         * without re-querying gamedb. Pass null when launching from a
         * path that doesn't have a GameInfo (Swap/Boot Disc file picker).
         */
        fun launchGame(uri: String, info: GameInfo? = null, external: Boolean = false) {
            if (uri.isBlank()) {
                println("@@ANDROID_LAUNCH_REJECT@@ reason=blank_uri title=${info?.title ?: ""}")
                return
            }
            // Remember the game for a post-exit re-launch from the Save Manager (#374).
            if (info != null) contextGame.value = info
            println(
                "@@ANDROID_LAUNCH_GAME@@ title=${info?.title ?: "<direct>"} " +
                    "uri=${uri.take(240)} state=${eState.value} runLoop=$vmRunLoopActive " +
                    "stopping=$vmStopInProgress nativeReady=${nativeReady.value}"
            )
            // Refresh the ANGLE EGL env before the GS thread opens the GL context, so a
            // just-changed AndroidUseAngleOpenGL / renderer choice takes effect on this boot.
            instance?.applicationContext?.let { applyAngleEnv(it) }
            // Native GS/settings calls in start()→applyRendererPrefs null-deref if the
            // base settings layer isn't installed yet (initialize() not finished). On a
            // cold first launch — reliably on Samsung DeX — a fast game tap races init
            // and crashes with no error. Defer until nativeReady; the LaunchedEffect
            // watching pendingLaunch fires it once init completes.
            if (!nativeReady.value) {
                println("@@ANDROID_LAUNCH_DEFER@@ nativeReady=false — queuing '${info?.title ?: uri.take(80)}'")
                pendingLaunch.value = uri to info
                return
            }
            currentGame.value = info
            launchedExternally = external
            // Arm a one-shot auto-load of the autosave state for this boot (fired by
            // onVmRunning once the game's CRC is set). Set here — not in start() — so
            // a manual Reset Game (which re-enters start() directly) doesn't re-load.
            pendingAutoLoadOnBoot = prefs.getBoolean("autoLoadOnBoot", false)
            m_szGamefile = uri
            synchronized(vmLifecycleLock) {
                if (eState.value != EmuState.STOPPED || vmStopInProgress || vmRunLoopActive) {
                    vmRestartAfterStop = true
                }
            }
            if (eState.value == EmuState.STOPPED && !vmStopInProgress && !vmRunLoopActive)
                start()
            else
                stop(restartAfterStop = true)
        }

        private fun launchPendingExternalGameIfReady() {
            if (!setupComplete.value || !nativeReady.value) return
            // A deferred library launch (nativeReady-gated) fires first, keeping its
            // GameInfo so per-game settings / title still apply.
            pendingLaunch.value?.let { (u, i) ->
                pendingLaunch.value = null
                launchGame(u, i)
                return
            }
            val queued = pendingExternalLaunch.value
            if (queued.isNullOrEmpty()) return
            pendingExternalLaunch.value = null
            // Launching from a frontend (Cocoon/Daijisho/ES-DE) used to pass a null GameInfo,
            // so settingsKey was null and launchGame resolved GLOBAL settings — per-game
            // settings, per-game memory cards and per-game orientation all silently ignored,
            // while the same title launched from our own library applied them correctly.
            // Build a GameInfo for the incoming URI so the external path keys off the same
            // settingsKey the library path does (serial for discs, filename stem otherwise).
            launchGame(queued, externalGameInfo(queued), external = true)
        }

        /**
         * Minimal [GameInfo] for a URI arriving from outside the app. The serial is probed
         * off the image the same way the library scan does (SYSTEM.CNF via
         * NativeApp.getGameSerialFromFd) so per-game settings stored under the serial are
         * found. A failed probe is fine and expected for ELF/homebrew — settingsKey then
         * falls back to the filename stem, matching issue #253's behaviour.
         */
        private fun externalGameInfo(uriString: String): GameInfo? = runCatching {
            val uri = uriString.toUri()
            val name = uri.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':').orEmpty()
            val stem = name.substringBeforeLast('.').ifBlank { name }
            val serial = runCatching {
                val ctx = instance ?: return@runCatching null
                // detachFd() hands ownership of the fd to native, which closes it — same
                // contract GameLibraryRepository.probeDocument/probeRaw use. Do NOT wrap in
                // use{}: closing an already-detached descriptor is not ours to do.
                val raw = if (uri.scheme == "content") {
                    val pfd = ctx.contentResolver.openFileDescriptor(uri, "r") ?: return@runCatching null
                    NativeApp.getGameSerialFromFd(pfd.detachFd())
                } else {
                    val f = java.io.File(uri.path ?: return@runCatching null)
                    if (!f.isFile) return@runCatching null
                    val pfd = ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY)
                    NativeApp.getGameSerialFromFd(pfd.detachFd())
                }
                // The probe tags its result "<platform>:<serial>" when SYSTEM.CNF parsed.
                raw?.takeIf { it.isNotBlank() }?.substringAfterLast(':')?.takeIf { it.isNotBlank() }
            }.getOrNull()
            GameInfo(
                uri = uri,
                title = stem,
                serial = serial,
                extension = name.substringAfterLast('.', "").uppercase(),
            )
        }.getOrNull()

        /**
         * Boot to BIOS (no game disc). Unlike `start()` this does NOT
         * hide the toolbar — the BIOS action in the library wants the
         * library/toolbar to remain visible so the user can pick a game
         * once BIOS finishes booting.
         */
        fun startBios() {
            currentGame.value = null
            m_szGamefile = ""
            val shouldStart = synchronized(vmLifecycleLock) {
                if (vmStopInProgress || vmRunLoopActive || eState.value != EmuState.STOPPED) {
                    vmRestartAfterStop = true
                    false
                } else {
                    vmRunLoopActive = true
                    true
                }
            }
            if (!shouldStart) {
                stop(restartAfterStop = true)
                return
            }
            invoke {
                try {
                    eState.value = EmuState.RUNNING
                    println("@@ANDROID_START_VM@@ kind=bios path=<empty>")
                    com.armsx2.input.PadRouter.reset()
                    // The BIOS is emulation too: claim the renderer rotation tier so it honours the
                    // Renderer page (global, since there is no game) instead of the launcher's.
                    emulationOwnsOrientation = true
                    applyRendererPrefs()
                    NativeApp.runVMThread(m_szGamefile)
                } finally {
                    eState.value = EmuState.STOPPED
                    val restartNow = synchronized(vmLifecycleLock) {
                        vmRunLoopActive = false
                        vmStopInProgress = false
                        if (vmRestartAfterStop) {
                            vmRestartAfterStop = false
                            true
                        } else {
                            false
                        }
                    }
                    if (restartNow) {
                        start()
                    } else {
                        // BIOS exit had no cleanup at all — it relied entirely on stop()'s racy
                        // branch, so quitting the BIOS also left the launcher stuck in its rotation.
                        onReturnedToLibrary()
                    }
                }
            }
        }

        // pause/resume run on a dedicated serialized executor, NOT the UI
        // thread. The native side queues the real pause/resume onto the CPU
        // thread, so a fast open→close still lands in the right order without
        // making the Android UI wait for MTVU/MTGS to park.
        // eState is updated by Host::OnVMPaused/Resumed → vmSetPaused, so the
        // UI never claims PAUSED before the VM actually parked.
        private val vmControl = Executors.newSingleThreadExecutor { r ->
            Thread(r, "VMControl")
        }
        private val vmStopControl = Executors.newSingleThreadExecutor { r ->
            Thread(r, "VMStop")
        }

        fun pause() {
            if (vmStopInProgress)
                return
            vmControl.execute {
                if (!vmStopInProgress)
                    NativeApp.pause()
            }
        }

        fun pauseForOverlay() {
            if (vmStopInProgress)
                return
            // Routed through vmControl exactly like resume(), NOT inline. The comment above the
            // executor claims pause and resume are serialised against each other; while this
            // bypassed it they were enqueued from two different Java threads, so a pause raised
            // while a resume was still in flight could be evaluated first and swallowed (native
            // pause() only acts when the VM is exactly Running, and never retries) — which leaves
            // the VM RUNNING in the background after the app is gone.
            vmControl.execute {
                if (vmStopInProgress)
                    return@execute
                pauseForOverlayOnVmThread()
            }
        }

        private fun pauseForOverlayOnVmThread() {
            // ★ Keep the audio device OPEN across an overlay pause. Otherwise SPU2::SetOutputPaused
            // pauses the Oboe stream, Android reclaims an idle low-latency stream after a few
            // seconds (#333), and then the RESUME has to Close/Open/Start it again — inline on the
            // CPU thread, inside the resume task, AHEAD of Host::OnVMResumed(). That is why coming
            // back from another app can sit "stuck on pause" for seconds before the game moves.
            // Suppressed, the stream underruns to silence instead: nothing to reclaim, nothing to
            // rebuild. native-lib.cpp has always documented pauseForOverlay as the caller that sets
            // this — it simply never called it.
            runCatching { NativeApp.setOutputPauseSuppressed(true) }
            NativeApp.pause()
        }

        fun resume() {
            if (vmStopInProgress)
                return
            vmControl.execute {
                if (!vmStopInProgress) {
                    NativeApp.resume()
                    // Cleared only after the resume lands, so a later non-overlay pause (VM stop,
                    // shutdown) still releases the device normally.
                    runCatching { NativeApp.setOutputPauseSuppressed(false) }
                }
            }
        }

        fun stop(saveAutosave: Boolean = false, restartAfterStop: Boolean = false) {
            // Drop any latched fast-forward / slow-down toggle; the next game boots
            // at normal speed. Same for the gyro hotkey latch — a game left with gyro
            // toggled off must not silently start the next one with gyro dead.
            fastForwardToggleActive = false
            slowDownToggleActive = false
            gyroActive.value = true
            val nativeActive = runCatching { NativeApp.hasActiveVM() }.getOrDefault(false)
            val shouldStop = synchronized(vmLifecycleLock) {
                if (restartAfterStop)
                    vmRestartAfterStop = true
                else
                    vmRestartAfterStop = false

                if (vmStopInProgress) {
                    nativeActive
                } else if (eState.value == EmuState.STOPPED && !vmRunLoopActive && !nativeActive) {
                    false
                } else {
                    vmStopInProgress = true
                    true
                }
            }
            if (!shouldStop) {
                // Nothing left to stop (the VM self-terminated). Still reconcile the library
                // state — this early return also used to leak a stale per-game rotation.
                onReturnedToLibrary()
                return
            }

            WindowImpl.overlayVisible.value = false
            WindowImpl.showLibrary.value = false
            // Auto-save-on-exit: any normal close (not a reset/restart) writes the
            // autosave state when the user has the toggle on, so the next boot can
            // auto-load it. An explicit Save-and-Exit still forces it via saveAutosave.
            val doAutosave = saveAutosave ||
                (!restartAfterStop &&
                    runCatching { prefs.getBoolean("autoSaveOnExit", false) }.getOrDefault(false))
            vmStopControl.execute {
                println("@@ANDROID_STOP_JAVA@@ begin saveAutosave=$doAutosave forced=$saveAutosave restart=$restartAfterStop")
                if (doAutosave)
                    NativeApp.saveAutosaveState()
                NativeApp.shutdown()
                println("@@ANDROID_STOP_JAVA@@ shutdown_return active=${NativeApp.hasActiveVM()} runLoop=$vmRunLoopActive state=${eState.value}")
                if (!vmRunLoopActive && (eState.value == EmuState.STOPPED || !NativeApp.hasActiveVM())) {
                    eState.value = EmuState.STOPPED
                    val restartNow = synchronized(vmLifecycleLock) {
                        vmStopInProgress = false
                        if (vmRestartAfterStop) {
                            vmRestartAfterStop = false
                            true
                        } else {
                            false
                        }
                    }
                    if (restartNow) {
                        start()
                    } else {
                        synchronized(vmLifecycleLock) {
                            WindowImpl.toolbarVisible.value = true
                            WindowImpl.showLibrary.value = false
                            WindowImpl.overlayVisible.value = false
                        }
                        // Clear the current-game pointer (so Settings reverts to Global scope) and
                        // hand rotation back to the launcher. Shared with the other terminal paths.
                        onReturnedToLibrary()
                        finishToLauncherIfRequested()
                    }
                }
            }
        }

        fun restart() {
            synchronized(vmLifecycleLock) {
                vmRestartAfterStop = true
            }
            if (eState.value == EmuState.STOPPED && !vmStopInProgress && !vmRunLoopActive)
                start()
            else
                stop(restartAfterStop = true)
        }

        /** Open a file picker to swap the mounted disc WITHOUT rebooting the VM.
         *  The picked URI is handed to NativeApp.changeDisc (see swapDiscAction),
         *  which parks the CPU thread and cycles the CDVD tray so the running game
         *  detects the new disc — needed for multi-disc titles and cheat discs
         *  (CodeBreaker/GameShark) that hand off to a game disc. Bridges Compose
         *  (the in-game menu) to the Activity-scoped ActivityResult launcher; the
         *  picker + native swap were intact but had no trigger after the monorepo
         *  UI migration, so Swap Disc silently did nothing. */
        fun promptSwapDisc() {
            val activity = instance ?: return
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            runCatching { activity.swapDiscAction.launch(intent) }
        }

        /** ANGLE (GLES-on-Vulkan) for the OpenGL renderer. Ported from sashkinbro/EmuCoreX:
         *  when the AndroidUseAngleOpenGL setting is on AND the renderer is OpenGL, point the
         *  ARMSX2_ANGLE_EGL_LIBRARY / _GLES_LIBRARY env vars at the bundled ANGLE .so in the
         *  native-lib dir; GLContextEGL::LoadEGL (native) then loads ANGLE's EGL instead of the
         *  system GLES driver — useful where the native GLES stack is broken (e.g. some MediaTek
         *  Mali). Cleared otherwise. Env vars are read by native getenv in this same process, so
         *  this Kotlin call is the whole hook. MUST run before the GS thread opens the GL context,
         *  so it's invoked at emucore init and before each game launch. Renderer restart applies a
         *  live toggle (like the GPU-profile override). Uses the GLOBAL settings; per-game renderer
         *  overrides are out of scope for v1. */
        fun applyAngleEnv(context: Context) {
            val settings = runCatching { com.armsx2.config.ConfigStore.loadGlobal() }.getOrNull()
            val eligible = settings?.useAngleOpenGL == true && settings.renderer == "opengl"
            val libDir = context.applicationInfo.nativeLibraryDir
            val egl = File(libDir, "libEGL_angle.so")
            val gles = File(libDir, "libGLESv2_angle.so")
            // gsBackThread rides on every line: GV7's back thread is the OTHER ANGLE suspect
            // (ANGLE binds an EGL context to a single thread far more strictly than the native
            // GLES drivers do), so the log has to say whether it was engaged.
            val ctx = "renderer=${settings?.renderer} useAngle=${settings?.useAngleOpenGL} gsBackThread=${settings?.gsBackThreadMode}"
            try {
                if (eligible && egl.exists() && gles.exists()) {
                    android.system.Os.setenv("ARMSX2_ANGLE_EGL_LIBRARY", egl.absolutePath, true)
                    android.system.Os.setenv("ARMSX2_ANGLE_GLES_LIBRARY", gles.absolutePath, true)
                    android.util.Log.i("ARMSX2", "ANGLE OpenGL enabled: ${egl.absolutePath}")
                    angleEmit("enabled $ctx egl=${egl.absolutePath}")
                } else {
                    runCatching { android.system.Os.unsetenv("ARMSX2_ANGLE_EGL_LIBRARY") }
                    runCatching { android.system.Os.unsetenv("ARMSX2_ANGLE_GLES_LIBRARY") }
                    // Distinguish "user never picked ANGLE" from "user picked ANGLE but the
                    // bundled .so isn't in the APK". The latter silently fell back to the system
                    // GLES driver, which reads to the user as "ANGLE is broken" — that silence
                    // is exactly why the 2.6.3 ANGLE report couldn't be diagnosed from a log.
                    if (eligible) {
                        android.util.Log.e("ARMSX2", "ANGLE selected but libs missing in $libDir")
                        angleEmit("MISSING_LIBS $ctx dir=$libDir egl=${egl.exists()} gles=${gles.exists()} -> fell back to system GLES")
                    } else {
                        angleEmit("off $ctx")
                    }
                }
            } catch (e: Exception) {
                android.util.Log.e("ARMSX2", "applyAngleEnv failed: ${e.message}")
                angleEmit("error $ctx ${e.javaClass.simpleName}: ${e.message}")
            }
        }

        /** Mirrors the @@JOYCON@@ diagnostic: logcat AND emulog, so a tester's emulog file
         *  alone answers "did ANGLE actually load?" without needing a live adb logcat. */
        private fun angleEmit(msg: String) {
            android.util.Log.i("ARMSX2", "@@ANGLE@@ $msg")
            runCatching { NativeApp.emulog("@@ANGLE@@ $msg") }
        }

        // Armed per-launch in launchGame when "Auto-load last state on boot" is on;
        // consumed once by onVmRunning. Set in launchGame (NOT start) so a manual
        // Reset Game — which re-enters start() directly — never re-loads the state.
        @Volatile
        var pendingAutoLoadOnBoot = false

        @Volatile
        private var pendingSlotLoadOnBoot: Int? = null

        // The last game we booted, retained across exit-to-library so the Save Manager can
        // re-launch + load a save AFTER the game was exited. Kept SEPARATE from currentGame
        // (which stop() nulls for settings-scope) so it can't resurrect per-game scope in the
        // library. GitHub #374 — "exit, press Load → nothing boots" because currentGame was null.
        val contextGame = mutableStateOf<GameInfo?>(null)

        fun launchCurrentGameFromSaveSlot(slot: Int): Boolean {
            val game = currentGame.value ?: contextGame.value ?: return false
            val launchPath = if (game.uri.scheme == "file") {
                game.uri.path ?: game.uri.toString()
            } else {
                game.uri.toString()
            }
            if (launchPath.isBlank()) return false
            pendingSlotLoadOnBoot = slot
            pendingAutoLoadOnBoot = false
            launchGame(launchPath, game)
            return true
        }

        /**
         * Give an externally-launched game the same identity a library-launched one has.
         *
         * A front-end (Daijisho / Lisi / Cocoon) hands us a bare content:// with no
         * GameInfo, so [handleExternalLaunchIntent] leaves [currentGame] null. That split
         * the game's identity in two: the settings hub keys its Global/Game switch off
         * currentGame, so the switch VANISHED — while the save path resolves the serial
         * from the running core and happily wrote per-game. Hence the report of settings
         * "showing as Global but saving as Per Game" only when launched from a front-end.
         *
         * The core knows the serial once the disc is read, which is the same key the save
         * path uses — so build the missing GameInfo from it and the two agree again.
         */
        /** Minutes between interval autosaves. 0 = off, which is the default: writing a
         *  savestate costs a visible hitch, so it is never turned on for you. */
        const val KEY_AUTOSAVE_INTERVAL_MIN = "autoSaveIntervalMin"

        /** How often the job below wakes to check. Well under the shortest interval (1
         *  minute), so a freshly-lowered setting takes effect promptly without the job
         *  spinning. */
        private const val AUTOSAVE_POLL_MS = 15_000L

        private var autosaveIntervalJob: kotlinx.coroutines.Job? = null

        /**
         * Interval autosave: while a game is actually RUNNING, write the autosave slot
         * every N minutes so a crash or a flat battery costs at most that much progress.
         *
         * Writes the SAME dedicated `.autosave.p2s` that auto-save-on-exit uses, so the
         * numbered slots 0-9 stay entirely the user's and auto-load-on-boot picks this up
         * with no extra plumbing.
         *
         * Started once and self-gating rather than hooked to VM start/stop: the state it
         * cares about (running, not covered by a menu, interval > 0) is all readable here,
         * and a single long-lived job can't be leaked by a boot path that forgets to stop
         * it. It deliberately does NOT fire while paused or while the pause menu / a
         * manager screen is up — the game isn't advancing, so a save then costs a hitch
         * and buys nothing.
         */
        private fun startAutosaveIntervalJob() {
            autosaveIntervalJob?.cancel()
            autosaveIntervalJob = instance?.lifecycleScope?.launch {
                var lastSaveAt = 0L
                while (true) {
                    kotlinx.coroutines.delay(AUTOSAVE_POLL_MS)
                    val minutes = runCatching { prefs.getInt(KEY_AUTOSAVE_INTERVAL_MIN, 0) }.getOrDefault(0)
                    if (minutes <= 0 || eState.value != EmuState.RUNNING || WindowImpl.frontendCovers) {
                        // Reset the clock while it can't fire, so re-entering a game doesn't
                        // immediately dump a save from time that accrued in a menu.
                        lastSaveAt = 0L
                        continue
                    }
                    val now = android.os.SystemClock.elapsedRealtime()
                    if (lastSaveAt == 0L) {
                        lastSaveAt = now
                        continue
                    }
                    if (now - lastSaveAt < minutes * 60_000L) continue
                    runCatching { NativeApp.saveAutosaveState() }
                    // Stamped AFTER the write: a savestate takes real time, and starting
                    // the next interval from before it would make saves creep earlier.
                    lastSaveAt = android.os.SystemClock.elapsedRealtime()
                }
            }
        }

        private fun adoptExternalGameIdentity() {
            if (!launchedExternally || currentGame.value != null) return
            val path = m_szGamefile.takeIf { it.isNotEmpty() } ?: return
            val handler = android.os.Handler(android.os.Looper.getMainLooper())
            handler.post(object : Runnable {
                var attempts = 0
                override fun run() {
                    // A library launch that lands mid-poll wins: it has the real entry.
                    if (vmStopInProgress || eState.value == EmuState.STOPPED) return
                    if (currentGame.value != null) return
                    // "00000000" is the placeholder the core reports before the disc is
                    // read — the same value TouchControls.coreSerial() rejects.
                    val serial = runCatching { NativeApp.getGameSerial() }.getOrNull()
                        ?.trim()?.uppercase()?.takeIf { it.isNotEmpty() && it != "00000000" }
                    if (serial == null) {
                        // ~10s of looking. A serial-less boot (ELF/homebrew) just never
                        // adopts one, and settingsKey's filename fallback still applies.
                        if (++attempts < 40) handler.postDelayed(this, 250)
                        return
                    }
                    val uri = runCatching { Uri.parse(path) }.getOrNull() ?: return
                    val name = uri.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':')
                        ?: path.substringAfterLast('/')
                    val (title, _) = FilenameParser.parse(name)
                    currentGame.value = GameInfo(
                        uri = uri,
                        title = title,
                        serial = serial,
                        extension = name.substringAfterLast('.', "").uppercase(),
                    )
                }
            })
        }

        /** Fired when the VM reaches RUNNING (from NativeApp.vmSetPaused). If the
         *  user enabled auto-load-on-boot, restore the autosave state once — but only
         *  after the renderer is actually presenting frames (polls getPresentedFrameCount),
         *  because restoring before the present loop is flowing leaves a black screen.
         *  Polls every 250ms, giving up after ~15s if the game never starts presenting. */
        @JvmStatic
        fun onVmRunning() {
            adoptExternalGameIdentity()
            val requestedSlot = pendingSlotLoadOnBoot
            val loadAutosave = pendingAutoLoadOnBoot && requestedSlot == null
            if (requestedSlot == null && !loadAutosave) return
            pendingSlotLoadOnBoot = null
            pendingAutoLoadOnBoot = false
            val handler = android.os.Handler(android.os.Looper.getMainLooper())
            val tryLoad = object : Runnable {
                var attempts = 0
                var lastFrame = -1
                var advancingPolls = 0
                override fun run() {
                    if (vmStopInProgress || eState.value == EmuState.STOPPED) return
                    // Wait until the renderer is actually PRESENTING frames before restoring the
                    // state. A boot-time load that fires as soon as the disc CRC is known — before
                    // the present loop is flowing — leaves the restored frame undisplayed (a black
                    // screen); loading the same state manually works only because the game is
                    // already rendering by then. The present counter can read stale-high across a
                    // re-launch (the GS may not fully reset between games), so gate on SUSTAINED
                    // advancement rather than an absolute value: require frames to have grown
                    // across a few consecutive polls (~0.75s of continuous presenting). (Native
                    // then forces one present of the restored frame so it shows immediately.)
                    val frame = runCatching { NativeApp.getPresentedFrameCount() }.getOrDefault(0)
                    advancingPolls = if (lastFrame in 0 until frame) advancingPolls + 1 else 0
                    lastFrame = frame
                    if (advancingPolls < 3) {
                        if (++attempts < 60) handler.postDelayed(this, 250)
                        return
                    }
                    val loaded = runCatching {
                        if (requestedSlot != null) NativeApp.loadStateFromSlot(requestedSlot)
                        else NativeApp.loadAutosaveState()
                    }.getOrDefault(false)
                    if (!loaded && ++attempts < 60)
                        handler.postDelayed(this, 250)
                }
            }
            handler.postDelayed(tryLoad, 250)
        }

        fun finishSetup() {
            prefs.edit { putBoolean("setupComplete", true) }
            setupComplete.value = true
            setupEditorVisible.value = false
        }

        fun reopenSetup() {
            setupEditorVisible.value = true
        }

        /** The data root that NativeApp.initialize() was actually handed
         *  (captured in kickoffEmucoreInit). EmuFolders::DataRoot is pinned
         *  ONCE per process, so the setup wizard compares against this to know
         *  whether a storage-location change actually needs a process restart
         *  to take effect (it can't be hot-swapped while the process lives). */
        private var lastInitDataRoot: String? = null
        fun currentInitDataRoot(): String? = lastInitDataRoot

        /**
         * Factory-reset every app SETTING and cold-restart.
         *
         * Wipes all preferences (settings, controls, hotkeys, touch layouts, per-game overrides,
         * library cache, recents, onboarding state) plus the on-disk settings layers that would
         * otherwise re-seed them — see [ConfigStore.purgeAllSettingsFiles], which is what stops
         * the reset being silently undone on the next launch.
         *
         * Deliberately does NOT delete content: games, BIOS, saves, memory cards, save states,
         * covers, texture packs and shaders all survive. Setup runs again afterwards because the
         * chosen data root is a preference; pointing it at the same folder restores everything.
         */
        fun resetAppToDefaults(context: Context) {
            // Files first — clearing prefs drops the data-root pref that locates them.
            runCatching { com.armsx2.config.ConfigStore.purgeAllSettingsFiles() }
            runCatching { prefs.edit { clear() } }
            restartApp(context)
        }

        /** Cold-restart the app so native re-runs initialize() with the newly
         *  chosen data root. Used after the user moves app data between Internal
         *  and SD in the setup wizard. */
        fun restartApp(context: Context) {
            val intent = context.packageManager.getLaunchIntentForPackage(context.packageName)
            if (intent != null) {
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
                context.startActivity(intent)
            }
            Runtime.getRuntime().exit(0)
        }

        fun renderOpenGL() {
            NativeApp.renderOpenGL()
        }

        fun renderVulkan() {
            NativeApp.renderVulkan()
        }

        fun renderSoftware() {
            NativeApp.renderSoftware()
        }

        /** Resolved root that bundled APK assets (resources/, bios/) are
         *  copied to. This prefers a custom systemDir only when it resolves to
         *  a writable POSIX path; otherwise it falls back to app-private
         *  storage. Game folders are separate and accessed through SAF. */
        /** True if at least one configured ROMs folder is actually reachable right
         *  now. content:// folders need a live persisted SAF read permission;
         *  file:// (all-files build) needs the path readable. Android Auto Backup
         *  restores the folder URIs but NOT their permissions, so after a reinstall
         *  or device-restore the saved folders can be present yet unreadable — this
         *  is how launch detects that and re-runs setup instead of stranding the
         *  user in an empty, perpetually-scanning library. */
        fun romsAccessible(context: Context, romsDirs: List<String>): Boolean {
            if (romsDirs.isEmpty()) return false
            // content://: still hold the EXACT persisted SAF read grant (string-prefix
            // matching is unsafe — "…ROMs" prefixes "…ROMs2"). The all-files build can
            // ALSO reach a content:// folder by resolving it to a POSIX path under
            // MANAGE_EXTERNAL_STORAGE, so honor that too (checking the grant itself, not
            // raw canRead, so a temporarily-unmounted SD isn't misread as lost access).
            val persisted = runCatching { context.contentResolver.persistedUriPermissions }
                .getOrDefault(emptyList())
                .filter { it.isReadPermission }
                .map { it.uri.toString() }
                .toHashSet()
            val allFiles = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
                android.os.Environment.isExternalStorageManager()
            // R+ scoped storage: a /storage path is only TRULY readable with all-files.
            // File.canRead() FALSE-POSITIVES there — it returns true for a path whose
            // contents scoped storage then refuses to list — which silently defeated this
            // whole check after an Auto Backup restore (folder URI restored, permission not,
            // yet canRead said "fine" → no recovery → empty library). So on R+ trust the
            // all-files grant and never canRead; canRead is only meaningful on pre-R legacy.
            fun posixReadable(path: String?): Boolean {
                if (path == null) return false
                if (allFiles) return true
                return Build.VERSION.SDK_INT < Build.VERSION_CODES.R &&
                    runCatching { File(path).canRead() }.getOrDefault(false)
            }
            return romsDirs.any { raw ->
                when {
                    raw.startsWith("content:") ->
                        raw in persisted || (allFiles && resolveTreeUriToPosix(raw) != null)
                    raw.startsWith("file:") -> posixReadable(raw.toUri().path)
                    else -> posixReadable(raw)
                }
            }
        }

        fun assetCopyRoot(context: Context): String {
            val custom = systemDirPosix()
            return custom?.takeIf { validateSystemDirWritable(it) }
                ?: context.getExternalFilesDir(null)?.absolutePath
                ?: context.dataDir.absolutePath
        }

        fun copyAssetAll(p_context: Context, srcPath: String) {
            copyAssetAll(p_context, srcPath, assetCopyRoot(p_context))
        }

        private fun copyAssetAll(p_context: Context, srcPath: String, rootDir: String) {
            val assetMgr = p_context.assets
            try {
                val destPath = rootDir + File.separator + srcPath
                assetMgr.list(srcPath)?.let {
                    if (it.isEmpty()) {
                        MainActivity.copyFile(p_context, srcPath, destPath)
                    } else {
                        val dir = File(destPath)
                        if (!dir.exists()) dir.mkdirs()
                        for (element in it) {
                            copyAssetAll(p_context, srcPath + File.separator + element, rootDir)
                        }
                    }
                }
            } catch (e: IOException) {
                android.util.Log.e("ARMSX2", "copyAssetAll failed: $srcPath -> $rootDir: ${e.message}")
            }
        }

        private fun sameFilePath(a: File, b: File): Boolean {
            val ca = runCatching { a.canonicalFile }.getOrDefault(a.absoluteFile)
            val cb = runCatching { b.canonicalFile }.getOrDefault(b.absoluteFile)
            return ca == cb
        }

        private fun copyFileViaTemp(src: File, target: File): Boolean {
            if (sameFilePath(src, target))
                return target.exists() && target.length() > 0L
            if (!src.exists() || src.length() <= 0L)
                return false

            val parent = target.parentFile ?: return false
            if (!parent.exists() && !parent.mkdirs())
                return false

            val tmp = File(parent, ".${target.name}.migrate.tmp")
            if (tmp.exists())
                tmp.delete()

            return runCatching {
                src.copyTo(tmp, overwrite = true)
                if (tmp.length() <= 0L)
                    return@runCatching false
                if (target.exists() && !target.delete())
                    return@runCatching false
                val installed = tmp.renameTo(target) || runCatching {
                    tmp.copyTo(target, overwrite = true)
                    true
                }.getOrDefault(false)
                installed && target.exists() && target.length() > 0L
            }.getOrDefault(false).also {
                tmp.delete()
            }
        }

        fun getSupportedGLESVersion(context: Context): Double {
            val am = context.getSystemService(ACTIVITY_SERVICE) as ActivityManager
            val info = am.deviceConfigurationInfo
            return info.glEsVersion.toDouble()
        }

        fun isAndroidEmulator(): Boolean {
            return Build.MODEL.startsWith("sdk_")
        }
    }

    val swapDiscAction = registerForActivityResult(
        StartActivityForResult()
    ) { result: ActivityResult ->
        if (result.resultCode == RESULT_OK) {
            try {
                val intent = result.data
                val uri = intent?.dataString ?: ""
                if (uri.isNotEmpty()) {
                    // Swap the mounted disc instead of rebooting. The old path
                    // (restart()) booted the picked disc as a fresh VM, which
                    // dropped CodeBreaker/multi-disc hand-offs and never showed
                    // a "disc changed" notification. NativeApp.changeDisc keeps
                    // the running VM, cycles the tray so the game detects the
                    // new disc, and emits the on-screen "Disc changed to …" OSD.
                    // Runs off-thread since it parks the CPU thread and blocks.
                    println("@@ANDROID_SWAP_DISC@@ uri=${uri.take(240)}")
                    kotlin.concurrent.thread {
                        val ok = runCatching { NativeApp.changeDisc(uri) }.getOrDefault(false)
                        instance?.runOnUiThread {
                            if (ok) {
                                // changeDisc parks the VM to swap on the CPU
                                // thread; unpause so the game runs and detects
                                // the new disc (otherwise the screen sits frozen
                                // on the paused frame).
                                resume()
                            } else {
                                // Swap Disc is swap-only. If native rejected
                                // the image it already restored the old disc,
                                // so just resume the existing session.
                                resume()
                            }
                        }
                    }
                }
            } catch (_: Exception) { }
        }
    }

    val bootDiscAction = registerForActivityResult(
        StartActivityForResult()
    ) { result: ActivityResult ->
        if (result.resultCode == RESULT_OK) {
            try {
                val uri = result.data?.dataString ?: ""
                if (uri.isNotEmpty()) {
                    println("@@ANDROID_BOOT_DISC@@ uri=${uri.take(240)}")
                    launchGame(uri, null)
                }
            } catch (_: Exception) { }
        }
    }

    init {
        instance = this
    }

    /** Latched on first kickoffEmucoreInit so a second call (e.g. after
     *  the user re-enters setup via the cog) is a no-op. Heavy init —
     *  asset copy, EmuFolders setup, JIT test prelude — must run once
     *  per process. */
    private var emucoreInitDone = false

    /** Latch for the debug-build auto-boot-to-BIOS path. Fires once per
     *  process from kickoffEmucoreInit's tail so JIT tests finish first,
     *  then runs startBios() with no game disc. Used for perfape baseline
     *  captures without manually tapping the BIOS card. */
    private var autoBootBiosFired = false

    /** Build-config flag for the auto-boot-to-BIOS path above. Flip to
     *  true (here, or move to BuildConfig via app/build.gradle.kts if a
     *  variant-level toggle is wanted) to drop straight into the BIOS
     *  shell on app launch — useful for perfape captures. */
    private val AUTO_BOOT_BIOS = false

    /**
     * Run the heavy one-shot emucore init (asset copy + EmuFolders +
     * SDL/HID setup + EE/VIF JIT-test prelude). MUST be called only
     * AFTER the user has finished the setup wizard so `MainActivityRuntime.systemDir`
     * resolves to the chosen path before `NativeApp.initializeOnce`
     * locks `EmuFolders::AppRoot` in for the rest of the process.
     *
     * Idempotent — guarded by emucoreInitDone. Safe to call from both
     * onCreate (returning user, setupComplete already true) and the
     * setContent LaunchedEffect (first-time user, setupComplete just
     * flipped).
     */
    private fun kickoffEmucoreInit() {
        if (emucoreInitDone) return
        emucoreInitDone = true
        // Record the root native is about to pin (same resolution as
        // NativeApp.initializeOnce's dataPath) so a later storage change can be
        // detected and trigger a restart instead of silently not taking effect.
        lastInitDataRoot = assetCopyRoot(applicationContext)

        // #9: one-time recovery for a fresh install that reuses an old data folder — restore
        // settings from the in-folder mirror, or seed from the folder's old PCSX2-Android.ini,
        // BEFORE the core loads/rewrites it. No-op (guarded) for anyone already on the new UI.
        runCatching { com.armsx2.config.ConfigStore.reconcileReusedFolder() }
        // A genuinely empty install gets capability-aware frame-queue defaults only
        // after reused-folder recovery had its chance. Capable handhelds start in
        // low-latency mode (queue 0); low-end devices retain the smoother queue 2.
        runCatching { com.armsx2.config.ConfigStore.seedFreshInstallDefaults(applicationContext) }
        // One-time: existing capable devices also get the Low Latency default (matches fresh installs).
        runCatching { com.armsx2.config.ConfigStore.migrateLowLatencyOff(applicationContext) }
        // Steer the renderer's Auto resolution. Vulkan HW on Adreno (tile-memory framebuffer-fetch
        // fast path) and on any device whose GL driver cannot read the render target in-tile, where
        // OpenGL degrades to a tile flush per self-referential draw; a healthy Mali stays on
        // OpenGL, which is its fast path. The verdict is computed natively because it consults the
        // driver-bug database, so all we do here is hand over the probed GL strings. Sets a native
        // flag GSUtil::GetPreferredRenderer reads before the GS starts, so an explicit GL/SW pick
        // still wins. Re-asserted each launch.
        runCatching {
            val gl = com.armsx2.GpuInfo.glStrings()
            kr.co.iefriends.pcsx2.NativeApp.setAutoRendererGpuStrings(gl.vendor, gl.renderer, gl.version)
        }

        // Default resources — shaders, GameIndex, fonts, fullscreenui,
        // patches.zip, controller DB. assetCopyRoot resolves to the
        // user's chosen systemDir (now valid post-setup) so emucore
        // finds them at <systemDir>/resources/...
        copyAssetAll(applicationContext, "bios")
        copyAssetAll(applicationContext, "resources")
        // Fork-owned clean-room shader presets. Kept outside `resources/` because the existing
        // shader picker scans <dataRoot>/shaders recursively. Copying is idempotent and preserves
        // the relative .slangp -> .slang paths required by librashader.
        copyAssetAll(applicationContext, "shaders")

        // On an app UPDATE (versionCode changed), drop the regenerable GPU caches. Installing a
        // new build over an old one keeps the compiled GS shader/pipeline cache under
        // <dataRoot>/cache, and a cache baked by a different core build can render corrupt — the
        // "scrambled PS2 logo" and post-update graphical glitches users currently fix by
        // reinstalling clean (#376/#385). The cache is pure derived data (rebuilt on demand),
        // never user content, so wiping it is always safe. Skipped on first install (no prior
        // version recorded) — there is nothing stale to clear.
        runCatching {
            val prevVc = prefs.getInt("lastRunVersionCode", 0)
            val curVc = BuildConfig.VERSION_CODE
            if (prevVc != 0 && prevVc != curVc) {
                File(assetCopyRoot(applicationContext), "cache").deleteRecursively()
                android.util.Log.i("ARMSX2", "Update $prevVc -> $curVc: cleared GS shader/pipeline cache")
            }
            if (prevVc != curVc) prefs.edit { putInt("lastRunVersionCode", curVc) }
        }

        // Point the ANGLE EGL env vars at the bundled libs (or clear them) before the
        // GS thread ever opens a GL context. Re-applied per launch below too.
        applyAngleEnv(applicationContext)

        // Keep the configured BIOS in app-private internal storage (NOT under a
        // custom/SD data root). The native core can't reliably open a BIOS off a
        // removable/SAF volume on Android 11+, so a data-root-on-SD setup failed VM
        // init and bounced back to the library. This also MIGRATES any BIOS an older
        // build moved onto the SD data root back to internal. No-op when no BIOS is
        // set or it's already internal; on copy failure we leave the pref untouched
        // so biosFolderPosix still points emucore at the old (working) location.
        bios.value?.takeIf { it.isNotEmpty() }?.let { current ->
            val src = File(current)
            val target = File(internalBiosDir(applicationContext).apply { mkdirs() }, src.name)
            if (!sameFilePath(target, src)) {
                val present = (target.exists() && target.length() > 0L) ||
                    copyFileViaTemp(src, target)
                if (present) {
                    bios.value = target.absolutePath
                    prefs.edit { putString("bios", target.absolutePath) }
                }
            } else if (target.exists() && target.length() > 0L) {
                bios.value = target.absolutePath
                prefs.edit { putString("bios", target.absolutePath) }
            }
        }

        // (BIOS data-root mirror runs in the background invoke{} block below — it's
        // cosmetic and must not block first paint / risk an ANR on slow SD cards.)

        invoke {
            NativeApp.initializeOnce(applicationContext)
            nativeReady.value = true

            // One-time repair of globally-armed patches. Older builds filled the global
            // [Patches]/[Cheats] Enable lists just by opening the Patch Manager, and since
            // patches are matched BY NAME those entries armed the same-named group in the
            // bundled archive for every game — the "60fps/16:9 with every patch setting off,
            // and it won't turn off" reports. The auto-sync is gone, but existing installs
            // still carry the poisoned lists, so clear them once. Must run after
            // initializeOnce (the base settings layer has to exist).
            // Key is versioned: v1 cleared only the base layer, which a stale PER-GAME list then
            // shadowed (GOW2 still reported "1 game patch active" with everything off). Bumping it
            // re-runs the now-complete purge for anyone who already took v1.
            // Lightgun: read the pref and re-assert the USB device type. The ini is
            // authoritative, but this covers a first run that has no USB section yet.
            runCatching {
                com.armsx2.input.UsbDevices.load()
                com.armsx2.input.Lightgun.load()
                com.armsx2.input.UsbDevices.applyAtBoot()
            }

            if (!prefs.getBoolean("patchEnableListsPurged.v2", false)) {
 …21631 tokens truncated…      return true
                }
                ControllerMappings.SysHotkey.CLOSE_GAME -> {
                    if (down) closeGame()
                    return true
                }
                ControllerMappings.SysHotkey.QUIT_APP -> {
                    // Stop the VM (flushes memcards/savestate), then finish the app once
                    // the VM has fully unwound — never finish inline (stop() is async).
                    if (down) { quitAfterStop = true; stop()
                    }
                    return true
                }
                ControllerMappings.SysHotkey.SAVE_AND_EXIT -> {
                    // Write an autosave state, THEN close the game — the frontend "exit"
                    // case (Cocoon/ES-DE) that returns to the launcher without losing
                    // progress. closeGame() applies the exit-to-launcher opt-in.
                    if (down) closeGame(saveAutosave = true)
                    return true
                }
                ControllerMappings.SysHotkey.RESET_GAME -> {
                    if (down) restart()
                    return true
                }
                null -> {}
            }
        }
        // Gameplay buttons take the shortest path after every higher-priority
        // frontend/capture/hotkey owner has declined them. This avoids routing
        // ABXY through View -> Compose -> onKeyEvent before the JNI pad write.
        if (dispatchGameplayKey(event)) return true
        return super.dispatchKeyEvent(event)
    }

    /** Route one gameplay key edge directly from Activity dispatch to the native pad. */
    private fun dispatchGameplayKey(event: KeyEvent): Boolean {
        if (eState.value != EmuState.RUNNING || controllerDrivesFrontend()) return false
        val type = when (event.action) {
            KeyEvent.ACTION_DOWN -> KeyEventType.KeyDown
            KeyEvent.ACTION_UP -> KeyEventType.KeyUp
            else -> return false
        }
        val physicalCode = event.keyCode
        if (physicalCode == KeyEvent.KEYCODE_UNKNOWN) return false

        // Local co-op routing and macro precedence exactly match the old Compose
        // onKeyEvent path; only the dispatch layer has changed.
        val port = com.armsx2.input.PadRouter.portForDevice(event.deviceId)
        com.armsx2.ui.touch.TouchControls.macroForPhysicalCode(physicalCode)?.let { macro ->
            com.armsx2.ui.touch.TouchControls.fireMacro(
                macro, "pad$port", type == KeyEventType.KeyDown,
            ) { code, pressed ->
                sendKeyAction(
                    if (pressed) KeyEventType.KeyDown else KeyEventType.KeyUp,
                    code, port,
                )
            }
            return true
        }

        val target = ControllerMappings.targetForPhysical(physicalCode, port) ?: return false
        if (ControllerMappings.isTurboTarget(target, port)) {
            handleTurbo(physicalCode, type, target, port)
        } else {
            sendKeyAction(type, target, port)
        }
        return true
    }

    /** #254: forward a hardware keyboard KeyEvent to the emulated USB keyboard.
     *  Returns true (event consumed) only when the game runs with the USB
     *  keyboard attached, the event comes from a real keyboard, no menu/overlay
     *  or binding capture is active, and the native side accepted the key (i.e.
     *  it mapped to a HID usage). Otherwise returns false so the event keeps
     *  flowing to the normal pad / hotkey / nav handling. */
    private fun forwardKeyToUsbKeyboard(event: KeyEvent, kc: Int): Boolean {
        if (!usbKeyboardActive) return false
        if (eState.value != EmuState.RUNNING) return false
        // Don't steal keys the frontend/menus need for navigation, or while
        // (re)binding a pad button / hotkey.
        if (controllerDrivesFrontend()) return false
        if (ControllerMappings.padCapturing.value ||
            ControllerMappings.captureHotkey.value != null) return false
        // Must be a real keyboard key. SOURCE_KEYBOARD is set for hardware/BT
        // keyboards; gamepad buttons (SOURCE_GAMEPAD) share some keyCodes (the
        // D-pad arrows) so require the keyboard source and reject anything that
        // also claims to be a gamepad/joystick, keeping pad input on its own path.
        if (!event.isFromSource(InputDevice.SOURCE_KEYBOARD)) return false
        if (event.isFromSource(InputDevice.SOURCE_GAMEPAD) ||
            event.isFromSource(InputDevice.SOURCE_JOYSTICK)) return false
        if (kc == KeyEvent.KEYCODE_UNKNOWN) return false
        // Never divert the system Back/Home keys into the emulated keyboard —
        // the user still needs Back to open the overlay / leave the game.
        if (kc == KeyEvent.KEYCODE_BACK || kc == KeyEvent.KEYCODE_HOME) return false
        val pressed = when (event.action) {
            KeyEvent.ACTION_DOWN -> true
            KeyEvent.ACTION_UP -> false
            else -> return false // MULTIPLE etc. — ignore
        }
        return runCatching {
            NativeApp.usbKeyboardKey(0, kc, pressed)
        }.getOrDefault(false)
    }

    /** Cycle the active quick save/load slot 0→9→0 with a brief on-screen note. */
    /** The limiter mode that fast-forward should fall back to when it ends:
     *  Nominal (0) when the frame limiter is on, Unlimited (3) when the user has
     *  turned it off. Mirrors the frame-limit toggle so the two stay in sync. */
    private fun baseLimiterMode(): Int =
        if (InGameOverlay.frameLimitOn.value) 0 else 3

    /** Toggle locked fast-forward (Turbo) on/off — shared by the FAST_FORWARD_TOGGLE
     *  hotkey and the on-screen fast-forward touch button (FastForwardWidget). Restores
     *  the user's base limiter mode when turning off so it stays in sync with the
     *  frame-limit toggle. */
    /** Flip the runtime gyro enable (issue #337). Shared by the GYRO_TOGGLE hotkey and the
     *  edge-triggered (stick/combo) path. Only silences the sensor for this session — the
     *  user's Gyro Mode setting is untouched, so re-enabling restores their configured mode. */
    private fun toggleGyro() {
        val on = !gyroActive.value
        gyroActive.value = on
        hotkeyToast(if (on) "Gyro ON" else "Gyro OFF")
    }

    /** Re-zero the motion neutral. Routed through [gyroRecenterHook] because the sensor
     *  instance is owned by the touch overlay composable, not the runtime. No-op (with a
     *  toast either way) when no gyro session is live, so the binding never feels dead. */
    private fun recenterGyro() {
        val hook = gyroRecenterHook
        if (hook == null) {
            hotkeyToast("Motion not active")
            return
        }
        hook()
        hotkeyToast("Motion recentered")
    }

    fun toggleFastForward() {
        fastForwardToggleActive = !fastForwardToggleActive
        val on = fastForwardToggleActive
        // Fast-forward supersedes an active slow-down latch (mutually exclusive).
        if (on) slowDownToggleActive = false
        runCatching { NativeApp.speedhackLimitermode(if (on) ffLimiterMode() else baseLimiterMode()) }
        hotkeyToast(if (on) "Fast Forward ON" else "Fast Forward OFF")
    }

    /** Toggle slow motion (native LimiterModeType::Slomo, ~50% speed). BLOCKED in
     *  RetroAchievements hardcore — slow-mo is a banned advantage there (matching
     *  desktop PCSX2's hardcore restrictions); shows a notice instead of engaging. */
    fun toggleSlowDown() {
        if (InGameOverlay.hardcoreOn.value) {
            slowDownToggleActive = false
            hotkeyToast("Slow Down is disabled in RetroAchievements Hardcore mode")
            return
        }
        slowDownToggleActive = !slowDownToggleActive
        val on = slowDownToggleActive
        // Slow-down supersedes an active fast-forward latch (mutually exclusive).
        if (on) fastForwardToggleActive = false
        runCatching { NativeApp.speedhackLimitermode(if (on) 2 else baseLimiterMode()) }
        hotkeyToast(if (on) "Slow Down ON (50%)" else "Slow Down OFF")
    }

    // Hotkey pop-up toasts (Fast-Forward, etc.). Android Toasts QUEUE, so toggling a
    // hotkey rapidly stacks a long backlog that blocks the screen — cancel the previous
    // one before showing the next so only the latest shows. Honors "ui.hotkeyToasts"
    // (default on) so they can be silenced entirely.
    private var lastHotkeyToast: android.widget.Toast? = null
    private fun hotkeyToast(text: String) {
        if (!prefs.getBoolean("ui.hotkeyToasts", true)) return
        lastHotkeyToast?.cancel()
        lastHotkeyToast = android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_SHORT)
            .also { it.show() }
    }

    /** Quick save / load to the active slot — shared by the SAVE_STATE/LOAD_STATE
     *  hotkeys and the on-screen Save/Load State touch buttons. Runs off the UI thread. */
    /**
     * RetroAchievements hardcore forbids save states — enforced HERE so every entry point is
     * covered at once.
     *
     * The slot picker checked it, but these direct quick-save/load helpers did not, so anything
     * bypassing the picker (the second-display panel, the on-screen state buttons, the hotkeys)
     * could still load a state in hardcore — precisely the cheat the mode exists to prevent.
     */
    private fun blockedByHardcore(): Boolean {
        val hardcore = runCatching { NativeApp.isHardcoreMode() }.getOrDefault(false)
        if (hardcore) {
            runOnUiThread {
                com.armsx2.ui.WelcomeBanner.show(com.armsx2.i18n.I18n.get("savestate.error.hardcore"))
            }
        }
        return hardcore
    }

    fun saveState() {
        if (blockedByHardcore()) return
        val slot = currentSaveSlot.value
        kotlin.concurrent.thread { runCatching { NativeApp.saveStateToSlot(slot) } }
    }

    fun loadState(onLoaded: (() -> Unit)? = null) {
        if (blockedByHardcore()) return
        val slot = currentSaveSlot.value
        kotlin.concurrent.thread {
            runCatching { NativeApp.loadStateFromSlot(slot) }
            // Resume/dismiss only AFTER the load lands. The caller used to resume
            // immediately, which raced the async load (the menu resumed the VM before
            // the state was restored) — that's why "Load" appeared to do nothing.
            onLoaded?.let { cb -> android.os.Handler(android.os.Looper.getMainLooper()).post(cb) }
        }
    }

    private fun cycleSaveSlot() {
        val next = (currentSaveSlot.value + 1) % 10
        currentSaveSlot.value = next
        android.widget.Toast.makeText(this, "Save slot $next", android.widget.Toast.LENGTH_SHORT).show()
    }

    /** Step the internal resolution multiplier up/down (1x..5x), apply live, and
     *  persist to the running game's tier — per-game when it has a serial — so it
     *  survives a reboot without bleeding into other titles. */
    private fun stepResolution(dir: Int) {
        val next = (upscale.value.toInt() + dir).coerceIn(1, 8)
        val nf = next.toFloat()
        upscale.value = nf
        runCatching { NativeApp.renderUpscalemultiplier(nf) }
        runCatching {
            val serial = currentGame.value?.serial?.takeIf { it.isNotBlank() }
            val resolved = com.armsx2.config.ConfigStore.resolveForGame(serial)
            com.armsx2.config.ConfigStore.save(
                if (serial != null) com.armsx2.config.SettingsScope.Game
                else com.armsx2.config.SettingsScope.Global,
                serial,
                resolved.copy(upscaleFloat = nf),
            )
        }
        android.widget.Toast.makeText(this, "Resolution ${next}x", android.widget.Toast.LENGTH_SHORT).show()
    }

    // Corrects the Samsung QHD on-screen-touch offset before the event is dispatched (a strict no-op
    // on every other device — see maybeCorrectTouchScale). ALWAYS returns super, so it can never
    // block or consume a tap.
    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        maybeCorrectTouchScale(ev)
        return super.dispatchTouchEvent(ev)
    }

    // Empirically-learned touch-coordinate extent (largest x/y ever delivered), plus the last window
    // size so a stale extent is dropped on resize. Drives maybeCorrectTouchScale — see there.
    private var touchPeakX = 0f
    private var touchPeakY = 0f
    private var lastDecorW = 0
    private var lastDecorH = 0

    /** Un-scaled physical display size in the current rotation. Only a SEED for the touch-space
     *  estimate below, never trusted alone: Samsung's QHD game-downscale reports this DOWNSCALED too
     *  (observed 1080 at QHD+), so the observed touch extent is the ground truth. */
    @Suppress("DEPRECATION")
    private fun realPanelMetrics(): android.util.DisplayMetrics? = runCatching {
        val dm = android.util.DisplayMetrics()
        val disp = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R)
            display else windowManager.defaultDisplay
        disp?.getRealMetrics(dm)
        dm.takeIf { it.widthPixels > 0 && it.heightPixels > 0 }
    }.getOrNull()

    /**
     * Correct the Samsung QHD touch-offset bug (#Nomad, S24 Ultra @ QHD+) — self-contained, trusting
     * NO resolution API. On that device at QHD every one of them (decorView, maximumWindowMetrics,
     * getRealMetrics) reports the DOWNSCALED ~1080 while the digitizer still delivers touch in the
     * physical ~1440 space, so the on-screen controls (laid out in the ~1080 window) sit up-and-left
     * of where the finger must press, the error growing with distance (a pure ≈1.33 scale). It works
     * at FHD+ (everything is a consistent 1080) and breaks only at QHD.
     *
     * Ground truth is the touches themselves: in this broken state a press near a far control lands
     * OUTSIDE the window. That never happens on a normal device or in split-screen/multi-window (the
     * OS descales touch to fit the window there), so this is self-gating — a strict no-op except the
     * exact bug. We learn the true touch extent from where fingers actually reach (seeded by
     * getRealMetrics when it happens to read larger) and rescale pointers back into the window:
     * precise from the first far press when the seed is right, else converging within a touch or two.
     */
    private fun maybeCorrectTouchScale(ev: MotionEvent) {
        runCatching {
            val decorW = window.decorView.width
            val decorH = window.decorView.height
            if (decorW <= 0 || decorH <= 0) return
            val slop = 8f
            // Drop the learned extent when the window size changes (rotation, or Samsung's game-mode
            // resolution switch), so a stale peak from a previous mode can't mis-scale the new one.
            if (decorW != lastDecorW || decorH != lastDecorH) {
                touchPeakX = 0f; touchPeakY = 0f
                lastDecorW = decorW; lastDecorH = decorH
            }
            // Grow the observed extent from THIS event's pointers (raw, before any correction),
            // capped at 2x the window so one spurious out-of-range sample can't over-shrink touch.
            val capX = decorW * 2f
            val capY = decorH * 2f
            for (i in 0 until ev.pointerCount) {
                if (ev.getX(i) > touchPeakX) touchPeakX = minOf(ev.getX(i), capX)
                if (ev.getY(i) > touchPeakY) touchPeakY = minOf(ev.getY(i), capY)
            }
            // Engage ONLY once a touch has escaped the window (proof the touch space exceeds the
            // layout space). True extent = the larger of the observed peak and a physical-panel
            // reading that ALSO exceeds the window; scale the window back onto it (clamped so a stray
            // reading can't invert the axis or shrink past 2x).
            val real = realPanelMetrics()
            val spaceW = maxOf(touchPeakX, (real?.widthPixels ?: 0).let { if (it > decorW) it.toFloat() else 0f })
            val spaceH = maxOf(touchPeakY, (real?.heightPixels ?: 0).let { if (it > decorH) it.toFloat() else 0f })
            val sx = if (touchPeakX > decorW + slop) (decorW / spaceW).coerceIn(0.5f, 1f) else 1f
            val sy = if (touchPeakY > decorH + slop) (decorH / spaceH).coerceIn(0.5f, 1f) else 1f
            if (sx != 1f || sy != 1f) {
                ev.transform(android.graphics.Matrix().apply { setScale(sx, sy) })
            }
        }
    }

    override fun dispatchGenericMotionEvent(ev: MotionEvent): Boolean {
        // Controller-input diagnostic (ARMSX2_JOYCON): logged before ANY gate so it
        // captures the raw axes even mid-(re)bind and for SOURCE_DPAD-only events the
        // gameplay path would drop. Pure logging — no behaviour change.
        logControllerDeviceOnce(ev.deviceId)
        logControllerMotion(ev)
        // While (re)binding a pad button or a hotkey, the physical D-pad on many
        // handhelds (AYN Odin 3, RP6, etc.) arrives HERE as a HAT *axis*, never as
        // a key in dispatchKeyEvent — so the capture (which only listens for key
        // events) never saw it, and the HAT instead navigated the settings UI. When
        // a capture is armed, translate the HAT direction into a synthetic D-pad
        // KeyEvent and route it through dispatchKeyEvent (which reaches both the pad
        // capture in Compose and the hotkey capture in dispatchKeyEvent), and
        // consume the motion so nothing navigates.
        if (ControllerMappings.padCapturing.value || ControllerMappings.captureHotkey.value != null) {
            return handleCaptureMotion(ev)
        }
        captureHatX = 0
        captureHatY = 0
        if (captureHeldSynth.isNotEmpty()) {
            // Capture ended while a synthetic direction was still "held": no UP was
            // ever dispatched for it, so also purge it from heldKeys or a stale
            // direction would satisfy combo-modifier checks forever after.
            heldKeys.removeAll(captureHeldSynth)
            captureHeldSynth.clear()
        }
        if (controllerDrivesFrontend() && handleControllerUiMotion(ev)) {
            return true
        }
        if (eState.value == EmuState.RUNNING) {
            // Only true gamepad/joystick motion drives the PS2 pads. A DualSense's
            // touchpad/mouse node also emits generic motion (pointer AXIS_X/Y); reading
            // it as stick input injects garbage AND (via PadRouter) lets a non-pad node
            // grab a player slot — which pushed the real 2nd pad onto Player 1.
            if (!ev.isFromSource(InputDevice.SOURCE_JOYSTICK) &&
                !ev.isFromSource(InputDevice.SOURCE_GAMEPAD)) {
                return super.dispatchGenericMotionEvent(ev)
            }
            // SOURCE_TOUCHSCREEN motion events go through dispatchTouchEvent,
            // not here — generic motion is gamepad / mouse / stylus. So any
            // event reaching this method means a controller (or similar
            // pointing device) is being used; latch touch controls off.
            com.armsx2.ui.touch.TouchControls.onControllerInputDetected()
            // Local co-op: which PS2 port this physical device drives (P1=0 / P2=1).
            // Stick mode + CUSTOM binds are read per-player; emits route to `port`.
            val port = com.armsx2.input.PadRouter.portForDevice(ev.deviceId)
            // Analog sticks → analog (default) OR remapped to the D-pad / face
            // buttons (per ControllerMappings.{left,right}StickMode) — useful for
            // fighting games on analog-centric pads (e.g. left stick = D-pad).
            dispatchStick(ev, ControllerMappings.leftStickMode(port),
                MotionEvent.AXIS_X, MotionEvent.AXIS_Y,
                aXPos = 111, aXNeg = 113, aYPos = 112, aYNeg = 110, // L right/left, down/up
                leftStick = true, port = port)
            // Right-stick axes resolved per-device (Joy-Con RX/RY; RZ-as-trigger pads → RX/RY) — was
            // hard-coded Z/RZ, which read an RZ trigger as stick-Y on AYANEO Xbox-mode pads (#394).
            val rsAxes = rightStickAxes(ev.deviceId)
            dispatchStick(ev, ControllerMappings.rightStickMode(port),
                rsAxes.first, rsAxes.second,
                aXPos = 121, aXNeg = 123, aYPos = 122, aYNeg = 120, // R right/left, down/up
                leftStick = false, port = port)
            // Fire any ARMSX2 hotkey bound (Hotkeys tab) to a stick DIRECTION — lets an
            // unused stick trigger Quick Save/Load etc. The stick still drives the pad.
            fireStickHotkeys(ev, port)
            // D-pad: the physical HAT *and* any stick remapped to D-pad drive the
            // same four PAD buttons. Combine every source and write each direction
            // once — otherwise the centered HAT released the stick-as-D-pad press
            // on the very same motion event (last write wins), so a stick set to
            // D-pad never registered while face-button mapping (different codes)
            // worked fine.
            dispatchDpadCombined(ev, port)
            // Analog triggers (L2/R2). Xbox / DualShock / most modern pads report these as
            // 0..1 motion-axis values, not KEYCODE_BUTTON_L2/R2 key events, so the direct key
            // path never sees them. Which axes that means per device is triggerAxes' job.
            sendTrigger(ev, left = true, port = port)
            sendTrigger(ev, left = false, port = port)
            // Physical STICK DIRECTIONS bound to a PS2 control via the "(send)"
            // rows — e.g. R-Stick Down bound to send Square. The analog "(send)"
            // targets contribute to the merge layer like every other writer.
            dispatchStickDirBindings(ev, port)
            // Single write per analog code per event, merged across ALL writers.
            flushAnalogAxes(port)
            debugStickProbe(ev)
            return true
        }
        return super.dispatchGenericMotionEvent(ev)
    }

    // ---- Physical stick-direction → bound PS2 control ("(send)" rows) ------
    // The Pad tab's stick-target rows may be bound to ANY physical input; when the
    // physical side is a stick direction (reserved keycodes 1000-1007), keys never
    // fire for it in gameplay — this pass reads the axes each motion event and
    // drives the bound PS2 target: proportionally for an analog target (via the
    // merge layer), thresholded for a digital one (change-tracked per code so we
    // only write edges, like dispatchDpadCombined).
    private val stickDirDigitalHeld = Array(8) { HashSet<Int>() } // per unified pad slot (multitap)
    private fun dispatchStickDirBindings(ev: MotionEvent, port: Int) {
        for (left in booleanArrayOf(true, false)) {
            // Same axis correction the main dispatch applies (swap, then inverts).
            val (rightX, rightY) = rightStickAxes(ev.deviceId)
            var vx = ev.getAxisValue(if (left) MotionEvent.AXIS_X else rightX)
            var vy = ev.getAxisValue(if (left) MotionEvent.AXIS_Y else rightY)
            if (ControllerMappings.stickSwapXY(left)) { val t = vx; vx = vy; vy = t }
            if (ControllerMappings.stickInvertX(left)) vx = -vx
            if (ControllerMappings.stickInvertY(left)) vy = -vy
            for (dir in ControllerMappings.StickDir.values()) {
                val physCode = ControllerMappings.stickHotkeyKeyCode(left, dir)
                val target = ControllerMappings.targetForPhysical(physCode, port) ?: continue
                val mag = when (dir) {
                    ControllerMappings.StickDir.UP -> -vy
                    ControllerMappings.StickDir.DOWN -> vy
                    ControllerMappings.StickDir.LEFT -> -vx
                    ControllerMappings.StickDir.RIGHT -> vx
                }.coerceAtLeast(0f)
                if (target in 110..123) {
                    accumAnalog(target, shapeStickMag(mag, left))
                } else {
                    val held = stickDirDigitalHeld[port]
                    val on = mag > STICK_DIGITAL_THRESHOLD
                    val was = held.contains(target)
                    if (on != was) {
                        NativeApp.setPadButtonForPort(port, target, if (on) 32767 else 0, on)
                        if (on) held.add(target) else held.remove(target)
                    }
                }
            }
        }
    }

    // Rate-limited raw-axis probe for the right-stick diagonals report (Area 51:
    // camera moves only in a cross pattern on Android). OFF unless the tester sets
    // prefs boolean "debug.stickLog" true. Shows the raw axes, the corrected pair
    // and the shaped radial output in logcat + the exportable emulog.
    // Right-stick axis pair, resolved per device and cached.
    //
    // Standard Android pads put the right stick on AXIS_Z/AXIS_RZ, but some controllers —
    // Nintendo Joy-Cons notably — report it on AXIS_RX/AXIS_RY. Every right-stick path here
    // read Z/RZ unconditionally, so on those pads the right stick's DIRECTIONS were simply
    // invisible: they couldn't be bound, folded onto the D-pad, or fire a stick hotkey — while
    // R3 bound fine, because R3 is a KEYCODE and not an axis. That asymmetry is exactly what
    // was reported. InputDevice.getDevice() is a binder call and motion events arrive far too
    // often to query per event, hence the cache.
    private val rightStickAxisCache = HashMap<Int, Pair<Int, Int>>()
    private fun rightStickAxes(deviceId: Int): Pair<Int, Int> = rightStickAxisCache.getOrPut(deviceId) {
        val dev = runCatching { InputDevice.getDevice(deviceId) }.getOrNull()
        fun has(axis: Int) = dev?.getMotionRange(axis) != null
        val hasRxRy = has(MotionEvent.AXIS_RX) || has(MotionEvent.AXIS_RY)
        // AXIS_RZ that idles at 0 (range min >= 0) is a TRIGGER, not a stick — some pads (AYANEO
        // handhelds in Xbox mode) put the right trigger on RZ. A real right-stick-Y spans -1..1, so
        // this never reclassifies a standard pad's stick.
        val rz = dev?.getMotionRange(MotionEvent.AXIS_RZ)
        val rzIsTrigger = rz != null && rz.min >= 0f
        when {
            // Joy-Cons expose RX/RY for the right stick; prefer it even if Z/RZ also exist.
            dev?.vendorId == 0x057E && hasRxRy -> MotionEvent.AXIS_RX to MotionEvent.AXIS_RY
            // RZ is really a trigger — the right stick can't live on it; use RX/RY when present.
            rzIsTrigger && hasRxRy -> MotionEvent.AXIS_RX to MotionEvent.AXIS_RY
            // Any pad with no Z/RZ at all but with RX/RY: that IS its right stick.
            !has(MotionEvent.AXIS_Z) && !has(MotionEvent.AXIS_RZ) && hasRxRy ->
                MotionEvent.AXIS_RX to MotionEvent.AXIS_RY
            else -> MotionEvent.AXIS_Z to MotionEvent.AXIS_RZ
        }
    }

    // Which axes carry the [left]/right trigger on this pad: LTRIGGER/RTRIGGER (modern),
    // BRAKE/GAS (older Moga, some BT mappings), or plain Z/RZ when Android has no vendor key
    // layout for the pad and passes raw HID through — AYANEO Xbox mode on the right (#394), a
    // plain Xbox controller on BOTH. Only a 0..1 range qualifies (a stick axis spans -1..1), so
    // a standard pad's right stick is never taken for a trigger; -1 = absent. The left side had
    // no such fallback, so on those pads LT was read by nothing at all. One resolver for capture
    // AND gameplay, so a bind can't capture an axis gameplay doesn't read. Cached:
    // InputDevice.getDevice is a binder call and motion events are far too frequent for it.
    private val triggerAxisCache = HashMap<Int, Triple<Int, Int, Int>>()
    private fun triggerAxes(deviceId: Int, left: Boolean): Triple<Int, Int, Int> =
        triggerAxisCache.getOrPut(deviceId * 2 + (if (left) 0 else 1)) {
            val raw = if (left) MotionEvent.AXIS_Z else MotionEvent.AXIS_RZ
            val range = runCatching { InputDevice.getDevice(deviceId)?.getMotionRange(raw) }.getOrNull()
            Triple(
                if (left) MotionEvent.AXIS_LTRIGGER else MotionEvent.AXIS_RTRIGGER,
                if (left) MotionEvent.AXIS_BRAKE else MotionEvent.AXIS_GAS,
                if (range != null && range.min >= 0f) raw else -1,
            )
        }

    /** 0..1 travel on the [left]/right trigger — highest of the candidate axes, negatives
     *  clamped (some pads idle an unused trigger axis at -1). Returns **-1 when the pad has no
     *  trigger axis on that side**, which is not the same as one resting at zero: a Switch Pro
     *  Controller sends L2/R2 as key events only, and reading its absent axes as 0.0 once wrote
     *  "released" every motion event, cancelling a held R2 whenever the stick moved. */
    private fun triggerTravel(ev: MotionEvent, left: Boolean): Float {
        val (a, b, c) = triggerAxes(ev.deviceId, left)
        if (!deviceHasAxis(ev.deviceId, a) && !deviceHasAxis(ev.deviceId, b) &&
            !deviceHasAxis(ev.deviceId, c))
            return -1f
        return maxOf(
            maxOf(ev.getAxisValue(a), ev.getAxisValue(b)),
            if (c >= 0) ev.getAxisValue(c) else 0f,
        ).coerceIn(0f, 1f)
    }

    /** The keycode a trigger stands in for. The binding model is keyed on keycodes and most
     *  pads give their triggers none, so every trigger path — capture and gameplay — refers to
     *  them by the code a key-emitting pad would send. */
    private fun triggerKeyCode(left: Boolean): Int =
        if (left) KeyEvent.KEYCODE_BUTTON_L2 else KeyEvent.KEYCODE_BUTTON_R2

    private var lastStickProbeMs = 0L
    private fun debugStickProbe(ev: MotionEvent) {
        if (!prefs.getBoolean("debug.stickLog", false)) return
        val now = SystemClock.uptimeMillis()
        if (now - lastStickProbeMs < 250) return
        lastStickProbeMs = now
        val z = ev.getAxisValue(MotionEvent.AXIS_Z)
        val rz = ev.getAxisValue(MotionEvent.AXIS_RZ)
        val rx = ev.getAxisValue(MotionEvent.AXIS_RX)
        val ry = ev.getAxisValue(MotionEvent.AXIS_RY)
        val mag = kotlin.math.hypot(z, rz)
        println("@@STICKPROBE@@ dev=${ev.deviceId} Z=%.3f RZ=%.3f RX=%.3f RY=%.3f mag=%.3f shaped=%.3f".format(
            z, rz, rx, ry, mag, shapeStickMag(mag.coerceAtMost(1f), false)))
    }

    // ---- Joy-Con / controller input diagnostic (tag: ARMSX2_JOYCON) --------
    // Dumps EXACTLY what a physical controller emits so a reporter can capture
    // (adb logcat -s ARMSX2_JOYCON) what e.g. a Nintendo Joy-Con d-pad actually
    // sends on their Android build — the unknown that blocks the real remap fix.
    // Pure logging, zero behaviour change. Release builds never enable this path.
    // Debug builds default OFF and can opt in with prefs "debug.joyconLog"=true.
    // Full device info
    // is logged once per deviceId; the per-event axis dump is throttled + non-zero.
    private val joyconLoggedDevices = HashSet<Int>()
    private var lastJoyconMotionLogMs = 0L
    private fun joyconLogEnabled(): Boolean =
        // Pref-gated, NOT BuildConfig.DEBUG-gated. The old `BuildConfig.DEBUG && pref` form
        // made this permanently unreachable in a release build — i.e. unreachable for exactly
        // the testers whose controllers we need to identify. It cost a round trip on the
        // 8BitDo/Switch-Pro trigger report. Default off; costs one boolean read per event.
        prefs.getBoolean("debug.joyconLog", false)

    /** Emit a diagnostic line to BOTH logcat (adb `-s ARMSX2_JOYCON`) AND the emulog (in-app
     *  Save Log — so a handheld tester with no PC can capture it). NativeApp.emulog no-ops
     *  safely when the native console isn't open yet (e.g. pre-boot). */
    private fun joyconEmit(msg: String) {
        android.util.Log.d("ARMSX2_JOYCON", msg)
        runCatching { NativeApp.emulog("@@JOYCON@@ $msg") }
    }

    /** One-time full dump of a controller: ids, name, sources, and every motion axis
     *  (id + name + range/flat/fuzz). Fires the first time a gamepad deviceId is seen. */
    private fun logControllerDeviceOnce(deviceId: Int) {
        if (!joyconLogEnabled() || deviceId < 0) return
        if (!joyconLoggedDevices.add(deviceId)) return
        val dev = InputDevice.getDevice(deviceId) ?: return
        val src = dev.sources
        val isPad = (src and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (src and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        if (!isPad) return
        joyconEmit(
            "DEVICE id=%d vendor=0x%04x product=0x%04x sources=0x%08x name=\"%s\"".format(
                deviceId, dev.vendorId, dev.productId, src, dev.name ?: "?"))
        for (r in dev.motionRanges) {
            joyconEmit(
                "  axis=%d (%s) src=0x%08x min=%.3f max=%.3f flat=%.3f fuzz=%.3f".format(
                    r.axis, MotionEvent.axisToString(r.axis), r.source, r.min, r.max, r.flat, r.fuzz))
        }
    }

    /** Throttled dump of every NON-ZERO axis on a controller motion event, so a reporter
     *  can see which axis (HAT? stick? something else?) the Joy-Con d-pad actually drives.
     *  Accepts DPAD-sourced events too — those are exactly the ones we're hunting. */
    private fun logControllerMotion(ev: MotionEvent) {
        if (!joyconLogEnabled()) return
        if (!ev.isFromSource(InputDevice.SOURCE_JOYSTICK) &&
            !ev.isFromSource(InputDevice.SOURCE_GAMEPAD) &&
            !ev.isFromSource(InputDevice.SOURCE_DPAD)) return
        val now = SystemClock.uptimeMillis()
        if (now - lastJoyconMotionLogMs < 80) return
        val ranges = ev.device?.motionRanges ?: return
        val sb = StringBuilder()
        for (r in ranges) {
            val v = ev.getAxisValue(r.axis)
            if (kotlin.math.abs(v) > 0.001f) sb.append(" %s=%.3f".format(MotionEvent.axisToString(r.axis), v))
        }
        if (sb.isEmpty()) return
        lastJoyconMotionLogMs = now
        joyconEmit(
            "MOTION id=%d vendor=0x%04x src=0x%08x%s".format(
                ev.deviceId, ev.device?.vendorId ?: -1, ev.source, sb))
    }

    /** Log a controller key event (code + name + action). Low frequency, so no throttle. */
    private fun logControllerKey(event: KeyEvent) {
        if (!joyconLogEnabled()) return
        if (!event.isFromSource(InputDevice.SOURCE_GAMEPAD) &&
            !event.isFromSource(InputDevice.SOURCE_JOYSTICK)) return
        val a = when (event.action) {
            KeyEvent.ACTION_DOWN -> "DOWN"; KeyEvent.ACTION_UP -> "UP"; else -> "?"
        }
        joyconEmit(
            "KEY id=%d vendor=0x%04x code=%d (%s) scan=%d %s repeat=%d".format(
                event.deviceId, InputDevice.getDevice(event.deviceId)?.vendorId ?: -1,
                event.keyCode, KeyEvent.keyCodeToString(event.keyCode), event.scanCode, a, event.repeatCount))
    }

    // True whenever a Compose frontend surface is drawn over (or instead of) the
    // game and should own the gamepad. Every navigable surface must be listed
    // here or its D-pad/A/B never reach Compose. The three explicit surfaces cover
    // the in-game overlays (pause menu, Save/Load & manager screens, the library
    // shown over a running game); when NO game is RUNNING the whole app IS the
    // frontend (root library + every manager/settings sub-screen reached from the
    // drawer), so the pad drives it unconditionally.
    private fun controllerDrivesFrontend(): Boolean =
        WindowImpl.overlayVisible.value ||
            WindowImpl.inGameScreen.value != null ||
            WindowImpl.showLibrary.value ||
            // A modal can be raised over a RUNNING game with none of the above up. Without
            // this the motion ladder is never even entered (its caller gates on this
            // predicate), so the modal would answer a D-pad and ignore a stick — and the
            // gameplay hotkeys below would keep firing behind the scrim.
            com.armsx2.ui.common.PadModals.visible ||
            eState.value != EmuState.RUNNING

    // B / BACK from any frontend surface EXCEPT the pause overlay and the library
    // cover grid (each consumes its own B earlier). Peels
    // the topmost layer: modal dialog > nav drawer > in-game manager screen >
    // library sub-route (Settings/Bios/... reached inside the in-game library) >
    // the library overlay itself > a root sub-route > (root Home) open the drawer.
    private fun handleFrontendBack() {
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.BACK)
        val nav = com.armsx2.navigation.UiNavigator
        val onHome = nav.route.value == com.armsx2.navigation.AppRoute.Home
        when {
            nav.drawerOpen.value -> nav.drawerOpen.value = false
            WindowImpl.inGameScreen.value != null -> WindowImpl.dismissInGameScreen()
            WindowImpl.showLibrary.value && !onHome -> nav.back()
            WindowImpl.showLibrary.value -> WindowImpl.showLibrary.value = false
            !onHome -> nav.back()
            // Root library home with nothing above it: B opens the nav drawer
            // (mirrors the cover-grid B handled in HomeInputController.back()).
            else -> nav.drawerOpen.value = true
        }
    }

    // --- Controller menu nav hold-to-repeat ---------------------------------
    // The per-frame stick handlers below are edge-triggered (one move per push),
    // which makes holding a direction feel dead/clunky. While a direction is
    // held we run a repeat loop so the selection keeps travelling, matching
    // normal D-pad-menu behaviour.
    private var navRepeatJob: kotlinx.coroutines.Job? = null
    private var navRepeatDx = 0
    private var navRepeatDy = 0

    private fun directionKeyCode(dx: Int, dy: Int): Int = when {
        dx < 0 -> KeyEvent.KEYCODE_DPAD_LEFT
        dx > 0 -> KeyEvent.KEYCODE_DPAD_RIGHT
        dy < 0 -> KeyEvent.KEYCODE_DPAD_UP
        dy > 0 -> KeyEvent.KEYCODE_DPAD_DOWN
        else -> 0
    }

    /** The one place a modal's directional input is interpreted, called from BOTH ladders.
     *  Shared on purpose: the key path and the motion path have drifted apart before, and a
     *  modal that walks on a D-pad but not on a stick — or that adjusts a value on one and
     *  moves the selection on the other — is exactly the failure this rung exists to prevent.
     *
     *  Horizontal adjusts the focused control first (stepper −/+, toggle off/on) and only moves
     *  when that control has no adjust action; vertical always moves. Same semantics the
     *  registry already uses on the base screens, so a widget behaves identically inside a
     *  modal and outside one. */
    private fun modalNavMove(dx: Int, dy: Int) {
        val nav = com.armsx2.ui.settings.SettingsControllerNav
        val acted = if (dx != 0 && dy == 0) {
            nav.adjust(dx) || nav.moveSpatial(dx, 0)
        } else {
            nav.moveSpatial(dx, dy)
        }
        // Nowhere left to move: scroll the panel body instead. A modal whose only focusable is
        // its Close button — an info panel, an error notice — would otherwise show a long
        // message with no way to read past the fold, which is the same defect as the window
        // dialog it replaced. Silently does nothing when the modal declared no scrollable body.
        if (!acted && dy != 0) com.armsx2.ui.common.PadModals.scrollTop(dy)
    }

    private fun fireNavMove(dx: Int, dy: Int) {
        // Mirror the key-event routing priority so the analog stick drives every
        // surface the D-pad does.
        when {
            com.armsx2.ui.home.LibraryKeyboard.visible.value -> {
                // Controller search keyboard owns the stick/HAT/D-pad while it's up
                // (this is the RP6 path — its D-pad arrives here as a HAT axis).
                com.armsx2.ui.home.LibraryKeyboard.move(dx, dy)
            }
            com.armsx2.ui.common.PadModals.visible -> {
                // A modal owns the stick and the HAT, mirroring the key rung near the top of
                // dispatchKeyEvent. Note the asymmetry with that rung, which is deliberate: it
                // needs an explicit keyboard exception because the keyboard's block sits below
                // it, whereas this `when` is ordered and terminal, so the keyboard branch above
                // already wins here for free.
                modalNavMove(dx, dy)
            }
            com.armsx2.ui.settingshub.SettingsSearch.visible.value -> {
                // Settings-search result browse (keyboard dismissed): vertical list nav.
                if (dy != 0) com.armsx2.ui.settingshub.SettingsSearch.move(if (dy < 0) -1 else 1)
            }
            com.armsx2.ui.common.ShaderParamsEditor.visible -> {
                // THE path that matters for this editor: on this hardware the D-pad is a
                // HAT axis, so it arrives here and never as KEYCODE_DPAD_*. Missing this
                // is why vc1150's editor did nothing while the pause menu behind it moved.
                // Rides the shared hold-repeat, so a held direction walks the list and
                // sweeps a value.
                com.armsx2.ui.common.ShaderParamsEditor.move(dx, dy)
            }
            WindowImpl.overlayVisible.value -> {
                // Pause menu — two-zone controller handles both the tab column and
                // the registry-driven content pane.
                com.armsx2.ui.emulation.EmulationMenuInputController.move(dx, dy)
            }
            // Library cover grid — only when it actually owns input (same gate as
            // the key path: not behind the drawer / an in-game screen).
            WindowImpl.inGameScreen.value == null &&
                !com.armsx2.navigation.UiNavigator.drawerOpen.value &&
                com.armsx2.ui.home.HomeInputController.active() -> {
                com.armsx2.ui.home.HomeInputController.move(dx, dy)
            }
            // Drawer, in-game manager/Save-Load screens, library sub-routes and
            // every root manager/settings screen: the manual registry (same as the
            // D-pad path). Left/Right adjust the focused control, else move.
            controllerDrivesFrontend() -> {
                val nav = com.armsx2.ui.settings.SettingsControllerNav
                if (dx != 0 && dy == 0) { if (!nav.adjust(dx)) nav.moveSpatial(dx, 0) }
                else nav.moveSpatial(dx, dy)
            }
            else -> {
                // Menu closed while a direction was held — stop repeating.
                stopNavRepeat()
            }
        }
    }

    private fun startNavRepeat(dx: Int, dy: Int) {
        if (dx == 0 && dy == 0) {
            stopNavRepeat()
            return
        }
        if (navRepeatJob?.isActive == true && navRepeatDx == dx && navRepeatDy == dy) return
        stopNavRepeat()
        navRepeatDx = dx
        navRepeatDy = dy
        fireNavMove(dx, dy)
        navRepeatJob = lifecycleScope.launch {
            kotlinx.coroutines.delay(NAV_REPEAT_INITIAL_MS)
            while (true) {
                fireNavMove(navRepeatDx, navRepeatDy)
                kotlinx.coroutines.delay(NAV_REPEAT_INTERVAL_MS)
            }
        }
    }

    private fun stopNavRepeat() {
        navRepeatJob?.cancel()
        navRepeatJob = null
        navRepeatDx = 0
        navRepeatDy = 0
    }

    private fun handleControllerUiMotion(ev: MotionEvent): Boolean {
        if (!ev.isFromSource(InputDevice.SOURCE_JOYSTICK) &&
            !ev.isFromSource(InputDevice.SOURCE_GAMEPAD)
        ) {
            return false
        }
        NativeApp.sRumbleDeviceId = ev.deviceId  // track active gamepad for rumble

        com.armsx2.ui.touch.TouchControls.onControllerInputDetected()
        return if (WindowImpl.overlayVisible.value) {
            handleOverlayControllerMotion(ev)
        } else {
            handleLibraryControllerMotion(ev)
        }
    }

    private fun handleLibraryControllerMotion(ev: MotionEvent): Boolean {
        val scrollY = uiScrollValue(ev.getAxisValue(MotionEvent.AXIS_RZ))
        handleControllerUiScroll(scrollY)

        // Accept BOTH the left stick and the D-pad (HAT axis on this hardware) so
        // handhelds with or without a stick can browse the library.
        val (stickDx, stickDy) = uiDominantStickDirection(
            ev.getAxisValue(MotionEvent.AXIS_X),
            ev.getAxisValue(MotionEvent.AXIS_Y),
        )
        val dx = uiHatDirection(ev.getAxisValue(MotionEvent.AXIS_HAT_X))
            .let { if (it != 0) it else stickDx }
        val dy = uiHatDirection(ev.getAxisValue(MotionEvent.AXIS_HAT_Y))
            .let { if (it != 0) it else stickDy }
        if (dx == 0 && dy == 0) {
            if (libraryAxisX != 0 || libraryAxisY != 0) stopNavRepeat()
            libraryAxisX = 0
            libraryAxisY = 0
            return true
        }

        if (dx != libraryAxisX || dy != libraryAxisY) {
            libraryAxisX = dx
            libraryAxisY = dy
            startNavRepeat(dx, dy)
        }
        return true
    }

    private fun handleOverlayControllerMotion(ev: MotionEvent): Boolean {
        // The overlay accepts BOTH the D-pad and the left analog stick, so
        // handhelds with or without a stick work. On this hardware the D-pad is a
        // HAT axis (not KEYCODE_DPAD_*); the stick is AXIS_X/Y. The adjust
        // skip/stuck bug was in the settings registry (now fixed), not the input
        // layer, so the stick is safe to use again. Right stick scrolls lists.
        handleControllerUiScroll(uiScrollValue(ev.getAxisValue(MotionEvent.AXIS_RZ)))

        val (stickDx, stickDy) = uiDominantStickDirection(
            ev.getAxisValue(MotionEvent.AXIS_X),
            ev.getAxisValue(MotionEvent.AXIS_Y),
        )
        val dirX = uiHatDirection(ev.getAxisValue(MotionEvent.AXIS_HAT_X))
            .let { if (it != 0) it else stickDx }
        val dirY = uiHatDirection(ev.getAxisValue(MotionEvent.AXIS_HAT_Y))
            .let { if (it != 0) it else stickDy }

        // Vertical = move between settings; horizontal = adjust the focused setting
        // (slider / segment). BOTH hold-to-repeat now — slider tweaks were previously
        // one-step-per-press, painful on long sliders (deadzone/sensitivity/etc.).
        // One repeat job at a time, so pick the dominant axis (vertical wins a tie);
        // returning to centre stops it. Toggle onLeft/onRight are idempotent (set
        // once then no-op), so repeating a held direction on a toggle is safe.
        when {
            dirY != 0 -> {
                if (dirY != overlayAxisY || overlayAxisX != 0) startNavRepeat(0, dirY)
                overlayAxisY = dirY
                overlayAxisX = 0
            }
            dirX != 0 -> {
                if (dirX != overlayAxisX || overlayAxisY != 0) startNavRepeat(dirX, 0)
                overlayAxisX = dirX
                overlayAxisY = 0
            }
            else -> {
                if (overlayAxisX != 0 || overlayAxisY != 0) stopNavRepeat()
                overlayAxisX = 0
                overlayAxisY = 0
            }
        }
        return true
    }

    private fun handleControllerUiScroll(velocityY: Float) {
        if (WindowImpl.overlayVisible.value) {
            com.armsx2.ui.settings.SettingsControllerNav.setScrollVelocity(velocityY)
        } else if (com.armsx2.ui.home.HomeInputController.active()) {
            com.armsx2.ui.home.HomeInputController.scroll(velocityY)
        }
    }

    // Last HAT direction seen during an active capture, so a held D-pad binds once
    // (on the neutral→direction transition) instead of repeating. Reset to 0 on any
    // non-capture motion event so each capture session starts fresh.
    private var captureHatX = 0
    private var captureHatY = 0

    /** During a pad/hotkey (re)bind, turn HAT-axis D-pad presses and firm stick
     *  pushes into synthetic KeyEvents routed through the normal capture path.
     *  Always consumes the motion so the D-pad/stick can't navigate the UI while
     *  capturing.
     *
     *  HELD-STATE MODEL (stick/D-pad + button combos): each engaged direction
     *  dispatches a synthetic DOWN when it engages and a synthetic UP only when it
     *  RELEASES — mirroring a real button. The old code fired DOWN+UP instantly,
     *  which (a) finalized every capture as a single-key bind the moment a stick
     *  moved ("the moment you hold the stick it registers just the stick"), and
     *  (b) made a direction unusable as a combo member (the zero eventTime of the
     *  bare KeyEvent constructor failed the combo anti-ghost gap check). Synthetic
     *  events now carry real uptimeMillis timestamps, so hold-direction-then-press-
     *  button and hold-button-then-push-direction both bind combos, and a push
     *  released with nothing else still binds the plain single direction. */
    private val captureHeldSynth = HashSet<Int>()
    private fun handleCaptureMotion(ev: MotionEvent): Boolean {
        // Desired engaged-direction set for this event: at most one per HAT axis
        // pair and one per stick (dominant direction), so sweeping through a
        // diagonal can't spuriously bind a two-direction combo.
        val want = HashSet<Int>()
        val dx = uiHatDirection(ev.getAxisValue(MotionEvent.AXIS_HAT_X))
        val dy = uiHatDirection(ev.getAxisValue(MotionEvent.AXIS_HAT_Y))
        if (dx != 0) want.add(if (dx > 0) KeyEvent.KEYCODE_DPAD_RIGHT else KeyEvent.KEYCODE_DPAD_LEFT)
        if (dy != 0) want.add(if (dy > 0) KeyEvent.KEYCODE_DPAD_DOWN else KeyEvent.KEYCODE_DPAD_UP)
        captureStickCode(ev, MotionEvent.AXIS_X, MotionEvent.AXIS_Y, true).takeIf { it != 0 }?.let { want.add(it) }
        // Right stick via the per-device pair — on a Joy-Con this is RX/RY, and reading Z/RZ
        // here is why its directions could never be bound.
        val (capRightX, capRightY) = rightStickAxes(ev.deviceId)
        captureStickCode(ev, capRightX, capRightY, false).takeIf { it != 0 }?.let { want.add(it) }
        // Analog TRIGGERS, same treatment: on a pad whose triggers are axis-only (an Xbox
        // controller, and most modern pads) the capture saw nothing at all when one was pulled
        // — and since this method consumes the motion, not even a UI twitch to explain why.
        // Standing in the keycode a key-emitting pad would send makes the trigger an ordinary
        // button downstream, and gameplay resolves that same code back (sendTrigger).
        for (left in booleanArrayOf(true, false)) {
            if (triggerTravel(ev, left) > TRIGGER_DIGITAL_THRESHOLD) want.add(triggerKeyCode(left))
        }
        captureHatX = dx
        captureHatY = dy
        val now = SystemClock.uptimeMillis()
        // Releases first (a direction that flipped is an UP then a DOWN).
        val released = captureHeldSynth.filter { it !in want }
        for (code in released) {
            captureHeldSynth.remove(code)
            // Re-enter dispatchKeyEvent (not super) so it reaches the hotkey
            // capture AND, while padCapturing, falls through to Compose's
            // onPreviewKeyEvent which records the pad bind.
            dispatchKeyEvent(KeyEvent(now, now, KeyEvent.ACTION_UP, code, 0))
        }
        for (code in want) {
            if (captureHeldSynth.add(code))
                dispatchKeyEvent(KeyEvent(now, now, KeyEvent.ACTION_DOWN, code, 0))
        }
        // Binding may have completed mid-loop (endHotkeyCapture); drop any held
        // state so the next capture session starts clean (incl. heldKeys, since no
        // UP will ever arrive for these synthetic codes).
        if (ControllerMappings.captureHotkey.value == null && !ControllerMappings.padCapturing.value) {
            heldKeys.removeAll(captureHeldSynth)
            captureHeldSynth.clear()
        }
        return true
    }

    /** The reserved hotkey keycode for whichever direction of the [left]/right stick is
     *  pushed past a firm threshold during capture, or 0 if centered. */
    private fun captureStickCode(ev: MotionEvent, axisX: Int, axisY: Int, left: Boolean): Int {
        val x = ev.getAxisValue(axisX)
        val y = ev.getAxisValue(axisY)
        val t = 0.7f
        return when {
            y <= -t -> ControllerMappings.stickHotkeyKeyCode(left, ControllerMappings.StickDir.UP)
            y >= t -> ControllerMappings.stickHotkeyKeyCode(left, ControllerMappings.StickDir.DOWN)
            x <= -t -> ControllerMappings.stickHotkeyKeyCode(left, ControllerMappings.StickDir.LEFT)
            x >= t -> ControllerMappings.stickHotkeyKeyCode(left, ControllerMappings.StickDir.RIGHT)
            else -> 0
        }
    }

    private fun uiHatDirection(value: Float): Int = when {
        value > UI_HAT_DEAD -> 1
        value < -UI_HAT_DEAD -> -1
        else -> 0
    }

    private fun uiDominantStickDirection(x: Float, y: Float): Pair<Int, Int> {
        val absX = abs(x)
        val absY = abs(y)
        if (absX < UI_NAV_DEAD && absY < UI_NAV_DEAD)
            return 0 to 0
        return if (absX >= absY)
            (if (x > 0f) 1 else -1) to 0
        else
            0 to (if (y > 0f) 1 else -1)
    }

    private fun uiAxisDirection(value: Float): Int = when {
        value > UI_NAV_DEAD -> 1
        value < -UI_NAV_DEAD -> -1
        else -> 0
    }

    private fun uiScrollValue(value: Float): Float {
        val dead = 0.18f
        return when {
            value > dead -> ((value - dead) / (1f - dead)).coerceIn(0f, 1f)
            value < -dead -> ((value + dead) / (1f - dead)).coerceIn(-1f, 0f)
            else -> 0f
        }
    }

    private fun recordUiNav(keyCode: Int, fromAxis: Boolean) {
        lastUiNavCode = keyCode
        lastUiNavAt = SystemClock.uptimeMillis()
        lastUiNavWasAxis = fromAxis
    }

    private fun shouldSuppressUiNav(keyCode: Int, fromAxis: Boolean, now: Long): Boolean {
        if (lastUiNavCode != keyCode) return false
        val age = now - lastUiNavAt
        return lastUiNavWasAxis != fromAxis && age <= UI_KEY_AXIS_SUPPRESS_MS
    }

    private fun dispatchSyntheticUiKey(keyCode: Int): Boolean {
        val now = SystemClock.uptimeMillis()
        val flags = KeyEvent.FLAG_FROM_SYSTEM or KeyEvent.FLAG_VIRTUAL_HARD_KEY
        val source = InputDevice.SOURCE_KEYBOARD or InputDevice.SOURCE_DPAD
        val down = KeyEvent(
            now, now, KeyEvent.ACTION_DOWN, keyCode, 0, 0,
            KeyCharacterMap.VIRTUAL_KEYBOARD, 0, flags, source
        )
        val up = KeyEvent(
            now, now, KeyEvent.ACTION_UP, keyCode, 0, 0,
            KeyCharacterMap.VIRTUAL_KEYBOARD, 0, flags, source
        )
        val downHandled = super.dispatchKeyEvent(down)
        val upHandled = super.dispatchKeyEvent(up)
        return downHandled || upHandled
    }

    /** Apply the user's stick sensitivity (linear output scale) + acceleration
     *  (exponential response curve) to a post-deadzone magnitude in [0,1]. accel 0
     *  = linear; higher = finer control near center, faster toward full tilt. Only
     *  shapes real analog output (native passthrough + CUSTOM analog targets). */
    private fun shapeStickMag(m: Float, left: Boolean): Float {
        val dz = ControllerMappings.stickDeadzone(left)
        if (m <= dz) return 0f
        // Re-normalize the window [dz, 1-outer] to [0, 1] so output ramps smoothly
        // from 0 past the inner deadzone (no jump), and reaches FULL at (1-outer) —
        // the outer/anti-deadzone lets a short-throw stick that can't physically
        // reach its corners still hit 100%. Then apply the accel curve + sensitivity.
        // ALL feel tunables are PER-STICK now (left/right independent).
        val outer = ControllerMappings.stickOuterDeadzone(left)
        val hi = (1f - outer).coerceAtLeast(dz + 0.01f) // upper edge; guard hi > dz
        val t = ((m - dz) / (hi - dz)).coerceIn(0f, 1f)
        val accel = ControllerMappings.stickAcceleration(left)
        // Acceleration + the response-curve preset compose into one exponent (both reshape
        // magnitude; exp==1 = linear = unchanged).
        val exp = 1f + accel + ControllerMappings.stickCurveGamma(left)
        val curved =
            if (exp != 1f) Math.pow(t.toDouble(), exp.toDouble()).toFloat()
            else t
        val out = (curved * ControllerMappings.stickSensitivity(left)).coerceIn(0f, 1f)
        // Anti-deadzone (output floor): lift ANY non-zero output up to start at the floor,
        // so a game with its own large stick deadzone responds the instant the stick moves
        // and the rest of the travel maps proportionally above it (no dead bottom, no jump).
        // True center (out == 0) stays 0. 0 floor = unchanged behaviour.
        if (out <= 0f) return 0f
        val anti = ControllerMappings.stickAntiDeadzone(left)
        return if (anti > 0f) (anti + out * (1f - anti)).coerceIn(0f, 1f) else out
    }

    // ---- Analog-code merge layer (native codes 110-123) --------------------
    // Several writers can drive the SAME PS2 stick direction in one motion event:
    // the physical stick (ANALOG mode), a CUSTOM direction defaulting to analog,
    // the D-pad HAT fold, a trigger bound to a stick direction, and a stick
    // direction of the OTHER stick bound via the "(send)" rows. Before this layer
    // each writer set the code directly, so whichever wrote LAST (usually the
    // resting real stick, at 0) released everyone else's deflection — the same
    // clobber class as the old dispatchDpadCombined bug. Now every motion-event
    // writer CONTRIBUTES (max per code) and flushAnalogAxes writes each code once.
    // Button-held deflections (sendKeyAction: a KEY bound to an analog code, incl.
    // d-pad-as-left-stick key path) are tracked in [analogKeyHeld] and folded into
    // every flush so stick motion can no longer release a held button-deflection.
    private val analogAccum = HashMap<Int, Float>()
    private val analogPrevSent = Array(8) { HashMap<Int, Float>() } // per unified pad slot (multitap)
    val analogKeyHeld = Array(8) { HashMap<Int, Float>() } // written by sendKeyAction; per unified pad slot

    // ---- Gyro <-> physical-stick ADDITIVE combine (P1 / port 0) -----------
    // The aim/steer gyro drives a PS2 analog stick; so does the physical stick.
    // They used to clobber (raw setPadButton, last-writer-wins), so moving one
    // killed the other. Instead the gyro is folded in as a SIGNED addend on top
    // of the physical stick that shares its axis, then clamped to the unit circle
    // by accumStickRadial — coarse stick aim + fine gyro adjust AT ONCE. Both the
    // MotionEvent path and the sensor callback run on the main looper, so these
    // are read/written without extra locking (volatile documents the sharing).
    @Volatile private var gyroCombineActive = false   // gyro currently deflected
    @Volatile private var gyroCombineLeft = false     // gyro drives left(true)/right(false) stick
    @Volatile private var gyroVecX = 0f               // signed gyro contribution, [-1,1]
    @Volatile private var gyroVecY = 0f
    private val lastPhysStickX = floatArrayOf(0f, 0f) // [0]=left [1]=right, P1 physical analog
    private val lastPhysStickY = floatArrayOf(0f, 0f)

    private fun accumAnalog(code: Int, v: Float) {
        if (v <= 0f) return
        val cur = analogAccum[code] ?: 0f
        if (v > cur) analogAccum[code] = v
    }

    /** Write the merged analog codes for this motion event: union of the fresh
     *  contributions, the key-held deflections, and everything sent last event
     *  (so stale codes release exactly once). */
    private fun flushAnalogAxes(port: Int) {
        val prev = analogPrevSent[port]
        for ((code, held) in analogKeyHeld[port]) accumAnalog(code, held)
        // Release pass: codes we sent before but that have no contribution now.
        for (code in prev.keys) {
            if (!analogAccum.containsKey(code)) {
                NativeApp.setPadButtonForPort(port, code, 0, false)
            }
        }
        for ((code, v) in analogAccum) {
            if (prev[code] != v)
                NativeApp.setPadButtonForPort(port, code, (v * 32767).toInt(), true)
        }
        prev.clear()
        prev.putAll(analogAccum)
        analogAccum.clear()
    }

    /** RADIAL analog-stick shaping: deadzone/curve/sensitivity applied to the
     *  stick's radial magnitude (not per-axis), so diagonals shape identically to
     *  cardinals — a per-axis deadzone was a "square" zone that ate diagonals.
     *  Direction is preserved exactly; only the magnitude is reshaped. */
    private fun accumStickRadial(vx: Float, vy: Float, left: Boolean,
                                 aXPos: Int, aXNeg: Int, aYPos: Int, aYNeg: Int) {
        // Off-axis BLEED gate (fixes the "push up also presses right" regression). Moving
        // the deadzone from per-axis to radial (above) stopped diagonals being eaten, but
        // the old per-axis zone was also silently cleaning up small perpendicular values —
        // so a near-cardinal push on a stick that doesn't sit perfectly centered on the
        // other axis now leaks that value through. Restore the cleanup WITHOUT the square
        // zone: drop the minor axis only when it's a small fraction of the major one. That
        // snaps just very shallow (~<9°) diagonals to the cardinal; genuine diagonals (minor
        // axis well above STICK_CROSS_GATE of the major) are untouched, so 8-way is intact.
        var gx = vx
        var gy = vy
        val ax = abs(gx)
        val ay = abs(gy)
        if (ax >= ay) { if (ay < ax * STICK_CROSS_GATE) gy = 0f }
        else { if (ax < ay * STICK_CROSS_GATE) gx = 0f }
        val mag = kotlin.math.hypot(gx, gy)
        if (mag <= 0f) return
        val shaped = shapeStickMag(mag.coerceAtMost(1f), left)
        val scale = shaped / mag // preserves direction; caps square-gate diagonals at unit circle
        val ox = gx * scale
        val oy = gy * scale
        if (ox > 0f) accumAnalog(aXPos, ox) else if (ox < 0f) accumAnalog(aXNeg, -ox)
        if (oy > 0f) accumAnalog(aYPos, oy) else if (oy < 0f) accumAnalog(aYNeg, -oy)
    }

    /** Gyro (aim mode 1 / steer mode 2) as an ADDITIVE stick contributor. Called
     *  from the sensor callback on the main looper. [gx],[gy] are the signed,
     *  smoothed gyro vector in [-1,1]; (0,0) on settle/stop releases it. The gyro
     *  sums with whichever physical stick shares its axis (aim -> right, or the
     *  user-chosen left for RE4-style games; steer -> left) so coarse stick aim
     *  and fine gyro adjustment work together instead of clobbering each other. */
    fun onGyroAnalog(mode: Int, gx: Float, gy: Float) {
        gyroCombineLeft = mode == 2 ||
            (mode == 1 && ControllerMappings.gyroAimStick() == ControllerMappings.GYRO_STICK_LEFT)
        gyroVecX = gx; gyroVecY = gy
        gyroCombineActive = gx != 0f || gy != 0f
        emitCombinedSticks()
    }

    /** Re-drive BOTH P1 sticks from their last physical vector plus the gyro addend
     *  on the target side, then flush once. Re-contributing the NON-target stick is
     *  what stops flushAnalogAxes' release pass from dropping it when only the gyro
     *  moved (single owner of the analog codes = the shared merge layer). flush only
     *  writes codes whose value changed, so an unchanged stick costs nothing. */
    private fun emitCombinedSticks() {
        val gxL = if (gyroCombineLeft) gyroVecX else 0f
        val gyL = if (gyroCombineLeft) gyroVecY else 0f
        val gxR = if (gyroCombineLeft) 0f else gyroVecX
        val gyR = if (gyroCombineLeft) 0f else gyroVecY
        accumStickRadial(lastPhysStickX[0] + gxL, lastPhysStickY[0] + gyL, true,  111, 113, 112, 110)
        accumStickRadial(lastPhysStickX[1] + gxR, lastPhysStickY[1] + gyR, false, 121, 123, 122, 120)
        flushAnalogAxes(0)
    }

    /** Route one physical stick's two axes to the PS2 pad per [mode]: native analog
     *  stick (default), thresholded digital D-pad / face presses, or per-direction
     *  CUSTOM binds. [leftStick] selects which stick's CUSTOM binds to read. */
    private fun dispatchStick(
        event: MotionEvent, mode: ControllerMappings.StickMode,
        axisX: Int, axisY: Int,
        aXPos: Int, aXNeg: Int, aYPos: Int, aYNeg: Int,
        leftStick: Boolean, port: Int,
    ) {
        // Read the raw axis values once, then apply the per-stick axis correction
        // (swap X/Y first, then invert each) BEFORE any mode dispatch — so it fixes
        // pads that read rotated/mirrored ("down is up, left is right") in Analog,
        // Face and Custom modes alike.
        var vx = event.getAxisValue(axisX)
        var vy = event.getAxisValue(axisY)
        if (ControllerMappings.stickSwapXY(leftStick)) { val t = vx; vx = vy; vy = t }
        if (ControllerMappings.stickInvertX(leftStick)) vx = -vx
        if (ControllerMappings.stickInvertY(leftStick)) vy = -vy
        when (mode) {
            ControllerMappings.StickMode.ANALOG -> {
                // Radial shaping into the merge layer (flushAnalogAxes writes once
                // per event, after every contributor has been folded in).
                // P1 (port 0): remember this stick's PHYSICAL vector and, when the
                // gyro is driving THIS stick, sum the gyro's signed addend on top so
                // coarse stick aim + fine gyro adjust simultaneously (onGyroAnalog).
                // Stored value is pre-gyro so the sensor path can add gyro cleanly.
                var sx = vx; var sy = vy
                if (port == 0) {
                    val si = if (leftStick) 0 else 1
                    lastPhysStickX[si] = vx; lastPhysStickY[si] = vy
                    if (gyroCombineActive && gyroCombineLeft == leftStick) { sx += gyroVecX; sy += gyroVecY }
                }
                accumStickRadial(sx, sy, leftStick, aXPos, aXNeg, aYPos, aYNeg)
                if (leftStick && ControllerMappings.dpadAsLeftStick()) {
                    // Fold the physical D-pad (HAT) into the left stick so the
                    // D-pad drives analog movement — full deflection, unshaped
                    // (a d-pad press is digital). The HAT is gated out of
                    // dispatchDpadCombined while this is on.
                    val hx = event.getAxisValue(MotionEvent.AXIS_HAT_X)
                    val hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
                    if (hx > STICK_DEAD) accumAnalog(aXPos, hx) else if (hx < -STICK_DEAD) accumAnalog(aXNeg, -hx)
                    if (hy > STICK_DEAD) accumAnalog(aYPos, hy) else if (hy < -STICK_DEAD) accumAnalog(aYNeg, -hy)
                }
            }
            ControllerMappings.StickMode.FACE -> {
                sendAxisDigital(vx, posCode = 97, negCode = 99, port = port)  // Circle / Square (right/left)
                sendAxisDigital(vy, posCode = 96, negCode = 100, port = port) // Cross / Triangle (down/up)
            }
            ControllerMappings.StickMode.DPAD -> {
                // "Stick as D-pad" preset (opt-in; the nightly's default for Joy-Cons).
                // The bit-writes happen in dispatchDpadCombined — the single, change-
                // tracked d-pad owner, keyed off stickModeFor(...)==DPAD — so a DPAD
                // stick and the physical HAT can never release each other. The swap/
                // invert correction above still applied; dispatchDpadCombined re-reads
                // the raw axes for the fold. Nothing to emit here by design.
            }
            ControllerMappings.StickMode.CUSTOM -> {
                // Each direction is bound to any PS2 button (per-player). D-pad targets
                // (19-22) are owned by dispatchDpadCombined() (avoids the release race);
                // emitCustom keeps analog targets proportional, others thresholded.
                emitCustom(ControllerMappings.customStickCode(leftStick, ControllerMappings.StickDir.RIGHT, port),
                    if (vx > 0f) vx else 0f, port, leftStick)
                emitCustom(ControllerMappings.customStickCode(leftStick, ControllerMappings.StickDir.LEFT, port),
                    if (vx < 0f) -vx else 0f, port, leftStick)
                emitCustom(ControllerMappings.customStickCode(leftStick, ControllerMappings.StickDir.DOWN, port),
                    if (vy > 0f) vy else 0f, port, leftStick)
                emitCustom(ControllerMappings.customStickCode(leftStick, ControllerMappings.StickDir.UP, port),
                    if (vy < 0f) -vy else 0f, port, leftStick)
            }
        }
    }

    // CUSTOM stick directions bound to an ARMSX2 hotkey are edge-triggered: this tracks
    // which hotkey codes are currently held past the threshold, per port, so each
    // crossing fires exactly once (re-armed on release).
    private val stickHotkeyHeld = Array(8) { HashSet<Int>() } // per unified pad slot (multitap)

    /** Fire any SysHotkey bound (Hotkeys tab) to a stick DIRECTION, edge-triggered. The
     *  stick still drives the pad, so this is meant for sticks/directions a game doesn't
     *  use. Reuses [stickHotkeyHeld] — the reserved 1000+ stick-hotkey keycodes don't
     *  collide with the Custom-mode 300+ codes also tracked there. */
    private fun fireStickHotkeys(ev: MotionEvent, port: Int) {
        fireStickHotkeyAxis(ev, MotionEvent.AXIS_X, MotionEvent.AXIS_Y, true, port)
        val (hkRightX, hkRightY) = rightStickAxes(ev.deviceId)
        fireStickHotkeyAxis(ev, hkRightX, hkRightY, false, port)
    }
    private fun fireStickHotkeyAxis(ev: MotionEvent, axisX: Int, axisY: Int, left: Boolean, port: Int) {
        val x = ev.getAxisValue(axisX)
        val y = ev.getAxisValue(axisY)
        val held = stickHotkeyHeld[port]
        val dirs = arrayOf(
            ControllerMappings.StickDir.UP to -y, ControllerMappings.StickDir.DOWN to y,
            ControllerMappings.StickDir.LEFT to -x, ControllerMappings.StickDir.RIGHT to x,
        )
        for ((dir, value) in dirs) {
            val code = ControllerMappings.stickHotkeyKeyCode(left, dir)
            if (value > STICK_DIGITAL_THRESHOLD) {
                // Mirror the held direction into heldKeys so it can serve as the
                // MODIFIER of a stick+button combo hotkey (dispatchKeyEvent's
                // matchHotkey consults heldKeys when the button arrives).
                heldKeys.add(code)
                if (held.add(code)) {
                    // Edge: fire a hotkey with this direction as its MAIN key —
                    // combo-aware (e.g. "hold Select + push R-Stick Up"), falling
                    // back to a plain single-direction binding.
                    ControllerMappings.matchHotkey(code, heldKeys)?.let { runEdgeHotkey(it) }
                }
            } else {
                heldKeys.remove(code)
                held.remove(code)
            }
        }
    }

    /** Fire an ARMSX2 hotkey from a non-key source (a stick direction or a trigger crossing
     *  its threshold — edge-triggered, treated as a single press). Hold-type hotkeys
     *  (FAST_FORWARD hold, PRESSURE_MOD) are no-ops here: a stick edge has no hold semantics,
     *  and sendTrigger handles them itself on both edges. The rest mirror the one-shot
     *  actions in dispatchKeyEvent. */
    private fun runEdgeHotkey(h: ControllerMappings.SysHotkey) {
        when (h) {
            ControllerMappings.SysHotkey.MENU -> InGameOverlay.toggle()
            ControllerMappings.SysHotkey.SCREENSHOT -> com.armsx2.Screenshots.capture(applicationContext)
            ControllerMappings.SysHotkey.SAVE_STATE -> {
                val slot = currentSaveSlot.value
                kotlin.concurrent.thread { runCatching { NativeApp.saveStateToSlot(slot) } }
            }
            ControllerMappings.SysHotkey.LOAD_STATE -> {
                val slot = currentSaveSlot.value
                kotlin.concurrent.thread { runCatching { NativeApp.loadStateFromSlot(slot) } }
            }
            ControllerMappings.SysHotkey.CYCLE_SLOT -> cycleSaveSlot()
            ControllerMappings.SysHotkey.TEXTURE_DUMP -> {
                val on = runCatching { NativeApp.toggleTextureDumping() }.getOrDefault(false)
                android.widget.Toast.makeText(this,
                    if (on) "Texture dumping ON" else "Texture dumping OFF",
                    android.widget.Toast.LENGTH_SHORT).show()
            }
            ControllerMappings.SysHotkey.FAST_FORWARD_TOGGLE -> {
                fastForwardToggleActive = !fastForwardToggleActive
                val on = fastForwardToggleActive
                runCatching { NativeApp.speedhackLimitermode(if (on) ffLimiterMode() else baseLimiterMode()) }
                hotkeyToast(if (on) "Fast Forward ON" else "Fast Forward OFF")
            }
            ControllerMappings.SysHotkey.GYRO_TOGGLE -> toggleGyro()
            // GYRO_HOLD needs key up/down edges, which this edge-triggered path (stick
            // directions / combos) doesn't provide — behave as a toggle here rather than
            // latching gyro on with no release.
            ControllerMappings.SysHotkey.GYRO_HOLD -> toggleGyro()
            ControllerMappings.SysHotkey.GYRO_RECENTER -> recenterGyro()
            ControllerMappings.SysHotkey.RES_UP -> stepResolution(1)
            ControllerMappings.SysHotkey.RES_DOWN -> stepResolution(-1)
            ControllerMappings.SysHotkey.ACHIEVEMENTS -> com.armsx2.ui.emulation.EmulationMenuInputController.open(com.armsx2.ui.emulation.EmulationMenuTab.Options)
            ControllerMappings.SysHotkey.CLOSE_GAME -> closeGame()
            ControllerMappings.SysHotkey.QUIT_APP -> { quitAfterStop = true; stop()
            }
            ControllerMappings.SysHotkey.SAVE_AND_EXIT -> closeGame(saveAutosave = true)
            ControllerMappings.SysHotkey.RESET_GAME -> restart()
            ControllerMappings.SysHotkey.SLOW_DOWN -> toggleSlowDown()
            ControllerMappings.SysHotkey.TOGGLE_OSD -> hotkeyToast(InGameOverlay.cycleOsd())
            ControllerMappings.SysHotkey.TOGGLE_KEYBOARD -> toggleSoftKeyboard()
            // Hold-type hotkeys have no one-shot stick-edge meaning.
            ControllerMappings.SysHotkey.FAST_FORWARD,
            ControllerMappings.SysHotkey.PRESSURE_MOD -> {}
        }
    }

    /** Emit one CUSTOM stick-direction binding given its 0..1 deflection [mag]
     *  toward that direction. D-pad codes (19-22) are skipped — dispatchDpadCombined
     *  owns them; analog codes (110-123) stay proportional; others are thresholded. */
    private fun emitCustom(code: Int, mag: Float, port: Int, srcLeft: Boolean) {
        // Bound to an ARMSX2 hotkey? Edge-trigger it (fire once on threshold crossing,
        // re-arm on release) instead of sending a PS2 button.
        ControllerMappings.hotkeyForStickCode(code)?.let { hk ->
            val held = stickHotkeyHeld[port]
            if (mag > STICK_DIGITAL_THRESHOLD) {
                if (held.add(code)) runEdgeHotkey(hk)
            } else {
                held.remove(code)
            }
            return
        }
        if (code in 19..22) return
        if (code in 110..123) {
            // Analog target: shape with the SOURCE stick's feel settings (the stick
            // being physically moved), and contribute to the merge layer instead of
            // writing directly, so a CUSTOM direction can't fight the other stick's
            // ANALOG writer (or a trigger/button bound to the same direction).
            val m = shapeStickMag(mag, srcLeft)
            accumAnalog(code, m)
        } else {
            NativeApp.setPadButtonForPort(port, code, 32767, mag > STICK_DIGITAL_THRESHOLD)
        }
    }

    /** Stick-as-button: press [posCode] / [negCode] once the axis passes the digital
     *  threshold. setPadButton is a state set, so re-sending the same state is a no-op. */
    // [v] is the already-corrected axis value (swap/invert applied by dispatchStick).
    private fun sendAxisDigital(v: Float, posCode: Int, negCode: Int, port: Int) {
        NativeApp.setPadButtonForPort(port, posCode, 32767, v > STICK_DIGITAL_THRESHOLD)
        NativeApp.setPadButtonForPort(port, negCode, 32767, v < -STICK_DIGITAL_THRESHOLD)
    }

    // D-pad codes (19-22) THIS function last pressed, so it releases only its own
    // presses. Owns the D-pad from ALL non-KeyEvent sources: the physical HAT, a
    // stick in DPAD mode, and any CUSTOM stick direction bound to a D-pad code.
    // PER-PORT (index = player) so P1 and P2 D-pad presses can't release each other.
    private val dpadOwnHeld = Array(8) { HashSet<Int>() } // per unified pad slot (multitap)

    /** True when any CUSTOM-mode stick has a direction bound to a D-pad code, for
     *  the given player. */
    private fun customTargetsDpad(port: Int): Boolean {
        for (isLeft in booleanArrayOf(true, false)) {
            if (ControllerMappings.stickModeFor(isLeft, port) != ControllerMappings.StickMode.CUSTOM) continue
            for (dir in ControllerMappings.StickDir.values())
                if (ControllerMappings.customStickCode(isLeft, dir, port) in 19..22) return true
        }
        return false
    }

    /** Drive the PS2 D-pad from every non-KeyEvent source that can map to it — the
     *  physical HAT, a stick in DPAD mode, and CUSTOM directions bound to a D-pad
     *  code — through ONE change-tracked owner. Writing all four codes every event
     *  (the old approach) released a held direction whenever the stick re-centered
     *  or another stick emitted an event; tracking our own presses avoids that and
     *  never clobbers a physical D-pad arriving as KeyEvents. */
    private fun dispatchDpadCombined(ev: MotionEvent, port: Int) {
        val held = dpadOwnHeld[port]
        // When the D-pad drives the left stick, the HAT is folded into the stick
        // in dispatchStick — ignore it here so it doesn't ALSO press the d-pad.
        val dpadAsStick = ControllerMappings.dpadAsLeftStick()
        val hatX = if (dpadAsStick) 0f else ev.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hatY = if (dpadAsStick) 0f else ev.getAxisValue(MotionEvent.AXIS_HAT_Y)
        val hatActive = hatX != 0f || hatY != 0f
        // "Stick as D-pad" preset (StickMode.DPAD, opt-in): a stick in DPAD mode drives
        // the PS2 d-pad through THIS single change-tracked owner (folded via foldStick
        // below) so it can never release — or be released by — the physical HAT. Left
        // stick = AXIS_X/Y, right = AXIS_Z/RZ. dispatchStick's DPAD branch is a no-op by
        // design so there is exactly one writer. The combined owner also still handles
        // the physical HAT and CUSTOM directions bound to a d-pad code.
        val leftDpad = ControllerMappings.stickModeFor(true, port) == ControllerMappings.StickMode.DPAD
        val rightDpad = ControllerMappings.stickModeFor(false, port) == ControllerMappings.StickMode.DPAD
        // Nothing we own could be active → release what we hold and bail, so we
        // never touch the D-pad bits a KeyEvent-style physical D-pad drives.
        if (!hatActive && !leftDpad && !rightDpad && !customTargetsDpad(port)) {
            if (held.isNotEmpty()) {
                held.forEach { NativeApp.setPadButtonForPort(port, it, 0, false) }
                held.clear()
            }
            return
        }
        // HAT → D-pad only when that direction is still bound. The physical HAT never
        // flows through the keycode binding path (it arrives as a motion axis), so
        // clearing/reassigning a D-pad direction in the Pad tab was previously ignored
        // here. The custom-stick→D-pad fold below is a SEPARATE binding and stays
        // active even when the physical D-pad is cleared.
        val dpRightBound = ControllerMappings.targetForPhysical(KeyEvent.KEYCODE_DPAD_RIGHT, port) != null
        val dpLeftBound = ControllerMappings.targetForPhysical(KeyEvent.KEYCODE_DPAD_LEFT, port) != null
        val dpDownBound = ControllerMappings.targetForPhysical(KeyEvent.KEYCODE_DPAD_DOWN, port) != null
        val dpUpBound = ControllerMappings.targetForPhysical(KeyEvent.KEYCODE_DPAD_UP, port) != null
        var right = hatX > 0.5f && dpRightBound
        var left = hatX < -0.5f && dpLeftBound
        var down = hatY > 0.5f && dpDownBound
        var up = hatY < -0.5f && dpUpBound

        fun foldStick(axisX: Int, axisY: Int) {
            val x = ev.getAxisValue(axisX)
            val y = ev.getAxisValue(axisY)
            right = right || x > STICK_DIGITAL_THRESHOLD
            left = left || x < -STICK_DIGITAL_THRESHOLD
            down = down || y > STICK_DIGITAL_THRESHOLD
            up = up || y < -STICK_DIGITAL_THRESHOLD
        }
        val (foldRightX, foldRightY) = rightStickAxes(ev.deviceId)
        if (leftDpad) foldStick(MotionEvent.AXIS_X, MotionEvent.AXIS_Y)
        if (rightDpad) foldStick(foldRightX, foldRightY)

        // Fold CUSTOM directions that target a D-pad code so they share this owner.
        fun foldCustom(isLeft: Boolean, axisX: Int, axisY: Int) {
            if (ControllerMappings.stickModeFor(isLeft, port) != ControllerMappings.StickMode.CUSTOM) return
            val x = ev.getAxisValue(axisX)
            val y = ev.getAxisValue(axisY)
            fun mark(dir: ControllerMappings.StickDir, active: Boolean) {
                if (!active) return
                when (ControllerMappings.customStickCode(isLeft, dir, port)) {
                    22 -> right = true
                    21 -> left = true
                    20 -> down = true
                    19 -> up = true
                }
            }
            mark(ControllerMappings.StickDir.RIGHT, x > STICK_DIGITAL_THRESHOLD)
            mark(ControllerMappings.StickDir.LEFT, x < -STICK_DIGITAL_THRESHOLD)
            mark(ControllerMappings.StickDir.DOWN, y > STICK_DIGITAL_THRESHOLD)
            mark(ControllerMappings.StickDir.UP, y < -STICK_DIGITAL_THRESHOLD)
        }
        foldCustom(true, MotionEvent.AXIS_X, MotionEvent.AXIS_Y)
        foldCustom(false, foldRightX, foldRightY)

        // Write only on change so a resting stick's motion stream can't re-release
        // a direction the physical D-pad is also holding.
        fun apply(code: Int, on: Boolean) {
            val was = held.contains(code)
            if (on == was) return
            NativeApp.setPadButtonForPort(port, code, if (on) 32767 else 0, on)
            if (on) held.add(code) else held.remove(code)
        }
        apply(22, right) // D-pad right
        apply(21, left)  // D-pad left
        apply(20, down)  // D-pad down
        apply(19, up)    // D-pad up
    }

    /** Does this device actually report the given motion axis? InputDevice.getDevice is a
     *  binder call and motion events arrive far too often to query per event, hence the cache. */
    private val axisPresenceCache = HashMap<Long, Boolean>()
    private fun deviceHasAxis(deviceId: Int, axis: Int): Boolean {
        if (axis < 0) return false
        return axisPresenceCache.getOrPut((deviceId.toLong() shl 32) or (axis.toLong() and 0xffffffffL)) {
            runCatching { InputDevice.getDevice(deviceId)?.getMotionRange(axis) }.getOrNull() != null
        }
    }

    // Triggers past TRIGGER_DIGITAL_THRESHOLD, per unified pad slot: edge state for
    // trigger-bound hotkeys, so each press fires once and re-arms on release.
    private val triggerHotkeyHeld = Array(8) { HashSet<Int>() }

    private fun sendTrigger(event: MotionEvent, left: Boolean, port: Int) {
        // -1 = no trigger axis on this side; its L2/R2 is a key event, key path owns it.
        val raw = triggerTravel(event, left)
        if (raw < 0f) return
        val code = triggerKeyCode(left)
        val held = triggerHotkeyHeld[port]
        val pressed = raw > TRIGGER_DIGITAL_THRESHOLD

        // Mirror into heldKeys so a held trigger can be a combo MODIFIER, exactly as it is on a
        // pad whose triggers send key events. Cleared on OUR release edge only — a pad that
        // reports its triggers both ways must not have the key path's hold wiped by a motion
        // event that happens to read the axis low.
        if (pressed) heldKeys.add(code)
        if (pressed != held.contains(code)) {
            if (pressed) held.add(code) else { held.remove(code); heldKeys.remove(code) }
            // Triggers now reach the Hotkeys tab's capture like any other button, so they have
            // to be able to fire one here. Hold-type hotkeys act on both edges (a trigger has a
            // real release, unlike a stick edge); the rest fire on the press. Matching on
            // release re-adds the code, as the key path does, so a combo still resolves.
            ControllerMappings.matchHotkey(code, if (pressed) heldKeys else heldKeys + code)?.let { hk ->
                when (hk) {
                    ControllerMappings.SysHotkey.FAST_FORWARD -> {
                        if (pressed) fastForwardToggleActive = false
                        runCatching {
                            NativeApp.speedhackLimitermode(if (pressed) ffLimiterMode() else baseLimiterMode())
                        }
                    }
                    ControllerMappings.SysHotkey.PRESSURE_MOD ->
                        com.armsx2.ui.touch.TouchControls.pressureModifierHeld.value = pressed
                    ControllerMappings.SysHotkey.GYRO_HOLD -> gyroActive.value = pressed
                    else -> if (pressed) runEdgeHotkey(hk)
                }
            }
            // Macros are keyed on the physical code too, and the Pad tab now lets a trigger be
            // captured for one. Same both-edges firing as dispatchGameplayKey.
            com.armsx2.ui.touch.TouchControls.macroForPhysicalCode(code)?.let { macro ->
                com.armsx2.ui.touch.TouchControls.fireMacro(macro, "pad$port", pressed) { c, p ->
                    sendKeyAction(if (p) KeyEventType.KeyDown else KeyEventType.KeyUp, c, port)
                }
            }
        }
        // A trigger bound to a hotkey or a macro doesn't also drive the pad — the precedence
        // the key path and emitCustom already apply. The hotkey match is combo-aware, so a
        // trigger that is merely a MODIFIER keeps working as L2/R2.
        if (ControllerMappings.matchHotkey(code, heldKeys) != null) return
        if (com.armsx2.ui.touch.TouchControls.macroForPhysicalCode(code) != null) return

        // Honor the L2/R2 binding: triggers arrive as motion axes, never through the
        // keycode binding path, so clearing/remapping them in the Pad tab was ignored.
        // Resolve the physical trigger keycode to its mapped PS2 target — null = cleared,
        // so the trigger is disabled; otherwise drive the resolved (possibly remapped) code.
        val target = ControllerMappings.targetForPhysical(code, port) ?: return
        // Deadzone off the bottom, re-normalized, so pressure ramps from zero instead of
        // flicking on/off at a hard threshold (the jitter non-Xbox pads showed).
        val out = if (raw <= TRIGGER_DEAD) 0f else (raw - TRIGGER_DEAD) / (1f - TRIGGER_DEAD)
        if (target in 110..123) {
            // Trigger bound to a PS2 STICK direction ("(send)" rows): contribute the
            // proportional pressure to the merge layer so it can't be released by
            // the target stick's own (resting) ANALOG writer in the same event.
            accumAnalog(target, out)
        } else {
            NativeApp.setPadButtonForPort(port, target, (out * 32767).toInt(), out > 0f)
        }
    }

    /** Set in onPause when the screen goes off (a real sleep), consumed in onResume so the sleep
     *  chime is paired with a wake chime + a brief "Welcome Back!" — never on a plain background. */
    private var wasAsleep = false

    /** Frees a pad's slot in PadRouter when its controller is unplugged / power-cycled, so a
     *  re-enumerated device (a NEW deviceId after AYANEO sleep/wake) re-claims Player 1 instead of
     *  the stale dead id owning it and shunting gameplay to an un-armed pad (#394). Registered in
     *  onCreate and kept alive across pause so the wake-time remove/add is caught. */
    private val inputDeviceListener = object : android.hardware.input.InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) {}
        override fun onInputDeviceChanged(deviceId: Int) {}
        override fun onInputDeviceRemoved(deviceId: Int) {
            com.armsx2.input.PadRouter.forgetDevice(deviceId)
        }
    }

    override fun onPause() {
        // Take the second-display panel down with the app. A Presentation is not torn down by the
        // activity stopping, so it otherwise stayed on the external screen while the user was off
        // doing something else (reported).
        runCatching { com.armsx2.SecondScreen.setForeground(applicationContext, false) }
        // DS-lid-style chime when the SCREEN is going off (device sleeping) — gated on isInteractive
        // so a plain background (home / recents, screen still on) stays silent. Fires before we pause
        // audio below so the blip is heard as the device sleeps.
        if ((getSystemService(android.content.Context.POWER_SERVICE) as? android.os.PowerManager)?.isInteractive == false) {
            wasAsleep = true
            com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.SLEEP)
        }
        com.armsx2.navigation.UiNavigator.drawerOpen.value = false
        // Mark backgrounded BEFORE opening the overlay below. open() sets overlayVisible = true,
        // which re-fires the pause-music LaunchedEffect — and without this flag already false, that
        // effect would call start() and play the track on the OS home screen. setForeground(false)
        // also pauses whatever is currently playing.
        com.armsx2.PauseMusic.setForeground(false)
        // Leaving the app (home / recents / slide-out) while a game is running:
        // open the pause OVERLAY instead of a silent pause. A bare pause left
        // users staring at a frozen game with no obvious way back — they had to
        // know to open the menu and tap Resume. open() pauses the VM AND shows
        // the pause menu, so returning lands straight on the Resume button.
        // No-op if the overlay is already up (it already paused the game).
        if (eState.value == EmuState.RUNNING)
            InGameOverlay.open()
        // Persist Vulkan pipeline cache before Android can reap the process.
        // ~VKShaderCache only fires on a clean device teardown, but swipe-kill
        // / OOM-kill skip that path — every cold launch would otherwise
        // re-compile every TFX pipeline from scratch. No-op on OpenGL.
        NativeApp.flushShaderCache()
        // PGO instrument build: flush profile counters so a profiling run survives
        // an Android process kill. No-op in normal builds.
        runCatching { NativeApp.dumpPgoProfile() }
        // Library music must not keep playing out of a backgrounded app — it would sound
        // like the emulator ignoring the home button. Paused, not stopped, so returning
        // to the library picks it back up.
        com.armsx2.LibraryMusic.pause()
        // Pause-menu track was already paused by setForeground(false) at the top of onPause.
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        runCatching { com.armsx2.SecondScreen.setForeground(applicationContext, true) }
        // Woke from a real sleep (paired with the onPause sleep chime): play the wake chime + a brief
        // top-left "Welcome Back!". A plain background return never set wasAsleep, so this only fires
        // after an actual screen-off sleep.
        if (wasAsleep) {
            wasAsleep = false
            com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.WAKE)
            if (prefs.getBoolean("ui.hotkeyToasts", true))
                com.armsx2.ui.WelcomeBanner.show(com.armsx2.i18n.I18n.get("app.welcomeBack"))
        }
        // #394 backstop: a controller that slept and woke returns with a NEW deviceId, so drop any
        // pad slot whose device is no longer connected — the re-enumerated pad then re-claims Player 1
        // rather than the dead id silently owning it. (inputDeviceListener catches the live remove;
        // this covers a remove that landed while we were paused / was never delivered.)
        com.armsx2.input.PadRouter.pruneStale(android.view.InputDevice.getDeviceIds())
        // Returning to the foreground: call start(), not resume(). A PERMANENT audio-focus
        // loss (another media app took over — YouTube, iiSU) releases our player entirely,
        // and resume() only un-pauses an existing player, so the music stayed dead until a
        // full app restart (#398-adjacent report). start() rebuilds a released player — and
        // still just un-pauses a merely-paused one — while its own guards keep it a no-op
        // when the setting is off, a VM is running, or that other app is still playing.
        com.armsx2.LibraryMusic.start(this)
        // Back in the foreground: clear the background guard first, THEN restart the pause track if a
        // menu is still up. The LaunchedEffect won't do it — the overlay states didn't change while
        // we were away, so it never re-runs — and start() no-ops until foreground is true again.
        com.armsx2.PauseMusic.setForeground(true)
        if (WindowImpl.overlayVisible.value || WindowImpl.inGameScreen.value != null) {
            com.armsx2.PauseMusic.start(this)
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleExternalLaunchIntent(intent)
    }

    override fun onDestroy() {
        getSystemService(android.hardware.input.InputManager::class.java)
            ?.unregisterInputDeviceListener(inputDeviceListener)
        // On a CONFIGURATION-driven recreate (e.g. Samsung DeX moving the activity to
        // an external display, density/uiMode change) Android destroys+recreates us.
        // Do NOT tear down the native VM or hard-kill the process then — that races the
        // recreate and crashes ("this app has a bug"). Only shut down on a real finish.
        if (isChangingConfigurations()) {
            super.onDestroy()
            return
        }
        // Real finish only — a configuration recreate must NOT tear the second-display panel
        // down (it would flicker away and rebuild on every rotation/density change).
        runCatching { com.armsx2.SecondScreen.release(applicationContext) }
        NativeApp.shutdown()
        super.onDestroy()

        val appPid = Process.myPid()
        Process.killProcess(appPid)
    }

    private fun handleExternalLaunchIntent(intent: Intent?) {
        val raw = extractLaunchUri(intent) ?: return
        persistReadGrant(intent, raw)
        // Frontends (Cocoon/Daijisho/ES-DE) list the .cue, since that's the canonical disc
        // descriptor for a cue+bin rip — but the core has no cue parser and .cue isn't in its
        // disc whitelist (VMManager::IsDiscFileName), so booting one fails outright. Resolve
        // the cue's first FILE "<name>" BINARY entry to its sibling track and launch that.
        // Falls back to the original URI whenever anything fails, so a launch that already
        // worked (.iso/.bin/.chd) can never be made worse by this.
        val uri = resolveCueToTrack(raw) ?: raw
        currentGame.value = null
        pendingExternalLaunch.value = uri.toString()
        launchPendingExternalGameIfReady()
    }

    /** Maps a `.cue` sheet to the track file it points at. Returns null for anything that
     *  isn't a resolvable cue, so the caller keeps the original URI. */
    private fun resolveCueToTrack(cue: Uri): Uri? = runCatching {
        val label = (cue.lastPathSegment ?: cue.path).orEmpty()
        if (!label.endsWith(".cue", ignoreCase = true)) return null
        val text = readBounded(cue) ?: return null
        // FILE "Game.bin" BINARY  — the name may also be unquoted. Strip any directory part;
        // a cue always references tracks sitting beside it.
        val m = Regex("""(?im)^\s*FILE\s+(?:"([^"]+)"|(\S+))""").find(text) ?: return null
        val track = (m.groupValues[1].takeIf(String::isNotBlank) ?: m.groupValues[2])
            .trim().substringAfterLast('/').substringAfterLast('\\')
        if (track.isBlank()) return null
        siblingOf(cue, track)
    }.getOrNull()

    /** Bounded read — cue sheets are a few hundred bytes, so never slurp an arbitrary file. */
    private fun readBounded(uri: Uri, limit: Int = 65536): String? = runCatching {
        val stream = if (uri.scheme == "content") contentResolver.openInputStream(uri)
        else uri.path?.let { java.io.File(it).takeIf(java.io.File::isFile)?.inputStream() }
        stream?.use { s ->
            val buf = ByteArray(limit)
            var n = 0
            while (n < limit) {
                val r = s.read(buf, n, limit - n)
                if (r <= 0) break
                n += r
            }
            String(buf, 0, n)
        }
    }.getOrNull()

    /** Sibling file alongside [origin]. Raw/file paths resolve directly (we hold all-files
     *  access on the sideload build); a content:// URI only resolves when it carries a parent
     *  — a single-document grant from a frontend does not, so we return null and fall back. */
    private fun siblingOf(origin: Uri, fileName: String): Uri? = runCatching {
        if (origin.scheme == null || origin.scheme == "file") {
            val parent = origin.path?.let { java.io.File(it).parentFile } ?: return null
            java.io.File(parent, fileName).takeIf { it.isFile }?.absolutePath?.toUri()
        } else {
            androidx.documentfile.provider.DocumentFile.fromSingleUri(this, origin)
                ?.parentFile?.findFile(fileName)?.takeIf { it.isFile }?.uri
        }
    }.getOrNull()

    private fun extractLaunchUri(intent: Intent?): Uri? {
        if (intent == null)
            return null

        intent.data?.let { return it }

        val stream: Uri? = if (Build.VERSION.SDK_INT >= 33) {
            intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(Intent.EXTRA_STREAM) as? Uri
        }
        stream?.let { return it }

        intent.clipData?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.uri?.let { return it }

        for (key in listOf("path", "game", "rom", "uri", "android.intent.extra.STREAM")) {
            val value = intent.getStringExtra(key)?.takeIf { it.isNotBlank() } ?: continue
            return value.toUri()
        }

        return null
    }

    private fun persistReadGrant(intent: Intent?, uri: Uri) {
        if (uri.scheme != "content" || intent == null)
            return

        val flags = intent.flags
        if ((flags and Intent.FLAG_GRANT_READ_URI_PERMISSION) == 0 ||
            (flags and Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION) == 0)
            return

        runCatching {
            contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION,
            )
        }
    }
}
