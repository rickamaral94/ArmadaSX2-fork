# Fase A — criação assíncrona experimental de pipelines Vulkan

> Filosofia: **Compatibility first. Performance measured. Features optional.**

## Estado e baseline

- branch de trabalho: `fork/phase-a-async-vulkan-pipelines`;
- baseline do fork: `7a1a0d498a4d9fc631278215839b70c9c2140237` (`v0.14.0-alpha.14`);
- baseline upstream: `2a987266923b25933b74e56d8e887b9e36d3626c`;
- árvore limpa antes da criação da branch;
- `tools/fork/fork-diff.sh`: 72 arquivos upstream modificados/removidos, 67 arquivos próprios,
  14.607 inserções e 468 remoções; nenhum núcleo protegido apontado pelo script.

Este documento foi escrito antes da implementação. Nenhum código de outro emulador é copiado.
O desenho parte do comportamento e dos contratos do próprio renderer Vulkan deste repositório.

## Evidência no código

`GSDeviceVK::GetTFXPipeline()` consulta `m_tfx_pipelines` e, no primeiro uso de uma chave,
executa `CreateTFXPipeline()` de forma síncrona na thread GS. A função resolve os módulos de shader,
monta todo o estado e chama `vkCreateGraphicsPipelines()` usando o cache global. O bind ocorre logo
depois e não existe estado intermediário entre “ausente” e “pronta”.

Os registros da Fase 9 mostram 305 criações residuais de pipeline com compilação de fonte igual a
zero. No mesmo recorte, EE, GS, VU e GPU não ficaram saturados. Isso sustenta a hipótese de espera
serial no driver; ainda não prova ganho, segurança em todos os drivers ou melhora sustentada.

## Limite desta fase

A experiência move apenas a criação do objeto `VkPipeline` para fora da thread GS. A resolução e a
compilação GLSL -> SPIR-V dos módulos, assim como a seleção/criação de render pass, continuam na
thread GS. Assim, mapas e arquivos já tratados como GS-only não passam a ser compartilhados.

Não são alterados EE, IOP, VU, MTVU, recompilador ARM64, temporização lógica, apresentação ou frame
generation. FPS apresentado continua sem valor como evidência de desempenho da emulação.

## Máquina de estados

Cada chave passa somente pelos estados abaixo:

| Estado | Significado | Transições válidas |
|---|---|---|
| `Missing` | não existe tarefa nem resultado | `Pending` |
| `Pending` | enfileirada ou executando | `Ready`, `Failed` |
| `Ready` | resultado concluído e ainda não consumido | removida ao ser adotada pela thread GS |
| `Failed` | criação falhou ou tarefa foi cancelada | removida ao fallback/retry controlado |

Solicitações duplicadas nunca compilam a mesma chave duas vezes. O ponto de uso espera somente por
uma chave `Pending`; uma pipeline pronta é adotada sem bloqueio. Se a fila estiver desligada,
negada pelo perfil ou indisponível, o caminho síncrono atual é usado integralmente.

## Concorrência e cache

- pool pequeno, configurável e limitado a dois workers;
- tarefas com shaders relacionados usam o mesmo `group_id` e portanto a mesma fila/worker;
- no gate serial, o único worker usa o cache principal exclusivamente até `join`; pools realmente
  paralelos recebem um `VkPipelineCache` privado por worker, eliminando acesso simultâneo;
- os caches dos workers só são mesclados ao cache principal depois de `join`, na thread GS;
- `VkDevice`, layouts, render passes e módulos são imutáveis durante uma tarefa;
- somente a thread GS publica ou destrói pipelines concluídas.

O perfil central de driver é a autoridade do gate. Perfil desconhecido/conservador nega a
experiência. `BrokenMultithreadedShaderCompilation` limita o pool a um único worker: ainda é
possível sobrepor criação de pipeline e preparação do draw, mas não executar chamadas concorrentes
ao driver. A opção continuará `off` por padrão.

## Lifecycle e teardown

`Quiesce` executa nesta ordem:

1. deixa de aceitar tarefas;
2. marca tarefas ainda não iniciadas como falhas/canceladas;
3. espera somente as chamadas ao driver já em andamento terminarem;
4. faz `join` de todos os workers;
5. adota ou destrói resultados órfãos na thread GS;
6. mescla e destrói os caches privados;
7. só então permite destruir módulos, render passes, layouts, cache principal e `VkDevice`.

O mesmo caminho será usado antes do teardown do renderer e da recriação de Surface. Depois de uma
pausa/recriação, o pool pode ser reaberto preguiçosamente no próximo draw; falhar ao reabrir
mantém o fallback síncrono.

## Telemetria

Os contadores não misturam FPS real com apresentado:

- solicitações e hits do cache de seletores (o driver não expõe hit interno neste caminho);
- tarefas enfileiradas e concluídas;
- criações executadas com mais de um worker permitido;
- esperas no ponto de uso e tempo total de espera;
- tempo total de criação no driver;
- falhas e cancelamentos.

O relatório A/B deve comparar pelo menos cinco execuções por cenário, com warm-up separado, p95,
p99, 1% low, stutters, temperatura e identidade do driver. Esta branch não mudará o default sem os
dados de hardware exigidos na Fase B.

## Critérios de aceite antes de habilitar `auto`

- testes de transição, deduplicação, concorrência, cancelamento e shutdown passam;
- Vulkan validation não registra uso após destruição;
- Adreno 740 é medido separadamente em Qualcomm e Turnip;
- pelo menos uma Mali e um aparelho intermediário completam o corpus;
- imagens/hash perceptual não indicam regressão;
- cinco rodadas A/B válidas mostram melhora de cauda sem regressão térmica relevante;
- falha, denylist e lifecycle sempre retornam ao caminho síncrono correto.

Sem esse conjunto, a conclusão será somente “hipótese implementada para medição”, nunca
“desempenho melhor”.

## Relatório de implementação — 2026-08-21

Estado: **experimento implementado, desligado por padrão; sem conclusão de desempenho**.

Verificação local bruta:

| Verificação | Resultado |
|---|---:|
| máquina de estados/fila, execução normal | 7/7 testes passaram, 14 ms |
| máquina de estados/fila, ASan+UBSan | 7/7 passaram, 19 ms |
| máquina de estados/fila, TSan | 7/7 passaram, 18 ms |
| perfil de driver, incluindo gate Qualcomm/Turnip | 13/13 passaram |
| sintaxe de `GSDeviceVK.cpp`, `ForkConfig.cpp` e fila | passou com GCC 13.3 |

O LeakSanitizer não pôde inspecionar leaks neste container porque ele roda sob `ptrace`; ASan e
UBSan foram repetidos com detecção de leaks desligada e passaram. O ambiente não possui `cmake`,
portanto a compilação integral do APK e as suítes completas ficam a cargo dos dois gates da branch:
`Fork · Android arm64 (debug APK)` e `Phase 0.5 ARM64 Correctness`.

Hardware real e A/B: **não executados nesta etapa**. Aparelhos testados: nenhum. Números de p95,
p99, 1% low, stutters, temperatura e consumo: indisponíveis. Por isso `PipelineCompiler.Mode`
permanece `off`, e nenhum default foi alterado.

Limitações conhecidas:

- o Vulkan não informa hit/miss do `VkPipelineCache` neste caminho; `selector_hits` deve significar
  apenas pipeline já pronta ou já enfileirada, nunca hit interno do driver;
- o perfil Android atual aplica `SerializePipelineCreation`, então Qualcomm e Turnip usam um
  worker. O trabalho pode sobrepor preparação do draw, mas chamadas ao driver não são paralelas;
- progresso assíncrono é mesclado/persistido em pause, Surface recreation e teardown. Não há flush
  periódico novo, pois introduzir uma serialização de cache no meio do jogo sem medição repetiria
  o stutter que a experiência procura reduzir;
- promoção para mais de um worker exige retirar a regra de serialização para um driver/versão
  específico somente depois de reprodução e teste em hardware.

## Revisão de código — 2026-08-21 (F3 e F5)

Duas correções em cima da implementação inicial, ambas achadas por leitura e nenhuma por medição.

### F3 — o worker serial usava o cache PRINCIPAL

A primeira versão entregava `g_vulkan_shader_cache->GetPipelineCache(true)` ao worker quando o gate
serializava para um, e só criava caches privados a partir de dois. Isso era seguro **por
coincidência**, não por construção: vale apenas porque todas as outras criações de pipeline
(`CompilePostProcessing`, `CAS`, `FSR1`, `ImGui`, `Convert`, `Merge`, `Interlace`) acontecem no init
do dispositivo, e porque o fallback síncrono de `GetTFXPipeline` é inalcançável enquanto o pool
roda. `vkCreateGraphicsPipelines` exige sincronização externa do `pipelineCache`; bastava um
post-process novo, um reload de shader ou um re-init do ImGui na thread GS para virar comportamento
indefinido — do tipo que as validation layers pegam de forma intermitente e some do relato.

Agora **todo** worker recebe cache privado, inclusive o serial, e a invariante deixa de existir em
vez de ser documentada. A regra saiu da integração Vulkan e virou `ForkPipelineCompiler::PlanCaches`,
função pura com teste.

O custo dessa segurança seria começar frio e recompilar o que o principal já sabe — pagando em
desempenho justamente na medição que a Fase A viabiliza. Por isso os caches privados nascem
**semeados** com `VKShaderCache::GetPipelineCacheData()`; o driver valida o cabeçalho e descarta em
silêncio um blob incompatível, então semear nunca é pior que não semear.

### F5 — adoção nula envenenava o seletor para sempre

`GetTFXPipeline` publicava em `m_tfx_pipelines` o que `WaitAndTake` devolvesse, inclusive zero. Mas
zero não é veredito: significa falha real do driver **ou** cancelamento por teardown. Publicado, o
seletor parava de desenhar pelo resto da sessão, sem nunca mais tentar. Agora uma adoção nula avisa
no log e cai para o caminho síncrono logo abaixo — que é o que "fallback síncrono integral"
significa: o experimento pode falhar, o desenho não.

### Verificação

| Verificação | Resultado |
|---|---|
| fila + regra de caches, ThreadSanitizer | 9/9 verdes |
| teste novo contra o desenho ANTIGO | falha nas 3 asserções, como esperado de um teste de regressão |
| estresse do padrão real (40 rodadas, Quiesce em voo, restart) | 5074 criadas / 5074 contabilizadas, zero vazamento, zero corrida |
| `GSDeviceVK.cpp`, `VKShaderCache.cpp` | análise semântica limpa |
| perfil de driver | 13/13 verdes |
| `fork-diff.sh` | nenhuma mudança em núcleo protegido |

### O que continua valendo

Nenhuma conclusão de desempenho. Duas expectativas a fixar antes de medir:

1. **No Adreno 740 + Turnip o gate resolve para UM worker.** A regra
   `vk-android-shader-serialization` tem `min_android_sdk=1` com vendor e driver `Unknown`, logo casa
   em todo Android Vulkan. `parallel=0` no log é estrutural, não medição — não leia como
   "paralelismo não ajudou", ele não foi tentado.
2. **A janela de sobreposição é o setup de um único draw.** `PrefetchTFXPipelines` roda dentro de
   `DoRenderHW` e `GetTFXPipeline` vem poucas dezenas de linhas depois. Não há submissão
   especulativa, então não se compila adiantado ao longo de vários draws. O ganho existe mas é
   limitado por construção; um delta pequeno não refuta o desenho.

### Pendente (F2), não corrigido aqui

O flush periódico do cache de pipelines para de acontecer com o experimento ligado:
`m_tfx_pipeline_compile_counter` só incrementa na cauda síncrona de `GetTFXPipeline`, inalcançável
com o pool ativo. Sobra o flush de `onPause`. Com caches privados em todos os caminhos, um OOM-kill
antes do próximo `Quiesce` perde tudo que os workers compilaram. Fica registrado como próximo passo.
