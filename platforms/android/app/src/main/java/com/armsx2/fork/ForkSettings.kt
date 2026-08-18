package com.armsx2.fork

import kr.co.iefriends.pcsx2.NativeApp

/**
 * Lado Android da superfície de configuração do fork (seção `[Fork]` do INI).
 *
 * O ponto desta camada é o que ela NÃO exige: nenhuma função de JNI nova, nenhum arquivo Java do
 * upstream alterado. `NativeApp.setSetting` e `NativeApp.gameIniPut` já são genéricos
 * (seção/chave/valor), e o núcleo lê `[Fork]` pela interface de settings em camadas do próprio
 * PCSX2 — então override por jogo funciona aqui pelo mesmo mecanismo das opções do upstream.
 *
 * Acrescentar uma opção do fork é acrescentar uma linha na tabela em `pcsx2/Fork/ForkConfig.cpp`.
 * Nada precisa mudar neste arquivo, exceto a constante de chave, se a UI quiser oferecê-la.
 */
object ForkSettings {

    /** Mesma seção declarada em ForkConfig::SECTION. */
    const val SECTION = "Fork"

    /** Chaves conhecidas hoje. Espelham a tabela em pcsx2/Fork/ForkConfig.cpp. */
    object Keys {
        const val PRESENTATION_METRICS_ENABLED = "PresentationMetrics.Enabled"
        const val PRESENTATION_METRICS_OVERLAY = "PresentationMetrics.Overlay"
    }

    /**
     * Grava uma opção globalmente. As escritas ficam na fila até [commit] — o núcleo aplica o lote
     * de uma vez, e aplicar metade de uma mudança é pior que não aplicar nada.
     */
    fun putGlobal(key: String, value: String, type: String = "string") {
        NativeApp.setSetting(SECTION, key, type, value)
    }

    fun putGlobalBool(key: String, value: Boolean) = putGlobal(key, value.toString(), "bool")

    /** Aplica o lote de escritas globais. */
    fun commit() {
        NativeApp.commitSettings()
    }

    /**
     * Grava uma opção no INI do jogo em edição. O chamador é responsável por abrir a escrita do
     * INI do jogo antes (o mesmo fluxo que as opções do upstream já usam) — este objeto não
     * inventa persistência que o frontend já sabe fazer.
     */
    fun putForGame(key: String, value: String) {
        NativeApp.gameIniPut(SECTION, key, value)
    }

    fun putForGameBool(key: String, value: Boolean) = putForGame(key, value.toString())
}
