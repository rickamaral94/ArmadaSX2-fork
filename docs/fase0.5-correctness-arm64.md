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
| Teste que **executava** no baseline e não executa no candidate (**desativado**) | **reprova** |
| Falha nos dois (**preexistente**) | reporta, não reprova |
| Falha no baseline que passa no candidate (**correção**) | reporta, não reprova |
| Teste novo no candidate | reporta, não reprova |

Por que não "zero falhas": a suíte pode ter falhas herdadas que não são nossas. Um gate
cronicamente vermelho é um gate ignorado — e um gate ignorado é pior que nenhum, porque dá a
sensação de cobertura.

As regras de **sumiço** e **desativado** existem porque apagar ou desabilitar o teste que incomoda
é a maneira mais fácil de fingir verde. A segunda foi acrescentada depois do primeiro run completo,
que revelou **38 testes já desabilitados** na suíte herdada: um `DISABLED_` continua aparecendo no
XML como `testcase`, então uma comparação que só olhasse presença de nomes não veria diferença
nenhuma — a suíte encolheria em silêncio e o total continuaria idêntico. O comparador agora
distingue *presente* de *executado*.

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
| Caminho do binário | `./build/bin/tests/recompiler_tests` fixo — **exit 127 nas duas pernas** | localizado com `find` após o build, com erro explícito se sumir | `add_pcsx2_test` não sobrescreve o diretório de saída: o binário cai em `build/bin/`. Procurar tira essa classe de quebra do caminho crítico |
| Falha de teste | derrubava o job | `continue-on-error` no passo de teste | falha de teste é dado para o comparador; o veredito é dele. Falha de **build** continua derrubando, e o comparador reprova por relatório ausente |

LTO foi **mantida** ligada, como na versão original: as duas pernas usam flags idênticas, então a
comparação continua válida, e o cache resolve o custo depois do primeiro run.

## 6. Registro de execuções

| Data | Run | Baseline | Candidate | Resultado |
|---|---|---|---|---|
| 18/08/2026 08:49 | [32118350621](https://github.com/rickamaral94/Ps2-fork/actions/runs/32118350621) | `1032f021` | `85a9238` | **inválido** — as duas pernas construíram os 845 alvos com sucesso (deps 39 min, build 10 min) e morreram com exit 127 ao invocar o binário no caminho errado. Nenhum teste chegou a executar. Corrigido em seguida. |
| 18/08/2026 10:28-10:45 | 32126935351, 32127080351, 32128354911 | `2a98726` | vários | **cancelados** pelo grupo de concorrência — cada push superava o run anterior. Nenhum veredito. |
| 18/08/2026 10:57 | [32129287911](https://github.com/rickamaral94/Ps2-fork/actions/runs/32129287911) | `2a98726` | `19117a2a` | **APROVADO** — 1940 testes, 0 falhas, 38 desabilitados, 0 erros nas duas pernas. 0 regressões, 0 sumidos, 0 preexistentes. Suítes do fork na perna candidate: `presentation_metrics_tests` e `fork_config_tests` (9/9) verdes. |

### Leitura do primeiro veredito válido (18/08, run 32129287911)

**A base ARM64 do ARMSX2 passa em 1940 de 1940 testes de recompilador, com zero falhas herdadas.**
Isso é a notícia relevante da fase: o risco nº 1 do relatório da Fase 1 — JIT traduzido com auxílio
de LLM — não se manifesta em nenhum caso da suíte de conformance contra console (FPU do EE, cache
do EE, VU, IOP). Não prova que o JIT está correto; prova que ele está correto **onde o upstream
sabe verificar**, e nos dá uma linha-base sem ressalvas: qualquer falha futura é nossa.

Ressalva honesta: **38 testes estão desabilitados** na suíte. São áreas que o upstream sabe que
ainda não passam. Elas não entram no nosso número e o gate agora impede que esse conjunto cresça em
silêncio.

Duração: ~53 min por perna a frio (deps 37 min, build 11 min, testes 2 s). O cache de dependências
e o ccache foram salvos ao final, então as próximas execuções devem ficar na casa dos 15 min.

Nota sobre o primeiro run (inválido): mesmo que tivesse funcionado, ele não validaria mudança nenhuma —
o único commit do branch era o próprio workflow, então as duas pernas testariam código idêntico.
O valor dele era calibrar o harness, e foi exatamente isso que ele entregou: apontou um defeito
real antes de qualquer promessa de cobertura.
