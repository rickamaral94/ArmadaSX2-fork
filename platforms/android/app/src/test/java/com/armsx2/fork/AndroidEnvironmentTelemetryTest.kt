package com.armsx2.fork

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class AndroidEnvironmentTelemetryTest {
    @Test
    fun normalizeTemperatureAcceptsCommonAndroidUnits() {
        assertEquals(42.0, AndroidEnvironmentTelemetry.normalizeTemperature("42000")!!, 0.001)
        assertEquals(42.0, AndroidEnvironmentTelemetry.normalizeTemperature("420")!!, 0.001)
        assertEquals(42.0, AndroidEnvironmentTelemetry.normalizeTemperature("42")!!, 0.001)
    }

    @Test
    fun normalizeTemperatureRejectsMissingAndImpossibleValues() {
        assertNull(AndroidEnvironmentTelemetry.normalizeTemperature(null))
        assertNull(AndroidEnvironmentTelemetry.normalizeTemperature("unavailable"))
        assertNull(AndroidEnvironmentTelemetry.normalizeTemperature("250000"))
    }

    @Test
    fun formatMakesUnavailableDataExplicit() {
        val line = AndroidEnvironmentTelemetry.format(
            AndroidEnvironmentTelemetry.Snapshot(
                thermalStatus = "none(0)",
                thermalHeadroom = "unavailable",
                batteryC = "unavailable",
                maxC = "unavailable",
                sensorsC = emptyMap(),
                cpuKHz = emptyMap(),
                devfreqHz = emptyMap(),
            ),
            phase = "periodic",
            ageMs = 0,
        )

        assertTrue(line.startsWith("@@ANDROID_ENV@@ phase=periodic age_ms=0"))
        assertTrue(line.contains("thermal_status=none(0)"))
        assertTrue(line.contains("sensors_c=unavailable"))
        assertTrue(line.contains("cpu_khz_cur_max=unavailable"))
        assertTrue(line.contains("devfreq_hz_cur_max=unavailable"))
    }

    @Test
    fun formatIsStableAndSanitizesSensorNames() {
        val line = AndroidEnvironmentTelemetry.format(
            AndroidEnvironmentTelemetry.Snapshot(
                thermalStatus = "light(1)",
                thermalHeadroom = "0.7",
                batteryC = "35.0",
                maxC = "67.5",
                sensorsC = linkedMapOf("GPU temperature" to "67.5", "cpu-0" to "61.0"),
                cpuKHz = linkedMapOf("policy7" to "2803200/3200000"),
                devfreqHz = linkedMapOf("kgsl 3d0" to "680000000/680000000"),
            ),
            phase = "stop",
            ageMs = 125,
        )

        assertTrue(line.contains("sensors_c=GPU_temperature:67.5,cpu-0:61.0"))
        assertTrue(line.contains("cpu_khz_cur_max=policy7:2803200/3200000"))
        assertTrue(line.contains("devfreq_hz_cur_max=kgsl_3d0:680000000/680000000"))
        assertTrue(line.contains("phase=stop age_ms=125"))
    }
}
