package com.armsx2.fork

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import android.os.HardwarePropertiesManager
import android.os.PowerManager
import android.os.Process
import android.os.SystemClock
import android.util.Log
import com.armsx2.BuildConfig
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File
import java.util.Locale
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

/**
 * Telemetria ambiental Android-only para sessões manuais em hardware real.
 *
 * O Android não oferece clocks de CPU/GPU por uma API pública e uniforme. Por isso as leituras
 * de sysfs são estritamente best-effort: um aparelho que bloqueia um nó registra `unavailable`,
 * nunca zero. O estado térmico público e a temperatura da bateria continuam disponíveis como
 * fallback sem permissões privilegiadas.
 *
 * A coleta roda fora do caminho por frame, em uma única thread de prioridade baixa. O token de
 * geração impede que uma amostra que estava lendo sysfs publique no emulog depois de [stop].
 */
object AndroidEnvironmentTelemetry {
    internal data class Snapshot(
        val thermalStatus: String,
        val thermalHeadroom: String,
        val batteryC: String,
        val maxC: String,
        val sensorsC: Map<String, String>,
        val cpuKHz: Map<String, String>,
        val devfreqHz: Map<String, String>,
    )

    private const val TAG = "ARMSX2_ENV"
    private const val SAMPLE_INTERVAL_SECONDS = 10L
    private val lock = Any()

    private var executor: ScheduledExecutorService? = null
    private var generation = 0L
    private var lastSnapshot: Snapshot? = null
    private var lastSampleElapsedMs = 0L

    fun start(context: Context) {
        // Telemetria de qualificação, não comportamento de produção: releases permanecem sem a
        // thread e sem o I/O de sysfs. O APK github/debug entregue aos testadores entra aqui.
        if (!BuildConfig.DEBUG) return
        val appContext = context.applicationContext
        synchronized(lock) {
            if (executor != null) return

            val token = ++generation
            lastSnapshot = null
            lastSampleElapsedMs = 0L
            val created = Executors.newSingleThreadScheduledExecutor { runnable ->
                Thread({
                    runCatching { Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND) }
                    runnable.run()
                }, "ForkEnvironmentTelemetry").apply { isDaemon = true }
            }
            executor = created
            created.scheduleWithFixedDelay(
                { collectAndEmit(appContext, token) },
                0L,
                SAMPLE_INTERVAL_SECONDS,
                TimeUnit.SECONDS,
            )
        }
    }

    /** Para a thread antes do teardown nativo e grava a última leitura sem tocar novamente em sysfs. */
    fun stop() {
        synchronized(lock) {
            val running = executor ?: return
            ++generation
            executor = null
            running.shutdownNow()

            lastSnapshot?.let { snapshot ->
                val age = (SystemClock.elapsedRealtime() - lastSampleElapsedMs).coerceAtLeast(0L)
                emit(format(snapshot, phase = "stop", ageMs = age))
            }
            lastSnapshot = null
            lastSampleElapsedMs = 0L
        }
    }

    private fun collectAndEmit(context: Context, token: Long) {
        val snapshot = runCatching { collect(context) }.getOrElse {
            Snapshot(
                thermalStatus = "unavailable",
                thermalHeadroom = "unavailable",
                batteryC = "unavailable",
                maxC = "unavailable",
                sensorsC = emptyMap(),
                cpuKHz = emptyMap(),
                devfreqHz = emptyMap(),
            )
        }
        synchronized(lock) {
            if (executor == null || token != generation) return
            lastSnapshot = snapshot
            lastSampleElapsedMs = SystemClock.elapsedRealtime()
            emit(format(snapshot, phase = "periodic", ageMs = 0L))
        }
    }

    private fun emit(line: String) {
        Log.i(TAG, line)
        runCatching { NativeApp.emulog(line) }
    }

    private fun collect(context: Context): Snapshot {
        val sensors = linkedMapOf<String, String>()
        collectThermalZones(sensors)
        collectHardwareTemperatures(context, sensors)

        val battery = readBatteryTemperature(context)
        val numericTemperatures = sensors.values.mapNotNull { it.toDoubleOrNull() }.toMutableList()
        battery.toDoubleOrNull()?.let(numericTemperatures::add)
        val max = numericTemperatures.maxOrNull()?.let(::formatDecimal) ?: "unavailable"

        return Snapshot(
            thermalStatus = readThermalStatus(context),
            thermalHeadroom = readThermalHeadroom(context),
            batteryC = battery,
            maxC = max,
            sensorsC = sensors,
            cpuKHz = collectCpuClocks(),
            devfreqHz = collectDeviceClocks(),
        )
    }

    private fun collectThermalZones(output: MutableMap<String, String>) {
        val zones = File("/sys/class/thermal").listFiles()
            ?.filter { it.name.startsWith("thermal_zone") }
            ?.sortedBy { it.name }
            .orEmpty()
        for (zone in zones) {
            val value = normalizeTemperature(readText(File(zone, "temp"))) ?: continue
            val rawName = readText(File(zone, "type")) ?: zone.name
            putUnique(output, sanitize(rawName), formatDecimal(value))
        }
    }

    private fun collectHardwareTemperatures(context: Context, output: MutableMap<String, String>) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) return
        val manager = context.getSystemService(HardwarePropertiesManager::class.java) ?: return
        val groups = listOf(
            "api_cpu" to HardwarePropertiesManager.DEVICE_TEMPERATURE_CPU,
            "api_gpu" to HardwarePropertiesManager.DEVICE_TEMPERATURE_GPU,
            "api_battery" to HardwarePropertiesManager.DEVICE_TEMPERATURE_BATTERY,
            "api_skin" to HardwarePropertiesManager.DEVICE_TEMPERATURE_SKIN,
        )
        for ((name, type) in groups) {
            val values = runCatching {
                manager.getDeviceTemperatures(type, HardwarePropertiesManager.TEMPERATURE_CURRENT)
            }.getOrNull() ?: continue
            values.forEachIndexed { index, value ->
                if (value.isFinite() && value in -40.0f..200.0f)
                    putUnique(output, "${name}_$index", formatDecimal(value.toDouble()))
            }
        }
    }

    private fun readBatteryTemperature(context: Context): String {
        val intent = runCatching {
            context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        }.getOrNull() ?: return "unavailable"
        val raw = intent.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
        if (raw == Int.MIN_VALUE || raw == 0) return "unavailable"
        val value = raw / 10.0
        return if (value in -40.0..200.0) formatDecimal(value) else "unavailable"
    }

    private fun readThermalStatus(context: Context): String {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return "unavailable"
        val manager = context.getSystemService(PowerManager::class.java) ?: return "unavailable"
        val status = runCatching { manager.currentThermalStatus }.getOrNull() ?: return "unavailable"
        val name = when (status) {
            PowerManager.THERMAL_STATUS_NONE -> "none"
            PowerManager.THERMAL_STATUS_LIGHT -> "light"
            PowerManager.THERMAL_STATUS_MODERATE -> "moderate"
            PowerManager.THERMAL_STATUS_SEVERE -> "severe"
            PowerManager.THERMAL_STATUS_CRITICAL -> "critical"
            PowerManager.THERMAL_STATUS_EMERGENCY -> "emergency"
            PowerManager.THERMAL_STATUS_SHUTDOWN -> "shutdown"
            else -> "unknown"
        }
        return "$name($status)"
    }

    private fun readThermalHeadroom(context: Context): String {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return "unavailable"
        val manager = context.getSystemService(PowerManager::class.java) ?: return "unavailable"
        val value = runCatching { manager.getThermalHeadroom(0) }.getOrNull()
        return if (value != null && value.isFinite()) formatDecimal(value.toDouble()) else "unavailable"
    }

    private fun collectCpuClocks(): Map<String, String> {
        val result = linkedMapOf<String, String>()
        val policies = File("/sys/devices/system/cpu/cpufreq").listFiles()
            ?.filter { it.name.startsWith("policy") }
            ?.sortedBy { it.name }
            .orEmpty()
        for (policy in policies) {
            val current = readUnsigned(File(policy, "scaling_cur_freq"))
            val maximum = readUnsigned(File(policy, "cpuinfo_max_freq"))
                ?: readUnsigned(File(policy, "scaling_max_freq"))
            if (current != null || maximum != null)
                result[policy.name] = "${current ?: "unavailable"}/${maximum ?: "unavailable"}"
        }
        if (result.isNotEmpty()) return result

        val cpus = File("/sys/devices/system/cpu").listFiles()
            ?.filter { it.name.matches(Regex("cpu\\d+")) }
            ?.sortedBy { it.name }
            .orEmpty()
        for (cpu in cpus) {
            val dir = File(cpu, "cpufreq")
            val current = readUnsigned(File(dir, "scaling_cur_freq")) ?: continue
            val maximum = readUnsigned(File(dir, "cpuinfo_max_freq"))
                ?: readUnsigned(File(dir, "scaling_max_freq"))
            result[cpu.name] = "$current/${maximum ?: "unavailable"}"
        }
        return result
    }

    private fun collectDeviceClocks(): Map<String, String> {
        val result = linkedMapOf<String, String>()
        val devices = File("/sys/class/devfreq").listFiles()?.sortedBy { it.name }.orEmpty()
        for (device in devices) {
            val current = readUnsigned(File(device, "cur_freq"))
            val maximum = readUnsigned(File(device, "max_freq"))
            if (current != null || maximum != null)
                putUnique(result, sanitize(device.name), "${current ?: "unavailable"}/${maximum ?: "unavailable"}")
        }

        val kgsl = File("/sys/class/kgsl/kgsl-3d0")
        val gpuCurrent = readUnsigned(File(kgsl, "gpuclk"))
        val gpuMaximum = readUnsigned(File(kgsl, "max_gpuclk"))
        if (gpuCurrent != null || gpuMaximum != null)
            putUnique(result, "kgsl-3d0", "${gpuCurrent ?: "unavailable"}/${gpuMaximum ?: "unavailable"}")
        return result
    }

    internal fun normalizeTemperature(raw: String?): Double? {
        var value = raw?.trim()?.toDoubleOrNull() ?: return null
        if (!value.isFinite()) return null
        value = when {
            kotlin.math.abs(value) > 1_000.0 -> value / 1_000.0
            kotlin.math.abs(value) > 200.0 -> value / 10.0
            else -> value
        }
        return value.takeIf { it in -40.0..200.0 }
    }

    internal fun format(snapshot: Snapshot, phase: String, ageMs: Long): String = buildString {
        append("@@ANDROID_ENV@@ phase=").append(sanitize(phase))
        append(" age_ms=").append(ageMs.coerceAtLeast(0L))
        append(" thermal_status=").append(snapshot.thermalStatus)
        append(" thermal_headroom=").append(snapshot.thermalHeadroom)
        append(" battery_c=").append(snapshot.batteryC)
        append(" temp_max_c=").append(snapshot.maxC)
        append(" sensors_c=").append(formatMap(snapshot.sensorsC))
        append(" cpu_khz_cur_max=").append(formatMap(snapshot.cpuKHz))
        append(" devfreq_hz_cur_max=").append(formatMap(snapshot.devfreqHz))
    }

    private fun formatMap(values: Map<String, String>): String = if (values.isEmpty()) {
        "unavailable"
    } else {
        values.toSortedMap().entries.joinToString(",") { (key, value) ->
            "${sanitize(key)}:${sanitize(value)}"
        }
    }

    internal fun sanitize(value: String): String = value.trim().replace(Regex("[^A-Za-z0-9._/()+-]+"), "_")
        .ifEmpty { "unknown" }

    private fun formatDecimal(value: Double): String = String.format(Locale.US, "%.1f", value)

    private fun readText(file: File): String? = runCatching { file.readText().trim() }
        .getOrNull()?.takeIf(String::isNotEmpty)

    private fun readUnsigned(file: File): String? = readText(file)?.takeIf {
        it.all(Char::isDigit)
    }

    private fun <T> putUnique(output: MutableMap<String, T>, requestedKey: String, value: T) {
        var key = requestedKey.ifEmpty { "unknown" }
        var suffix = 2
        while (output.containsKey(key)) key = "${requestedKey}_$suffix".also { suffix++ }
        output[key] = value
    }
}
