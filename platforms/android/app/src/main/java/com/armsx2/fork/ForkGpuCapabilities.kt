package com.armsx2.fork

import android.os.Build
import com.armsx2.GpuInfo

/**
 * Veredito sobre troca de driver Vulkan, para a UI — espelho da regra nativa em
 * `pcsx2/Fork/ForkGpuCapabilities.h`.
 *
 * Existe em Kotlin porque a decisão precisa ser tomada ANTES de qualquer renderer subir: o driver
 * é escolhido com o emulador parado, e `VKLoader` o consome no primeiro carregamento da libvulkan.
 * Então aqui a fonte é a sonda GL que `GpuInfo` já faz (que lê o driver do SISTEMA, que é
 * justamente o que se quer para decidir) mais `Build.VERSION.SDK_INT`.
 *
 * A regra dura é a mesma dos dois lados: **Turnip é freedreno, logo é Adreno.** Mali, PowerVR,
 * Xclipse e qualquer coisa não reconhecida ficam de fora, em qualquer versão de Android. Xclipse
 * tem pacotes próprios em formato AdrenoTools, mas não são Turnip e não entram neste veredito.
 *
 * Se a regra mudar, mude nos DOIS lugares — e o teste nativo
 * (`tests/ctest/core/fork/gpu_capabilities_tests.cpp`) é quem define o comportamento correto.
 */
object ForkGpuCapabilities {

    /** `VKLoader` chama adrenotools pelo caminho memfd, que exige API 29. */
    const val MIN_SDK_FOR_CUSTOM_DRIVER = 29

    enum class TurnipSupport {
        SUPPORTED,
        UNSUPPORTED_VENDOR,
        UNSUPPORTED_ADRENO_GENERATION,
        UNSUPPORTED_ANDROID_VERSION,
        UNKNOWN,
    }

    /** Geração Adreno extraída do GL_RENDERER, ou null quando não é Adreno / não deu para ler. */
    fun adrenoModel(renderer: String?): Int? {
        val r = renderer ?: return null
        if (!r.contains("Adreno", ignoreCase = true)) return null
        return Regex("""(\d{3,4})""").find(r.substringAfter("Adreno", ""))?.value?.toIntOrNull()
    }

    fun evaluate(
        renderer: String? = GpuInfo.rendererName(),
        sdk: Int = Build.VERSION.SDK_INT,
    ): TurnipSupport {
        // Sem leitura da GPU não se afirma nada: "não sei" é diferente de "não suporta", e dizer
        // ao usuário que o aparelho dele não serve com base em uma sonda que falhou é pior que
        // deixar a opção visível e o carregador cair no driver do sistema.
        if (renderer.isNullOrBlank()) return TurnipSupport.UNKNOWN

        // Fabricante antes da versão do Android, pelo mesmo motivo do lado nativo: mandar o dono
        // de uma Mali atualizar o sistema é mandá-lo perseguir algo que nunca vai funcionar.
        val model = adrenoModel(renderer) ?: return TurnipSupport.UNSUPPORTED_VENDOR
        if (model < 600) return TurnipSupport.UNSUPPORTED_ADRENO_GENERATION
        if (sdk < MIN_SDK_FOR_CUSTOM_DRIVER) return TurnipSupport.UNSUPPORTED_ANDROID_VERSION
        return TurnipSupport.SUPPORTED
    }

    /** Só quando há certeza positiva. UNKNOWN não conta como suportado nem como recusado. */
    fun isSupported(): Boolean = evaluate() == TurnipSupport.SUPPORTED

    /**
     * A seção de driver deve aparecer? Some apenas quando há certeza de que não funciona — em
     * UNKNOWN ela continua visível, porque esconder por causa de uma sonda falha é indistinguível
     * de um bug para quem está olhando.
     */
    fun shouldShowDriverSection(): Boolean = when (evaluate()) {
        TurnipSupport.UNSUPPORTED_VENDOR,
        TurnipSupport.UNSUPPORTED_ADRENO_GENERATION,
        TurnipSupport.UNSUPPORTED_ANDROID_VERSION -> false
        TurnipSupport.SUPPORTED, TurnipSupport.UNKNOWN -> true
    }

    /** Motivo apresentável. Uma seção que some sem explicação vira relato de bug. */
    fun reason(state: TurnipSupport = evaluate()): String = when (state) {
        TurnipSupport.SUPPORTED -> "Disponível."
        TurnipSupport.UNSUPPORTED_VENDOR ->
            "Turnip é um driver para GPUs Adreno (Qualcomm). Esta GPU usa outra arquitetura."
        TurnipSupport.UNSUPPORTED_ADRENO_GENERATION ->
            "Esta Adreno é anterior à série 6xx, para a qual não há builds de Turnip."
        TurnipSupport.UNSUPPORTED_ANDROID_VERSION ->
            "Carregar driver customizado exige Android 10 ou mais recente."
        TurnipSupport.UNKNOWN -> "Não foi possível identificar a GPU deste aparelho."
    }
}
