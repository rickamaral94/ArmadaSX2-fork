# Fase 0.5 — gate de correctness do recompilador ARM64

**Origem:** branch `phase-0.5-arm64-correctness-hardening` (commit `85a9238`, 18/08/2026),
incorporado ao branch de trabalho por merge e endurecido em seguida.

**Problema que ele resolve:** o JIT ARM64 desta base foi traduzido do x86-64 com forte auxílio de
LLM — declarado no README do ARMSX2 e registrado como risco nº 1 no relatório da Fase 1. Bugs de
recompilador não aparecem em compilação; aparecem como corrupção gráfica, física errada ou
travamento em um jogo específico. O CI Android da Fase 0 prova que **compila**; este prova que o
recompilador **continua calculando o mesmo**.

---

## 1. O que roda

`recompiler_tests` — alvo já existente na árvore (`tests/ctest/core/recompilers/`): 132 arquivos,
~1.830 casos, incluindo conformance contra console de FPU do EE, cache do EE, VU e IOP.

Em runner **arm64 nativo** (`ubuntu-24.04-arm`). Isso não é detalhe: em x86 o recompilador testado
não é o que roda no celular.

## 2. Como ele julga — critério diferencial

Duas pernas produzem relatórios JUnit; um terceiro job compara os **conjuntos**:

| Situação | Veredito |
|---|---|
| Falha no candidate que não falha no baseline (**regressão**) | **reprova** |
| Teste presente no baseline e ausente no candidate (**sumiço**) | **reprova** |
| Falha nos dois (**preexistente**) | reporta, não reprova |
| Falha no baseline que passa no candidate (**correção**) | reporta, não reprova |
| Teste novo no candidate | reporta, não reprova |

Por que não "zero falhas": a suíte tem falhas herdadas que não são nossas. Um gate cronicamente
vermelho é um gate ignorado — e um gate ignorado é pior que nenhum, porque dá a sensação de
cobertura. A regra de **sumiço** existe porque apagar ou desabilitar o teste que incomoda é a
maneira mais fácil de fingir verde, e é exatamente o que a política do fork não aceita.

Implementação: `tools/fork/compare-gtest-xml.py`. Roda offline, sem CI:

```sh
python3 tools/fork/compare-gtest-xml.py baseline.xml candidate.xml
```

## 3. Qual baseline

`tools/fork/UPSTREAM_BASE` — o commit do ARMSX2 sobre o qual este fork está montado, atualizado
por `tools/fork/sync-upstream.sh --merge` a cada sincronização com o upstream.

A pergunta que o gate responde é *"nossas mudanças quebraram algo que o upstream fazia certo?"*, e
essa é a única referência que responde isso sem envelhecer sozinha. Um SHA fixo no YAML passaria a
comparar contra uma base que já não é a nossa no primeiro merge do upstream.

Para investigar um caso específico, `workflow_dispatch` aceita `baseline_ref` e sobrepõe o arquivo.

## 4. Quando dispara

Só quando código de emulação ou de build muda: `pcsx2/**`, `common/**`, `tests/**`, `3rdparty/**`,
`cmake/**`, `CMakeLists.txt`, o próprio workflow e o comparador — em push nos nossos branches, em
PRs, e sob demanda.

Pela regra 1 do `FORK.md` nós não encostamos no núcleo, então na prática ele roda **depois de um
merge do upstream** ou **quando alguém mexeu onde não deveria**. São as duas horas em que ele
importa; commits de documentação e de UI Android não pagam ~90 min de CI arm64 à toa.

## 5. Ajustes aplicados sobre a versão original

| Item | Antes | Agora | Motivo |
|---|---|---|---|
| Baseline | SHA `1032f021` fixo no YAML | lido de `tools/fork/UPSTREAM_BASE`, com override manual | envelhecia a cada merge do upstream |
| Gatilho | só push no branch `phase-0.5-*` | nossos branches + PRs, filtrados por caminho | não protegia o branch onde o código muda |
| Veredito | leitura humana de dois logs | job `compare` que reprova em regressão/sumiço | comparação manual não é gate |
| Timeout | 90 min | 150 min | deps Qt/FFmpeg + Release LTO não cabiam no primeiro run |
| Deps / ccache | sem cache | cache de `~/deps` + ccache (mesmo padrão do `linux_build_qt.yml` do upstream) | o caminho quente cai para minutos |
| Paralelismo | `--parallel 2` | `--parallel $(nproc)` | o runner tem mais núcleos que isso |
| Falha de teste | derrubava o job | `continue-on-error` no passo de teste | falha de teste é dado para o comparador; o veredito é dele. Falha de **build** continua derrubando, e o comparador reprova por relatório ausente |

LTO foi **mantida** ligada, como na versão original: as duas pernas usam flags idênticas, então a
comparação continua válida, e o cache resolve o custo depois do primeiro run.

## 6. Registro de execuções

| Data | Run | Baseline | Candidate | Regressões | Sumidos | Preexistentes |
|---|---|---|---|---|---|---|
| — | — | — | — | — | — | — |
