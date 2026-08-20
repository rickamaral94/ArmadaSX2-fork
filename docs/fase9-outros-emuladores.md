# Fase 9 — O que outros emuladores maduros de Android fazem, e o que trouxemos

Etapa 4 de 4 do ciclo da alpha 13. O critério foi o mesmo das etapas anteriores: **ler o
código**, não as notas de versão. Cada item abaixo diz de onde veio, se foi adotado, e —
quando não foi — por qual motivo concreto.

Emuladores lidos neste ciclo: **DuckStation** (`src/util`, HEAD `5f3db8e`), **PPSSPP**
(`Common/GPU`), **Dolphin** (ubershaders), **ARMSX3 0.9.3** (etapa anterior),
**Eden/Yuzu** (etapa anterior, frame generation).

---

## 1. O achado principal: tínhamos duas fontes de verdade sobre drivers

Não veio de um emulador específico — veio de comparar a nossa organização com a deles.

DuckStation concentra os desvios por driver em `GPUDevice::DisableFeaturesForDriver`.
PPSSPP concentra em `bugs_.Infest(...)`, num bloco só, dentro de `thin3d_vulkan.cpp`.
Cada um tem **um** lugar onde se decide "este driver é quebrado assim".

Nós temos dois, e eles não se falavam:

- `GSGPUDriverProfile.cpp` — a tabela de regras, 27 entradas, versionada e comentada.
- `GSDeviceVK.cpp` — verificações de `vendorID`/`driverID` escritas à mão, cada uma com
  a sua medição em aparelho no comentário.

Auditoria (grep de `DriverWorkaround::<nome>` fora dos três arquivos de perfil): **9 dos 15
workarounds não tinham consumidor nenhum**. O bit acendia, ia para o log, e não mudava nada.

Três consequências, todas reais:

**a) Um bug de verdade, no PowerVR.** `vk-powervr-proprietary` declara
`BrokenPushDescriptors` / `UseDescriptorSets` desde sempre. Mas o código só desligava push
descriptors para ARM (`0x13B5`) e para um Adreno de driver desconhecido — Imagination
(`0x1010`) nunca foi verificado. Ou seja: a GPU seguia pelo caminho que a tabela chama de
quebrado, e ninguém percebia, porque ninguém lia a tabela.

*Corrigido:* `ProcessDeviceExtensions` agora resolve o perfil e respeita `UseDescriptorSets`.
A consulta é **aditiva de propósito** — a tabela só pode tirar push descriptors, nunca
devolver. As duas verificações manuais que ficam carregam medição em aparelho que a tabela
não tem (a do Adreno é uma remedição explícita que **derrubou** um desligamento anterior
mais amplo), e medição ganha de tabela.

*Sem efeito no aparelho-alvo:* nenhuma regra Adreno liga `UseDescriptorSets`. Odin 2, blob
e Turnip, continuam com push descriptors. Há teste fixando isso nas duas direções.

**b) O `workarounds=0x…` do log reporta INTENÇÃO, não execução.** Isto me afeta
diretamente: li esse campo na etapa 2 para decidir o que cada driver pagava. A conclusão
da etapa 2 sobrevive — `UseRenderTargetCopyForFeedback` tem três consumidores, é aplicado
de verdade — mas a leitura só era válida por sorte. `GSGPUProfile.h` agora abre com a
classificação dos 15 bits em três grupos (aplicado pelo perfil / aplicado em outro lugar /
ainda não implementado) e com o comando de grep que produz a lista, para ela não apodrecer.

**c) `DisableProvokingVertex`** dizia exatamente o que o código já fazia, sobre exatamente o
mesmo conjunto. Passou a ser consultado também — não muda comportamento hoje, mas impede que
uma edição na tabela **re-habilite** a extensão no blob em silêncio.

---

## 2. PPSSPP #10421 — a regra de colorWriteMask estava estreita demais

`GSDeviceVK::CheckFeatures` já implementa o defeito de `colorWriteMask` ignorado sob teste
de profundidade, com a condição certa: Adreno, não-Turnip, e (pré-6xx **ou** blob abaixo de
`0x801EA000`). A citação do PPSSPP já estava no comentário.

A tabela, porém, só descrevia a metade 5xx (`vk-adreno5xx-depth-stencil`). Num Adreno 6xx/7xx
com blob antigo o renderer aplicava o contorno e o log **não reportava**. Adicionada
`vk-qualcomm-colormask-before-512-490` (modelo 600+, blob abaixo de 512.490.0, que é
`0x801EA000` decodificado — e é o primeiro blob que o PPSSPP registra como bom, no Adreno 620).

A armadilha que essa regra existe para evitar está fixada em teste: **o Mesa reporta a própria
versão** (26.x), numericamente muito abaixo de 512.490. Uma regra keyed só na versão dispararia
em todo aparelho Turnip do planeta, e em silêncio. A regra é keyed em
`MobileGpuDriver::QualcommProprietary`; o teste prova que essa chave segura.

---

## 3. Corroboração independente da etapa 2

PPSSPP infesta `SUBPASS_FEEDBACK_BROKEN` em **todo Qualcomm, incluindo Turnip** — a flag
`turnip` é calculada logo acima e deliberadamente não é usada para isentar este bug. O
comentário deles: seguiram todas as regras de self-dependency e feedback loop da spec e
mesmo assim há artefato nas bordas dos triângulos, no Adreno.

É exatamente o que a etapa 2 concluiu lendo as nossas duas regras: o self-read de attachment
quebra nos **dois** drivers, e `UseRenderTargetCopyForFeedback` não é pedágio do Turnip.
Terceiro projeto, mesma conclusão, medida em hardware diferente.

DuckStation reforça o outro lado: desliga `dynamic_rendering`, `dynamic_rendering_local_read`,
`fragment_shader_interlock`, `maintenance5` e `push_descriptor` nos drivers **proprietários**
Qualcomm/ARM/Imagination — e **nunca** no Mesa. O Turnip continua sendo o driver que precisa
de menos desvios; isso é diferente de ser sempre mais rápido, e continua valendo.

---

## 4. Avaliado e recusado (com motivo)

**Compressão Zstandard do pipeline cache** (DuckStation `ClosePipelineCache`). O que nos
interessava do trecho deles — não reescrever quando nada mudou — nós já temos, e com mais:
comparação de tamanho antes de escrever, limite de 120 s entre flushes, escrita atômica via
`.tmp` + `rename`, chave por driver, e descarte do blob quando o cache de SPIR-V é
descartado. A compressão sobraria: custaria CPU na thread do GS para poupar disco que não
está faltando. **Somos os que estão à frente neste caminho** — DuckStation nem sequer separa
o cache por driver (`{lower_api_name}_{type}`), o que é o problema que a nossa 8.6 resolveu.

**`VK_KHR_present_mode_fifo_latest_ready`** (PPSSPP habilita a extensão). Recusado por dois
motivos: no próprio PPSSPP é só encanamento — a extensão é habilitada e o modo aparece num
`switch`, mas nada o seleciona; e para nós ele seria **ativamente errado com FG**, porque o
modo apresenta o quadro mais recente pronto e descarta os anteriores — ou seja, jogaria fora
exatamente o quadro gerado. Fica registrado como candidato para o caminho **sem** FG, onde
seria latência menor com vsync, e só depois de alguém medir.

**Ubershaders do Dolphin.** O truque é renderizar com um shader genérico enquanto o
especializado compila em segundo plano. Não há equivalente barato no GS do PS2: o nosso TFX
já *é* o mega-shader, e as variantes existem porque a especialização é o que o torna viável.
Sem um "genérico" mais barato que o especializado, não há o que trocar.

---

## 5. O maior item que NÃO cabe nesta alpha

**Criação de pipelines em segundo plano, como o PPSSPP.** `VulkanRenderManager` mantém uma
thread de compilação (`CompileThreadFunc`) que agrupa os pipelines pendentes por par
(vertex, fragment) — "os do mesmo par devem ficar na mesma thread, pelo menos na NVIDIA" — e
despacha `CreateMultiPipelinesTask` no pool. O desenho só bloqueia no ponto de uso, via
`WaitForPipelines()`.

Isto ataca de frente o achado da etapa 3 — *nada satura, o gargalo é serialização* — e os
305 pipelines que sobram no perfil dos 2 primeiros minutos depois que o cache de SPIR-V
passou a funcionar. Mas `GSDeviceVK::GetTFXPipeline` é síncrono por construção e chamado de
dentro do desenho; transformá-lo em assíncrono é reescrever o caminho de bind, não ajustar
um parâmetro. **Fica como o candidato número 1 do próximo ciclo**, com medição antes e A/B
depois, como todo o resto.

---

## Mudanças desta etapa

| Arquivo | O quê |
|---|---|
| `GSDeviceVK.cpp` / `.h` | `BuildMobileDriverContext()`, fonte única do contexto; perfil consultado em `ProcessDeviceExtensions` para `UseDescriptorSets` e `DisableProvokingVertex` |
| `GSGPUDriverProfile.cpp` | `vk-qualcomm-colormask-before-512-490` (tabela 27 → 28 regras) |
| `GSGPUProfile.h` | Como ler `workarounds=0x…`: os três grupos de bits, e o grep que mantém a lista honesta |
| `gs_gpu_driver_profile_tests.cpp` | 5 testes novos: os dois lados do limite 512.490, a armadilha do Turnip, PowerVR pedindo o fallback, Adreno mantendo push descriptors nos dois drivers |
