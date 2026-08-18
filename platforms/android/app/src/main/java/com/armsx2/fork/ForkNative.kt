package com.armsx2.fork

import android.util.Log
import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONObject

/**
 * Lado Kotlin da porta única do fork (`pcsx2/Fork/ForkBridge.h`).
 *
 * Toda consulta passa por `NativeApp.forkQuery`, que devolve JSON. Consultas novas custam uma
 * linha aqui — nenhuma função JNI nova, nenhum arquivo do upstream editado de novo.
 */
object ForkNative {

    private const val TAG = "ForkNative"

    /** JSON da consulta, ou null quando a chamada nativa falhou (biblioteca ainda não carregada,
     *  build sem o módulo). Null é distinto de `ok=false`: um é "não consegui perguntar", o
     *  outro é "perguntei e a resposta foi não". */
    fun query(request: String): JSONObject? = runCatching {
        JSONObject(NativeApp.forkQuery(request))
    }.onFailure { Log.w(TAG, "forkQuery($request) falhou: ${it.message}") }.getOrNull()

    // ---- Driver ------------------------------------------------------------

    /** Resultado da inspeção de um .so de driver, antes de instalá-lo. */
    data class PackageInspection(
        val usable: Boolean,
        val verdict: String,
        val reason: String,
        val sha256: String,
        val sizeBytes: Long,
    )

    fun inspectDriver(libraryPath: String): PackageInspection? {
        val json = query("driver.inspect:$libraryPath") ?: return null
        return PackageInspection(
            usable = json.optBoolean("ok", false),
            verdict = json.optString("verdict"),
            reason = json.optString("reason"),
            sha256 = json.optString("sha256"),
            sizeBytes = json.optLong("size", 0L),
        )
    }

    /** O que está rodando de fato, depois que um renderer subiu. */
    data class DriverStatus(
        val probed: Boolean,
        val outcome: String,
        val reason: String,
        /** True quando o que roda não é o que foi pedido — o fallback silencioso. */
        val unexpected: Boolean,
        val activeDriver: String,
        val mesa: String,
        val vulkan: String,
        val gpu: String,
        val requested: String,
        val sha256: String,
        val error: String,
    )

    fun driverStatus(): DriverStatus? {
        val json = query("driver.status") ?: return null
        if (!json.optBoolean("ok", false)) return null
        return DriverStatus(
            probed = json.optBoolean("probed", false),
            outcome = json.optString("outcome"),
            reason = json.optString("reason"),
            unexpected = json.optBoolean("unexpected", false),
            activeDriver = json.optString("activeDriver"),
            mesa = json.optString("mesa"),
            vulkan = json.optString("vulkan"),
            gpu = json.optString("gpu"),
            requested = json.optString("requested"),
            sha256 = json.optString("sha256"),
            error = json.optString("error"),
        )
    }

    /** Linha pronta para o card de status: "Turnip · Mesa 25.2.0 · Vulkan 1.3.281". */
    fun DriverStatus.summaryLine(): String = buildList {
        if (activeDriver.isNotBlank()) add(activeDriver)
        if (mesa.isNotBlank()) add("Mesa $mesa")
        if (vulkan.isNotBlank()) add("Vulkan $vulkan")
    }.joinToString(" · ")

    // ---- Frame Generation --------------------------------------------------

    /**
     * Política de FG e o que ela decidiu no último quadro.
     *
     * [warning] vem do núcleo (`ForkFrameGen::USER_WARNING`) em vez de ser uma string da UI. A
     * diferença importa: uma cópia no frontend pode ser reescrita numa tradução mais otimista —
     * "FG aumenta o desempenho" — e é exatamente essa frase que o projeto proíbe. Se a ponte não
     * responder, a UI cai no texto embutido abaixo, que diz a mesma coisa; o que ela nunca faz é
     * mostrar a seção sem aviso nenhum.
     */
    data class FrameGenStatus(
        val mode: String,
        val warning: String,
        val state: String,
        val reason: String,
        val framesToGenerate: Int,
        val budgetMs: Double,
        val minRealFps: Double,
        val statusLine: String,
    )

    fun frameGenStatus(): FrameGenStatus? {
        val json = query("framegen.status") ?: return null
        if (!json.optBoolean("ok", false)) return null
        return FrameGenStatus(
            mode = json.optString("mode", "off"),
            warning = json.optString("warning"),
            state = json.optString("state"),
            reason = json.optString("reason"),
            framesToGenerate = json.optInt("framesToGenerate", 0),
            budgetMs = json.optDouble("budgetMs", 0.0),
            minRealFps = json.optDouble("minRealFps", 0.0),
            statusLine = json.optString("statusLine"),
        )
    }
}
