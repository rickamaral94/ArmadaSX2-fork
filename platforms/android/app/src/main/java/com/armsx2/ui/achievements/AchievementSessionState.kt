package com.armsx2.ui.achievements

/**
 * O estado de RetroAchievements que a segunda tela carrega ENTRE ticks, isolado da View.
 *
 * Por que existe separado: esta lógica já errou três vezes, e todas as três eram invisíveis para
 * o compilador e caras de reproduzir à mão — exigiam dois jogos, um tile específico colocado e a
 * ordem certa de troca. Aqui ela é uma classe sem Android nenhum, então cada erro daqueles vira
 * um teste de meia dúzia de linhas.
 *
 * Os três erros, para que não voltem:
 *
 *   * `seenUnlocked` atravessava a troca de jogo. Como ids de conquista são globalmente únicos,
 *     NENHUM id do jogo B estava no conjunto herdado de A, então tudo que B já tinha desbloqueado
 *     entrava como "novidade" e a régua anunciava uma conquista velha como recém-caída.
 *   * `lastUnlock` também atravessava, e o tile de último desbloqueio seguia mostrando o título
 *     de um jogo enquanto se jogava outro.
 *   * `items` não era limpo ao trocar de jogo quando nenhum tile de RA estava no painel, então um
 *     tile adicionado depois exibia dados do jogo anterior.
 *
 * A regra que os cobre é uma só: **este estado pertence a um jogo**, e trocar de jogo o descarta
 * inteiro. [onGame] é o único ponto onde a identidade muda, e ele zera os três juntos.
 */
class AchievementSessionState {

    /** Jogo a que o estado atual se refere; `null` fora de jogo. */
    var gameKey: String? = null
        private set

    /** Conquistas do tick mais recente. Vazia quando não há jogo ou nada foi consultado ainda. */
    var items: List<AchievementItem> = emptyList()
        private set

    /** Título do desbloqueio mais recente observado NESTA sessão de jogo; `null` se nenhum. */
    var lastUnlock: String? = null
        private set

    private var seenUnlocked: Set<Int> = emptySet()

    /** Já houve um snapshot para o jogo atual? É o que separa "primeira leitura" de "mudou". */
    var hasSnapshot: Boolean = false
        private set

    /**
     * Declara qual jogo está em cena. `null` = biblioteca, sem jogo.
     *
     * Chamar com a mesma chave é inócuo — é o caso comum, um por tick. Chamar com chave diferente
     * descarta TUDO: itens, desbloqueios vistos e último desbloqueio.
     *
     * @return `true` quando a identidade mudou e o estado foi descartado.
     */
    fun onGame(key: String?): Boolean {
        if (key == gameKey) return false
        gameKey = key
        items = emptyList()
        seenUnlocked = emptySet()
        lastUnlock = null
        hasSnapshot = false
        return true
    }

    /**
     * Entrega o snapshot de conquistas do tick.
     *
     * O PRIMEIRO snapshot de um jogo apenas semeia o conjunto: tudo que já estava desbloqueado
     * quando o jogo abriu não é novidade, é histórico. Só a partir do segundo é que a transição
     * bloqueado→desbloqueado conta como desbloqueio novo — RetroAchievements não manda carimbo de
     * hora, então a única evidência de "acabou de cair" é ter visto o estado anterior.
     *
     * Sem jogo declarado o snapshot é ignorado: dado de conquista sem dono é exatamente o que
     * produzia contaminação entre jogos.
     *
     * @return título do desbloqueio novo deste tick, ou `null`.
     */
    fun onSnapshot(snapshot: List<AchievementItem>): String? {
        if (gameKey == null) return null

        items = snapshot
        val unlocked = snapshot.filter { it.unlocked }.map { it.id }.toSet()

        // Lista VAZIA nao semeia. Vazio nao quer dizer "este jogo tem zero desbloqueadas" — quer
        // dizer que nao ha dado: usuario deslogado, ou jogo sem conjunto de conquistas. Semear
        // com o conjunto vazio fazia o login seguinte tratar o ACERVO INTEIRO como novidade e
        // anunciar uma conquista de anos atras como recem-caida. Um jogo que tem conquistas mas
        // nenhuma desbloqueada devolve lista NAO vazia com unlocked=false, entao "nao vazia" e o
        // discriminador certo.
        if (snapshot.isEmpty()) return null

        if (!hasSnapshot) {
            hasSnapshot = true
            seenUnlocked = unlocked
            return null
        }

        val novos = unlocked - seenUnlocked
        seenUnlocked = unlocked
        if (novos.isEmpty()) return null

        // Em ordem da lista, que e a ordem da propria RetroAchievements: com dois desbloqueios no
        // mesmo tick, qualquer escolha e arbitraria, mas uma escolha ESTAVEL e reproduzivel.
        val titulo = snapshot.firstOrNull { it.id in novos }?.title
        if (titulo != null) lastUnlock = titulo
        return titulo
    }

    /** Conquistas desbloqueadas / total. `null` quando não há dados. */
    fun counts(): Pair<Int, Int>? =
        if (items.isEmpty()) null else Pair(items.count { it.unlocked }, items.size)

    /** Pontos ganhos / total. `null` quando não há dados. */
    fun points(): Pair<Int, Int>? =
        if (items.isEmpty()) null else Pair(
            items.filter { it.unlocked }.sumOf { it.points },
            items.sumOf { it.points },
        )
}
