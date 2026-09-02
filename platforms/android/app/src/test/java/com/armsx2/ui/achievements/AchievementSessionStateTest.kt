package com.armsx2.ui.achievements

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Os cenários que a segunda tela já errou, e alguns que ela poderia errar do mesmo jeito.
 *
 * Todos foram caros de reproduzir à mão: exigiam dois jogos, um tile específico colocado e a
 * ordem certa de troca. Aqui cada um é meia dúzia de linhas, porque o estado saiu da View.
 */
class AchievementSessionStateTest {

    private fun item(id: Int, titulo: String, desbloqueada: Boolean, pontos: Int = 10) =
        AchievementItem(
            id = id, title = titulo, description = "", points = pontos,
            unlocked = desbloqueada, progress = "", iconUrl = "",
        )

    // ---- ciclo de vida basico -----------------------------------------------------------------

    @Test
    fun `sem jogo nao ha estado`() {
        val s = AchievementSessionState()
        assertNull(s.gameKey)
        assertTrue(s.items.isEmpty())
        assertNull(s.counts())
    }

    @Test
    fun `snapshot sem jogo declarado e ignorado`() {
        // Dado de conquista sem dono e exatamente o que produzia contaminacao entre jogos.
        val s = AchievementSessionState()
        assertNull(s.onSnapshot(listOf(item(1, "A", true))))
        assertTrue(s.items.isEmpty())
    }

    @Test
    fun `abrir jogo A muda a identidade`() {
        val s = AchievementSessionState()
        assertTrue(s.onGame("SLUS-A"))
        assertEquals("SLUS-A", s.gameKey)
    }

    @Test
    fun `mesma chave repetida nao descarta nada`() {
        // O caso comum: uma chamada por tick, 2x por segundo.
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        s.onSnapshot(listOf(item(1, "A", true)))
        assertEquals(false, s.onGame("SLUS-A"))
        assertEquals(1, s.items.size)
    }

    // ---- semear x anunciar --------------------------------------------------------------------

    @Test
    fun `conquista ja desbloqueada no primeiro snapshot nao e anunciada`() {
        // RetroAchievements nao manda carimbo de hora. A unica evidencia de "acabou de cair" e
        // ter visto o estado anterior — no primeiro snapshot nao ha estado anterior.
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        assertNull(s.onSnapshot(listOf(item(1, "Velha", true), item(2, "Pendente", false))))
        assertNull(s.lastUnlock)
    }

    @Test
    fun `conquista realmente nova e anunciada`() {
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        s.onSnapshot(listOf(item(1, "Velha", true), item(2, "Nova", false)))
        assertEquals("Nova", s.onSnapshot(listOf(item(1, "Velha", true), item(2, "Nova", true))))
        assertEquals("Nova", s.lastUnlock)
    }

    @Test
    fun `tick sem mudanca nao anuncia nada`() {
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        s.onSnapshot(listOf(item(1, "A", true)))
        assertNull(s.onSnapshot(listOf(item(1, "A", true))))
    }

    // ---- troca de jogo: os tres bugs reais ----------------------------------------------------

    @Test
    fun `jogo B nao exibe dados do jogo A`() {
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        s.onSnapshot(listOf(item(1, "Do jogo A", true)))
        s.onGame("SLUS-B")
        assertTrue("items tem de ser descartado na troca", s.items.isEmpty())
        assertNull("lastUnlock tem de ser descartado na troca", s.lastUnlock)
    }

    @Test
    fun `ids do jogo B nao viram desbloqueios novos so porque o jogo mudou`() {
        // O pior dos tres. Ids de conquista sao globalmente unicos, entao NENHUM id de B estava
        // no conjunto herdado de A — e tudo que B ja tinha desbloqueado entrava como novidade.
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        s.onSnapshot(listOf(item(100, "De A", true)))
        s.onGame("SLUS-B")
        assertNull(
            "primeiro snapshot de B nao pode anunciar o que B ja tinha",
            s.onSnapshot(listOf(item(900, "Ja era de B", true), item(901, "Outra de B", true))),
        )
        assertNull(s.lastUnlock)
    }

    @Test
    fun `voltar para a biblioteca descarta o estado`() {
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        s.onSnapshot(listOf(item(1, "A", true)))
        assertTrue(s.onGame(null))
        assertTrue(s.items.isEmpty())
        assertNull(s.lastUnlock)
    }

    @Test
    fun `troca rapida A para B para A nao ressuscita o estado de A`() {
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        s.onSnapshot(listOf(item(1, "De A", true)))
        s.onGame("SLUS-B")
        s.onGame("SLUS-A")
        assertTrue("voltar para A e um jogo novo, nao a sessao anterior", s.items.isEmpty())
        assertNull(s.lastUnlock)
        // E o primeiro snapshot de volta em A tem de semear, nao anunciar.
        assertNull(s.onSnapshot(listOf(item(1, "De A", true))))
    }

    // ---- tiles adicionados e removidos --------------------------------------------------------

    @Test
    fun `tile colocado depois da troca recebe dados do jogo certo`() {
        // Sem tile de RA no painel o chamador nao consulta, entao nao ha snapshot — e este era o
        // buraco: o estado antigo sobrevivia ate alguem colocar um tile.
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        s.onSnapshot(listOf(item(1, "De A", true)))
        s.onGame("SLUS-B")               // trocou sem nenhum tile colocado
        assertTrue(s.items.isEmpty())
        s.onSnapshot(listOf(item(900, "De B", false)))  // tile colocado agora
        assertEquals(1, s.items.size)
        assertEquals("De B", s.items[0].title)
    }

    @Test
    fun `remover todos os tiles e recolocar nao anuncia o acervo inteiro`() {
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        s.onSnapshot(listOf(item(1, "A", true), item(2, "B", false)))
        // Tiles removidos: o chamador para de consultar. Nenhum onSnapshot por varios ticks.
        // Recolocado: o jogo NAO mudou, entao o conjunto visto continua valendo e nada e anunciado.
        assertNull(s.onSnapshot(listOf(item(1, "A", true), item(2, "B", false))))
        assertNull(s.lastUnlock)
    }

    // ---- identidade do jogo -------------------------------------------------------------------

    @Test
    fun `serial ausente cai no titulo`() {
        // A chave e montada pelo chamador como `serial ?: title`; aqui se prende o efeito: dois
        // jogos sem serial e com titulos diferentes sao jogos DIFERENTES.
        val s = AchievementSessionState()
        s.onGame("Jogo Sem Serial")
        s.onSnapshot(listOf(item(1, "X", true)))
        assertTrue(s.onGame("Outro Sem Serial"))
        assertTrue(s.items.isEmpty())
    }

    @Test
    fun `mesmo titulo com serial diferente sao jogos diferentes`() {
        // Duas regioes do mesmo jogo tem o mesmo titulo e conjuntos de conquistas distintos.
        val s = AchievementSessionState()
        s.onGame("SLUS-12345")
        s.onSnapshot(listOf(item(1, "X", true)))
        assertTrue(s.onGame("SLES-54321"))
        assertTrue(s.items.isEmpty())
    }

    // ---- entradas degeneradas -----------------------------------------------------------------

    @Test
    fun `lista vazia nao quebra nem apaga a identidade`() {
        // Logout, ou jogo sem conjunto de conquistas: a consulta devolve lista vazia.
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        assertNull(s.onSnapshot(emptyList()))
        assertEquals("SLUS-A", s.gameKey)
        assertNull(s.counts())
        assertNull(s.points())
    }

    @Test
    fun `login depois de lista vazia semeia sem anunciar`() {
        // Deslogado a lista vem vazia; ao logar, o acervo inteiro aparece de uma vez. Anunciar
        // isso encheria o tile de "ultimo desbloqueio" com uma conquista de anos atras.
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        s.onSnapshot(emptyList())
        assertNull(
            "o primeiro snapshot COM dados ainda e o primeiro do jogo",
            s.onSnapshot(listOf(item(1, "Antiga", true), item(2, "Outra antiga", true))),
        )
        assertNull(s.lastUnlock)
    }

    @Test
    fun `logout apos ter dados nao anuncia nada e zera as contagens`() {
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        s.onSnapshot(listOf(item(1, "A", true)))
        assertNull(s.onSnapshot(emptyList()))
        assertNull(s.counts())
    }

    // ---- numeros exibidos ---------------------------------------------------------------------

    @Test
    fun `contagens e pontos refletem o snapshot atual`() {
        val s = AchievementSessionState()
        s.onGame("SLUS-A")
        s.onSnapshot(listOf(item(1, "A", true, 5), item(2, "B", false, 10), item(3, "C", true, 25)))
        assertEquals(Pair(2, 3), s.counts())
        assertEquals(Pair(30, 40), s.points())
    }
}
