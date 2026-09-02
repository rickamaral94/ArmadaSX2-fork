# Auditoria de baseline e plano de validação — setembro/2026

Registro exigido antes de qualquer alteração de código: baseline, estado comprovado de
compilação, achados e o plano de validação. Nada aqui foi implementado ainda — este documento é
a linha de partida contra a qual as mudanças seguintes serão medidas.

---

## 1. Resumo executivo

| | |
|---|---|
| Último verde COMPROVADO | `3eb7f886c3` — CI release verde (alpha 24) + debug e release locais |
| HEAD | `f14d4ad4e2` |
| Merge de upstream sob auditoria | `3934d5819f` (ARMSX2 `6e1e8f0a18`, 214 commits) |
| Commits desde o último verde pré-merge (`a0a1206e24`) | 230 |
| Commits próprios do fork desde o merge | 14 |
| Commits no HEAD ainda não cobertos pela alpha 24 | 1 (`f14d4ad4e2`) |

**Estado de compilação.** `3eb7f886c3` está verde nos cinco estratos: Kotlin, C++ ARM64, link
nativo, APK debug e APK release (LTO + R8), este último tanto local quanto no runner.
**`f14d4ad4e2` (HEAD) foi validado APENAS em Kotlin.** Não passou por compilação nativa, link,
APK nem release. Pela regra do próprio brief, HEAD não está validado.

**Ganhos desde o merge.** Três alphas quebradas foram diagnosticadas e corrigidas
(link, sintaxe Kotlin, ambiguidade Kotlin); o `native.log` deixou de dar `fflush` por bloco;
o RetroAchievements deixou de ser consultado 2×/s por quem não usa; nove campos mortos saíram
do código; e passou a existir toolchain local capaz de compilar tudo antes de marcar tag.

**Regressões encontradas nesta auditoria: nenhuma.** As varreduras de marcadores de conflito
residuais, propriedades duplicadas em toda a base Kotlin e declarações sem definição vieram
limpas. O que existe são lacunas de VALIDAÇÃO e de PROCESSO, não defeitos de comportamento
identificados.

**Nível de risco: MÉDIO.** Não por suspeita sobre o código, mas porque três alterações que já
estão em `main` tocaram persistência de configuração do usuário e caminhos de log sem teste
automatizado nem medição — e porque o gate que impediria isso ainda não existe.

**Recomendação de release: APROVAR COM RESSALVAS.** A alpha 24 (`3eb7f886c3`) está publicada e
é sólida. `f14d4ad4e2` **não deve virar tag** antes de passar pelo gate completo e de ganhar os
testes de migração descritos em V-11.

---

## 2. Baseline reconstruído

### Histórico de compilação, por commit

| commit | alpha | resultado | causa |
|---|---|---|---|
| `a0a1206e24` | 20 | verde | último verde antes do merge |
| `3934d5819f` | — | — | merge de 214 commits |
| `3934d5819f` | 21 | **falha no LINK** | resolver conflito pegando "todo o lado do upstream" apagou `GSDeviceVK::QuiesceAsyncPipelineCompiler` e `::PersistAsyncPipelineWork`; declarações e 8 chamadas sobreviveram |
| `d92ec9787b` | 22 | **falha no KOTLIN** | resolução por UNIÃO cortou o meio de um bloco KDoc, deixando ` *` órfão |
| `92e93f1041` | 23 | **falha no KOTLIN** | sob a cascata de sintaxe havia `lsfgFlowScale` declarado DUAS vezes; `Overload resolution ambiguity` em 3 arquivos |
| `3eb7f886c3` | 24 | **verde** | primeira release verde desde a 20 |
| `f14d4ad4e2` | — | parcial | só Kotlin |

O padrão é único e vale registrar: **as três falhas vieram do mesmo merge, em estratos
diferentes**, e cada uma só apareceu depois que a anterior foi corrigida. Uma cascata de erros de
sintaxe esconde o erro semântico embaixo dela.

### Subsistemas tocados pelo merge (773 arquivos)

| subsistema | arquivos |
|---|---|
| `platforms/android` | 366 |
| `3rdparty` | 132 |
| `pcsx2-qt` | 67 |
| `tests` | 60 |
| `pcsx2/GS` | 59 |
| `pcsx2/arm64` | 13 |
| `.github` | 13 |

### Varreduras estruturais — resultado

| verificação | método | resultado |
|---|---|---|
| Marcadores de conflito residuais | grep `<<<<<<<`/`=======`/`>>>>>>>` em `.cpp/.h/.kt/.inl/.kts` | **0** |
| Propriedades duplicadas | parser de construtor primário sobre TODAS as `class`/`data class` do app | **0** |
| Declarações sem definição | heurística sobre `GSDeviceVK.h`, `GSDevice.h`, `GSLsfg.h`, `GSGPUProfile.h` e todos os `Fork/*.h` | 12 candidatos, **todos falsos positivos** |
| Link real | `assembleGithubDebug` + `assembleGithubRelease` | **passa** |

Os 12 candidatos são `static inline constexpr` definidos no próprio cabeçalho
(`IsDATMConvertShader`, `HasColorOutput`, …), o template `BitEqual`, e `DoBeginPresent`, que é
virtual puro implementado por backend. A checagem autoritativa continua sendo o linker.

### Tempo de vida de `matched_rule_ids` — verificado

Os ids guardados como `const char*` vêm de `static constexpr std::array<DriverRule, 28>
s_driver_rules` em escopo de arquivo: **duração estática, sem ponteiro pendurado**. Os três
consumidores (`GSDeviceVK`, `GSDeviceOGL`, `ForkGpuCapabilities`) chamam na criação do device ou
no snapshot de capacidades — **não há uso por quadro**, então a alocação de `MatchedRulesString()`
não está em hot path.

---

## 3. Achados

Campos: problema · efeito atual · proposta · risco · evidência · teste necessário.

### V-01 · HEAD validado apenas em Kotlin — **ALTO** · correção de processo

`f14d4ad4e2` removeu cinco propriedades da `data class Settings`. Rodei
`compileGithubDebugKotlin` e nada mais.

*Efeito atual:* o commit no topo do branch não tem link nem APK comprovados. É exatamente a
situação que produziu as alphas 21-23.
*Proposta:* rodar o gate completo sobre HEAD antes de qualquer coisa.
*Risco de agir:* nenhum. *Risco de não agir:* repetir o ciclo já pago três vezes.
*Evidência:* histórico desta sessão; nenhuma execução de `assembleGithubDebug` sobre `f14d4ad4e2`.
*Teste:* o gate da seção 4.

### V-11 · Migração de configuração sem teste — **ALTO** · correção de compatibilidade

`f14d4ad4e2` removeu `hwAat` e os quatro `useMac*`. A compatibilidade com JSON antigo passou a
depender de duas strings literais (`"hwAat"` em `fromJson` e no diff de override por jogo) que
**nenhum teste exercita**.

*Efeito atual:* se alguém reorganizar esse bloco, um usuário que gravou `hwAat=true` numa versão
antiga perde a escolha em silêncio — o pior modo de falha possível em migração.
*Proposta:* teste que carrega um JSON de versão anterior contendo `hwAat` e as quatro chaves
`UseMac*`, e afirma que (a) `hwAccurateAlphaTest` recebe o valor antigo, (b) nenhuma outra opção
é perdida, (c) as chaves `UseMac*` continuam saindo no INI.
*Risco:* BAIXO para implementar, ALTO se ficar sem.
*Evidência:* `Settings.kt` — `hwAat` sobrevive só como literal em duas expressões.
*Prioridade:* 2.

### V-07 · Não existe gate único — **ALTO** · melhoria de processo

A validação de hoje foi ad-hoc: eu executei os passos na mão, na ordem que julguei certa.

*Efeito atual:* não é reproduzível por outra pessoa nem pelo CI, e a ordem certa mora só nesta
conversa.
*Proposta:* `tools/fork/gate.sh` conforme a seção 4, chamado também pelo CI, com o job de release
dependendo dele.
*Prioridade:* 1.

### V-02 · `|| true` engole falha de instalação do NDK — **MÉDIO** · correção de estabilidade

`fork-release.yml:68` e `fork-android-arm64.yml:77`:
`yes | sdkmanager "ndk;28.2.13676358" "cmake;3.31.6" >/dev/null || true`

*Efeito atual:* se a instalação falhar, o workflow segue e ou usa outro NDK que o runner já tinha,
ou falha adiante com mensagem que não aponta para a causa. Um binário de release compilado com
NDK diferente do pinado invalida qualquer A/B de driver ou de frame generation.
*Proposta:* remover o `|| true` e verificar depois que o diretório do NDK pinado existe.
*Evidência:* as duas linhas citadas.
*Teste:* rodar o workflow com uma versão inexistente e confirmar que ele falha.

### V-03 · JDK divergente entre CI e validação local — **MÉDIO** · reprodutibilidade

CI usa Temurin **17**; a validação local desta sessão rodou em OpenJDK **21**.

*Efeito atual:* o "verde local" que passei a usar como garantia não foi produzido no mesmo JDK do
CI. Passou nos dois, então não houve dano — mas a garantia é mais fraca do que eu a apresentei.
*Proposta:* o gate valida a major do JDK e avisa quando diferir do CI; a doc passa a dizer 17.
*Evidência:* `fork-release.yml:59` contra `java -version` local.

### V-04 · `cmdline-tools` baixado sem verificação de integridade — **MÉDIO** · melhoria de processo

`tools/fork/setup-android-sdk.sh` faz `curl` do zip das command-line tools e extrai sem conferir
checksum, num script que o brief exige que confira. O Google publica o SHA-256 na página do
repositório de pacotes.

*Efeito atual:* um download corrompido ou substituído entra no toolchain sem aviso.
*Proposta:* pinar o SHA-256 e abortar quando não bater.
*Nota:* o Gradle wrapper JÁ faz isso (`distributionSha256Sum` no `gradle-wrapper.properties`) — o
padrão existe no projeto, o script é que não o seguiu.

### V-05 · Versão do Rust não pinada em lugar nenhum — **MÉDIO** · reprodutibilidade

CI e script fazem `rustup target add aarch64-linux-android`, mas nada fixa a versão do `rustc`.
O librashader é compilado por cargo com o que houver no runner.

*Efeito atual:* dois builds do mesmo commit podem embutir um librashader gerado por compiladores
diferentes. Para um projeto cuja regra é "A/B não vale se o toolchain mudou entre as medições",
é uma porta aberta.
*Proposta:* `rust-toolchain.toml` na raiz, honrado por CI e script.

### V-09 · A03 (`native.log`) declarado como ganho sem números — **MÉDIO** · otimização provável ainda sem medição

O commit `cd272f5359` afirma que agrupar os flush reduz escrita. O raciocínio é sólido — um bloco
de ≤511 bytes por `fflush` contra coalescimento de 200 ms — mas **não medi nada**.

*Proposta:* ferramenta de estresse que injete N blocos a uma taxa dada e reporte blocos recebidos,
bytes gravados, número de flushes, tempo em escrita, atraso máximo entre receber e persistir, e
comportamento ao passar de 8 MiB; rodar contra as duas versões.
*Classificação até lá:* otimização PROVÁVEL, não comprovada.

### V-10 · Segunda tela sem medição e sem os cenários testados — **MÉDIO** · otimização provável ainda sem medição

`600998e535` e `a11592f37c` reduzem trabalho por tick, sem número e sem os cenários que o brief
lista (nenhum tile de RA, cada tile isolado, todos juntos, login/logout, troca de jogo, volta à
biblioteca, tiles adicionados em sessão, conectar/desconectar a tela, mudança de idioma).

*Risco específico e concreto:* `raItems` é estado de instância do painel. Com a nova guarda,
trocar de jogo com nenhum tile de RA colocado **não limpa `raItems`** — o `else if (!inGame)`
só zera fora de jogo. Se um tile for adicionado durante a sessão seguinte, o primeiro tick pode
exibir dados do jogo anterior até `trackAchievements()` rodar.
*Proposta:* zerar `raItems` na troca de jogo, não só ao sair para a biblioteca.
*Teste:* jogo A com tile de RA → biblioteca → jogo B → conferir que nada de A aparece.
**Este é o achado de comportamento mais concreto da auditoria.** Prioridade 3.

### V-08 · Suíte de testes host fora do ciclo local — **MÉDIO** · melhoria de processo

`tests/ctest/core/fork/` tem 12 arquivos, e a `phase-0.5` roda ~1.830 casos de recompilador — nada
disso roda no ciclo local, porque o build host exige a cadeia `~/deps` (Qt, ffmpeg) que o build
Android não precisa. Viabilidade local **não verificada**.
*Proposta:* medir o custo de um configure host mínimo; se couber, entra no gate; se não couber,
o gate delega ao CI e diz isso explicitamente em vez de omitir.

### V-12 · Cobertura de `MatchedRulesString` incompleta — **BAIXO** · higiene

Existem testes para nenhuma regra, uma, várias com ordem, e estouro. **Faltam dois** que o brief
pede: contagem EXATAMENTE no limite (`MAX_TRACKED_RULES`, sem sufixo `+N`) e id nulo — o código
trata `nullptr` com um `continue`, e esse ramo não tem teste.

### V-06 · Script de setup duplica `compileSdk`/`build-tools` — **BAIXO** · higiene

`setup-android-sdk.sh` fixa `platforms;android-37.0` e `build-tools;37.0.0` enquanto o projeto
declara `compileSdk = 37` no `build.gradle.kts`. Se um subir, o outro fica para trás em silêncio.
*Proposta:* derivar do `build.gradle.kts`, ou o gate compara os dois e falha na divergência.

### V-13 · `PolicyFromConfig()` por quadro apresentado — **INFORMATIVO** · hipótese registrada

`ForkFrameGen::EvaluateAtPresent` chama `PolicyFromConfig()` em todo quadro, antes do caminho
rápido de "desligado". Oito dos nove getters são `atomic` relaxados sobre cache — custo
desprezível. O nono, `GetString`, pega mutex e copia string: algo entre 30 e 80 ns por quadro,
~5 µs por segundo.

**Não implementar sem evidência.** Fica registrado, com o experimento que o justificaria: perfilar
o caminho de apresentação e mostrar o custo acima do ruído. Se um dia valer, a saída natural é
cachear a `Policy` e invalidá-la pelo callback de mudança que o `ForkConfig` já tem.

---

## 4. Gate obrigatório de pré-release

`tools/fork/gate.sh` — falha imediata em qualquer etapa, na ordem:

| # | etapa | verifica |
|---|---|---|
| 1 | Ambiente | JDK (major, com aviso na divergência com o CI), SDK, NDK e CMake pinados presentes, alvo Rust, `git-sync-deps` do shaderc feito |
| 2 | Consistência de toolchain | NDK/CMake/SDK iguais entre `build.gradle.kts`, os dois workflows, o script e a doc |
| 3 | Kotlin | `:app:compileGithubDebugKotlin` |
| 4 | C/C++ ARM64 | `:app:externalNativeBuildGithubDebug` |
| 5 | Link | a `.so` existe e não tem símbolo indefinido |
| 6 | Testes | fork tests quando o build host for viável; caso contrário delega ao CI e DIZ isso |
| 7 | APK debug | `:app:assembleGithubDebug` |
| 8 | Release | `:app:assembleGithubRelease` (LTO + R8) |
| 9 | Estrutura | conflitos residuais, propriedades duplicadas, declaração sem definição |
| 10 | Relatório | resumo por etapa, com SHA e versões de toolchain |

No CI: um job `gate` do qual o job de release **depende**, para que uma tag não possa nascer
com o gate vermelho.

---

## 5. Ordem de execução

1. **V-07** — escrever o gate. É o que impede a repetição de tudo que já custou três alphas.
2. **V-01** — rodar o gate sobre HEAD. Fecha a lacuna do commit no topo.
3. **V-10** — corrigir `raItems` na troca de jogo. Único achado de comportamento; risco real de
   mostrar dado de outro jogo.
4. **V-11** — testes de migração de configuração (`hwAat`, `UseMac*`, `lsfgFlowScale`, global × por jogo).
5. **V-02** — remover o `|| true` dos dois workflows.
6. **V-04 + V-05 + V-03** — checksum das command-line tools, `rust-toolchain.toml`, JDK alinhado.
7. **V-09** — medir o `native.log` e reclassificar o ganho, ou retirar a afirmação.
8. **V-12 + V-06** — os dois testes que faltam e a duplicação de versões no script.
9. **V-08** — decidir o lugar da suíte host.
10. **V-13** — só se houver medição.

Fora desta lista e ainda pendente do lado do aparelho: **o teste de barreiras de textura**, que
segue bloqueando as Etapas 2-4 de `regras-driver-com-validade.md`.
