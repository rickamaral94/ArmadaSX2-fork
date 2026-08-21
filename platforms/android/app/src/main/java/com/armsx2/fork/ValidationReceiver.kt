package com.armsx2.fork

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.armsx2.BuildConfig
import com.armsx2.config.ConfigStore
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONObject
import java.security.MessageDigest
import kotlin.concurrent.thread

/**
 * Porta de automação da Fase B.
 *
 * O manifest exige android.permission.DUMP, que o shell do adb possui e aplicativos comuns não.
 * A lista de operações também é fechada: isto não é uma API pública nem um console arbitrário.
 */
class ValidationReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != "${context.packageName}.action.VALIDATION") return

        val pending = goAsync()
        thread(name = "ForkValidationReceiver") {
            val result = runCatching { dispatch(context, intent) }.getOrElse {
                errorJson("${it.javaClass.simpleName}: ${it.message ?: "unknown error"}")
            }
            pending.setResultCode(if (result.optBoolean("ok", false)) Activity.RESULT_OK else Activity.RESULT_CANCELED)
            pending.setResultData(result.toString())
            pending.finish()
        }
    }

    private fun dispatch(context: Context, intent: Intent): JSONObject {
        return when (val command = intent.getStringExtra(EXTRA_COMMAND).orEmpty()) {
            "status" -> environment(context)
            "query" -> query(intent.getStringExtra(EXTRA_REQUEST).orEmpty())
            "setting" -> setting(intent)
            "load-state" -> loadState(intent.getIntExtra(EXTRA_SLOT, -1))
            "pause" -> vmAction { MainActivityRuntime.pause() }
            "resume" -> vmAction { MainActivityRuntime.resume() }
            "stop" -> vmAction { MainActivityRuntime.stop(saveAutosave = false) }
            else -> errorJson("unsupported command: $command")
        }
    }

    private fun query(request: String): JSONObject {
        val command = request.substringBefore(':')
        if (command !in ALLOWED_QUERIES)
            return errorJson("query not allowed: $command")
        return ForkNative.query(request) ?: errorJson("native bridge unavailable")
    }

    private fun setting(intent: Intent): JSONObject {
        val key = intent.getStringExtra(EXTRA_KEY).orEmpty()
        val type = intent.getStringExtra(EXTRA_TYPE).orEmpty()
        val value = intent.getStringExtra(EXTRA_VALUE).orEmpty()
        val options = ForkNative.query("config.options") ?: return errorJson("native bridge unavailable")
        val descriptors = options.optJSONArray("options") ?: return errorJson("config surface unavailable")

        var declaredType: String? = null
        for (index in 0 until descriptors.length()) {
            val descriptor = descriptors.optJSONObject(index) ?: continue
            if (descriptor.optString("key") == key) {
                declaredType = descriptor.optString("type")
                break
            }
        }
        if (declaredType == null) return errorJson("unknown Fork setting: $key")
        if (declaredType != type) return errorJson("$key expects $declaredType, got $type")

        NativeApp.setSetting(ForkSettings.SECTION, key, type, value)
        NativeApp.commitSettings()
        return JSONObject().put("ok", true).put("key", key).put("type", type).put("value", value)
    }

    private fun loadState(slot: Int): JSONObject {
        if (slot !in 0..9) return errorJson("save slot must be between 0 and 9")
        if (!runCatching { NativeApp.hasActiveVM() }.getOrDefault(false))
            return errorJson("no active VM")
        val loaded = NativeApp.loadStateFromSlot(slot)
        return JSONObject().put("ok", loaded).put("slot", slot)
            .apply { if (!loaded) put("error", "savestate load failed") }
    }

    private fun vmAction(action: () -> Unit): JSONObject {
        action()
        return JSONObject().put("ok", true)
    }

    private fun environment(context: Context): JSONObject {
        val active = runCatching { NativeApp.hasActiveVM() }.getOrDefault(false)
        val serial = MainActivityRuntime.currentGame.value?.serial.orEmpty()
        val settings = ConfigStore.resolveForGame(serial.takeIf(String::isNotBlank))
        val fork = ForkNative.query("config.options") ?: JSONObject().put("ok", false)
        val configMaterial = settings.toJson().toString() + "\n" + fork.toString()
        val hash = MessageDigest.getInstance("SHA-256").digest(configMaterial.toByteArray())
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }

        return JSONObject()
            .put("ok", true)
            .put("activeVm", active)
            .put("emuState", MainActivityRuntime.eState.value.name)
            .put("gameSerial", serial)
            .put("appVersion", BuildConfig.VERSION_NAME)
            .put("configSha256", hash)
            .put("driver", ForkNative.query("driver.status") ?: JSONObject.NULL)
            .put("gpu", ForkNative.query("gpu.capabilities") ?: JSONObject.NULL)
            .put("forkConfig", fork)
    }

    private fun errorJson(message: String) = JSONObject().put("ok", false).put("error", message)

    companion object {
        private const val EXTRA_COMMAND = "command"
        private const val EXTRA_REQUEST = "request"
        private const val EXTRA_KEY = "key"
        private const val EXTRA_TYPE = "type"
        private const val EXTRA_VALUE = "value"
        private const val EXTRA_SLOT = "slot"

        private val ALLOWED_QUERIES = setOf(
            "benchmark.begin",
            "benchmark.end",
            "benchmark.status",
            "benchmark.runs",
            "benchmark.clear",
            "config.options",
            "driver.status",
            "gpu.capabilities",
        )
    }
}
