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

        const val DRIVER_MODE = "Driver.Mode"
        const val DRIVER_DIR = "Driver.Dir"
        const val DRIVER_NAME = "Driver.Name"
        const val DRIVER_REDIRECT_DIR = "Driver.RedirectDir"
        const val DRIVER_HOOK_LIB_DIR = "Driver.HookLibDir"
        const val DRIVER_ID = "Driver.Id"

        const val FRAMEGEN_MODE = "FrameGen.Mode"
        const val FRAMEGEN_BUDGET_MS = "FrameGen.BudgetMs"
        const val FRAMEGEN_MIN_REAL_FPS = "FrameGen.MinRealFps"
    }

    /** Modos aceitos por `Driver.Mode`. */
    object DriverMode {
        /** Não opina — herda o que estiver valendo. É o padrão de um jogo sem override. */
        const val INHERIT = "inherit"
        /** Força o driver do sistema, mesmo que o global seja um Turnip. */
        const val SYSTEM = "system"
        /** Usa os caminhos configurados. */
        const val CUSTOM = "custom"
    }

    /** Modos aceitos por `FrameGen.Mode`. Mesmas strings que `ForkFrameGen::ParseMode` entende —
     *  o núcleo cai em [FrameGenMode.OFF] diante de qualquer valor que não reconheça, então uma
     *  divergência aqui desliga FG em vez de fazer algo inesperado. */
    object FrameGenMode {
        const val OFF = "off"
        /** Engata sozinho quando o ritmo está estável e desengata quando não está. */
        const val AUTO = "auto"
        /** 2x sempre que as condições de segurança permitirem — as condições continuam valendo. */
        const val X2 = "2x"
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

    // ---- Seleção de driver (Fase 5) -----------------------------------------

    /** Caminhos de um pacote instalado, no formato que o adrenotools espera. */
    data class DriverPaths(
        val id: String,
        val dir: String,
        val libraryName: String,
        val redirectDir: String,
        val hookLibDir: String,
    )

    /**
     * Grava a seleção global. Não aplica nada por si: o núcleo lê estas chaves em
     * VMManager::LoadSettings, que roda antes do primeiro MTGS::Open — que é onde o VKLoader
     * consome o driver. Passe null para o driver do sistema.
     */
    fun selectDriverGlobally(paths: DriverPaths?) {
        if (paths == null) {
            putGlobal(Keys.DRIVER_MODE, DriverMode.SYSTEM)
            putGlobal(Keys.DRIVER_ID, "")
        } else {
            putGlobal(Keys.DRIVER_MODE, DriverMode.CUSTOM)
            putGlobal(Keys.DRIVER_ID, paths.id)
            putGlobal(Keys.DRIVER_DIR, paths.dir)
            putGlobal(Keys.DRIVER_NAME, paths.libraryName)
            putGlobal(Keys.DRIVER_REDIRECT_DIR, paths.redirectDir)
            putGlobal(Keys.DRIVER_HOOK_LIB_DIR, paths.hookLibDir)
        }
        commit()
    }

    /**
     * Override por jogo. O chamador abre a escrita do INI do jogo antes, como nas demais opções.
     *
     * `paths = null` força o driver do SISTEMA neste jogo, mesmo com um Turnip global — que é o
     * caso de uso principal do override: um jogo que quebra no Turnip. Para remover o override e
     * voltar a seguir o global, use [clearGameDriverOverride].
     */
    fun selectDriverForGame(paths: DriverPaths?) {
        if (paths == null) {
            putForGame(Keys.DRIVER_MODE, DriverMode.SYSTEM)
        } else {
            putForGame(Keys.DRIVER_MODE, DriverMode.CUSTOM)
            putForGame(Keys.DRIVER_ID, paths.id)
            putForGame(Keys.DRIVER_DIR, paths.dir)
            putForGame(Keys.DRIVER_NAME, paths.libraryName)
            putForGame(Keys.DRIVER_REDIRECT_DIR, paths.redirectDir)
            putForGame(Keys.DRIVER_HOOK_LIB_DIR, paths.hookLibDir)
        }
    }

    /** Faz o jogo voltar a seguir a seleção global. */
    fun clearGameDriverOverride() {
        putForGame(Keys.DRIVER_MODE, DriverMode.INHERIT)
    }

    // ---- Frame Generation (Fase 7) ------------------------------------------

    /**
     * Grava o modo de FG globalmente. Só o modo: orçamento e FPS real mínimo são as travas de
     * segurança da régua, e expô-las na mesma tela em que se liga o recurso convida a afrouxá-las
     * até FG engatar em cima de uma emulação lenta — que é precisamente o que ele não pode fazer.
     * Quem precisa mexer nelas tem a seção `[Fork]` do INI.
     */
    fun setFrameGenModeGlobally(mode: String) {
        putGlobal(Keys.FRAMEGEN_MODE, mode)
        commit()
    }

    /** Override por jogo. O chamador abre a escrita do INI do jogo antes, como nas demais opções. */
    fun setFrameGenModeForGame(mode: String) {
        putForGame(Keys.FRAMEGEN_MODE, mode)
    }
}
