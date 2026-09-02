package com.armsx2.config

import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Migração de configuração: o que acontece quando um blob gravado por uma versão ANTERIOR é
 * carregado pela atual.
 *
 * Por que isto existe: o commit que removeu `hwAat` e os quatro `useMac*` deixou a
 * compatibilidade com JSON antigo dependendo de duas strings literais espalhadas em `fromJson` e
 * no merge de override por jogo — e nada as exercitava. Se alguém reorganizar aquele bloco, um
 * usuário que gravou `hwAat=true` numa versão antiga perde a escolha EM SILÊNCIO, que é o pior
 * modo de falha possível numa migração: não há erro, não há log, só uma opção que voltou sozinha.
 *
 * O caminho do INI é testado pela costura que já existia: [Settings.emitSink] desvia as escritas
 * de `applyTo()` para um capturador em vez do `NativeApp`, e `applyTo` retorna antes das chamadas
 * nativas ao vivo quando o sink está setado — então tudo aqui roda em JVM pura, sem aparelho.
 */
class SettingsMigrationTest {

    @After
    fun tearDown() {
        // O sink é estado global do companion. Deixá-lo setado contaminaria qualquer teste
        // seguinte que chamasse applyTo esperando o caminho nativo.
        Settings.emitSink = null
    }

    /** Captura o conjunto de chaves persistidas que [Settings.applyTo] emitiria. */
    private fun iniOf(settings: Settings): Map<String, String> {
        val out = LinkedHashMap<String, String>()
        Settings.emitSink = { section, key, _, value -> out["$section/$key"] = value }
        try {
            settings.applyTo()
        } finally {
            Settings.emitSink = null
        }
        return out
    }

    // ---- hwAat -> hwAccurateAlphaTest ---------------------------------------------------------

    @Test
    fun `nome antigo hwAat vira hwAccurateAlphaTest`() {
        val s = Settings.fromJson(JSONObject().put("hwAat", true))
        assertTrue("um JSON gravado antes da renomeacao tem de manter a escolha", s.hwAccurateAlphaTest)
    }

    @Test
    fun `hwAat antigo nao apaga as outras opcoes do mesmo blob`() {
        val json = JSONObject()
            .put("hwAat", true)
            .put("eeCycleRate", 2)
            .put("lsfgFlowScale", 75)
        val s = Settings.fromJson(json)
        assertTrue(s.hwAccurateAlphaTest)
        assertEquals(2, s.eeCycleRate)
        assertEquals(75, s.lsfgFlowScale)
    }

    @Test
    fun `com as duas chaves presentes o nome novo vence`() {
        // Um blob gravado DURANTE a transicao pode trazer as duas. A resposta certa e a nova:
        // e a que a UI atual escreve, entao e a ultima escolha real do usuario.
        val json = JSONObject().put("hwAat", true).put("hwAccurateAlphaTest", false)
        assertFalse(Settings.fromJson(json).hwAccurateAlphaTest)
    }

    @Test
    fun `override por jogo tambem aceita o nome antigo`() {
        val merged = Settings.merge(Settings(), JSONObject().put("hwAat", true))
        assertTrue("o override por jogo tem o proprio caminho de leitura", merged.hwAccurateAlphaTest)
    }

    // ---- UseMac* ------------------------------------------------------------------------------

    @Test
    fun `chaves useMac antigas nao quebram o parse nem apagam nada`() {
        val json = JSONObject()
            .put("useMacEE", true)
            .put("useMacIOP", true)
            .put("useMacVU0", true)
            .put("useMacVU1", true)
            .put("eeCycleRate", 3)
        assertEquals(3, Settings.fromJson(json).eeCycleRate)
    }

    @Test
    fun `as quatro chaves UseMac continuam sendo escritas no INI`() {
        // As PROPRIEDADES foram removidas; as CHAVES continuam saindo como literal "true", que e
        // o que limpa um valor antigo persistido. Remover as propriedades nao pode ter mudado
        // isto, e e exatamente o que este teste prende.
        val ini = iniOf(Settings())
        for (key in listOf("UseMacEE", "UseMacIOP", "UseMacVU0", "UseMacVU1")) {
            assertEquals("EmuCore/CPU/Recompiler/$key", "true", ini["EmuCore/CPU/Recompiler/$key"])
        }
    }

    // ---- lsfgFlowScale ------------------------------------------------------------------------

    @Test
    fun `lsfgFlowScale ausente usa o padrao do fork e nao o do upstream`() {
        // 25 saiu de medicao no Odin 2 (5,3 ms contra 14,2 ms a 100%). O merge de 214 commits
        // trouxe a declaracao do upstream com 100 e chegou a deixar as DUAS na mesma data class,
        // o que derrubou a alpha 23. Este teste prende qual das duas sobreviveu.
        assertEquals(25, Settings.fromJson(JSONObject()).lsfgFlowScale)
    }

    @Test
    fun `lsfgFlowScale fora da faixa e limitado ao escrever, nunca vira valor perigoso`() {
        assertEquals("100", iniOf(Settings(lsfgFlowScale = 500))["EmuCore/GS/LsfgFlowScale"])
        assertEquals("25", iniOf(Settings(lsfgFlowScale = 0))["EmuCore/GS/LsfgFlowScale"])
        assertEquals("25", iniOf(Settings(lsfgFlowScale = -7))["EmuCore/GS/LsfgFlowScale"])
    }

    // ---- robustez do parse --------------------------------------------------------------------

    @Test
    fun `chave desconhecida e ignorada sem levar o resto junto`() {
        val json = JSONObject().put("umaOpcaoQueNuncaExistiu", 42).put("eeCycleRate", 3)
        assertEquals(3, Settings.fromJson(json).eeCycleRate)
    }

    @Test
    fun `valor de tipo errado cai no padrao em vez de derrubar o carregamento`() {
        val json = JSONObject().put("eeCycleRate", "isto nao e um numero").put("lsfgFlowScale", 60)
        val s = Settings.fromJson(json)
        assertEquals(Settings().eeCycleRate, s.eeCycleRate)
        assertEquals("o resto do blob tem de sobreviver ao campo invalido", 60, s.lsfgFlowScale)
    }

    // ---- global x por jogo --------------------------------------------------------------------

    @Test
    fun `override por jogo so muda o que declara`() {
        val base = Settings(eeCycleRate = 2, lsfgFlowScale = 50, hwAccurateAlphaTest = true)
        val merged = Settings.merge(base, JSONObject().put("eeCycleRate", 3))
        assertEquals(3, merged.eeCycleRate)
        assertEquals("nao declarado no override, tem de vir da base", 50, merged.lsfgFlowScale)
        assertTrue("nao declarado no override, tem de vir da base", merged.hwAccurateAlphaTest)
    }

    @Test
    fun `override vazio devolve a base inteira`() {
        val base = Settings(eeCycleRate = 2, lsfgFlowScale = 50)
        assertEquals(base, Settings.merge(base, JSONObject()))
    }

    // ---- invariante geral ---------------------------------------------------------------------

    @Test
    fun `ida e volta pelo JSON preserva todos os campos`() {
        // O invariante que pega o proximo campo que sumir da serializacao — que foi como hwAat e
        // os useMac* apareceram. Vale para a data class inteira, nao so para os campos citados
        // acima, entao cobre tambem o que ainda nem existe.
        val original = Settings(
            eeCycleRate = 2,
            lsfgFlowScale = 60,
            hwAccurateAlphaTest = true,
            upscaleFloat = 2.5f,
        )
        assertEquals(original, Settings.fromJson(original.toJson()))
    }
}
