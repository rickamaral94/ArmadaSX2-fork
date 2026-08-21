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
- cada worker recebe seu próprio `VkPipelineCache`, eliminando a sincronização externa exigida
  para acesso simultâneo a um único cache;
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

- solicitações e hits de cache;
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
