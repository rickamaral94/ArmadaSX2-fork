# Fase 1 — Avaliação técnica e escolha da base

**Data:** 18 de agosto de 2026
**Escopo:** relatório prévio à implementação, conforme solicitado (nenhuma linha do emulador foi modificada ainda).
**Filosofia do projeto:** *Compatibility first. Performance measured. Features optional.*

> Evidência: todas as referências a arquivos/linhas abaixo foram extraídas de um clone real do
> `ARMSX2/ARMSX2` no commit `2a98726` (16/08/2026), inspecionado localmente durante esta avaliação.

---

## 1. Base recomendada

**Recomendação: forkar o ARMSX2 (`github.com/ARMSX2/ARMSX2`, GPL-3.0), mantendo dois remotos
permanentes — `armsx2` (upstream direto) e `pcsx2` (upstream de origem) — e organizando 100% das
nossas mudanças em módulos isolados.**

Motivos, em ordem de peso:

1. **É a única base que já satisfaz os pré-requisitos duros do projeto ao mesmo tempo:** licença
   GPL-3.0 com código-fonte integral, JIT ARM64 nativo (EE, IOP, VU0/VU1 + vtlb fastmem), app
   Android in-tree (`platforms/android`, Gradle + Kotlin/Compose) e o renderer Vulkan maduro
   herdado do PCSX2.
2. **O ponto de conexão do libadrenotools já existe e está correto.** O `libadrenotools` está
   *vendored* em `platforms/android/app/src/main/cpp/3rdparty/adrenotools` (BSD-2-Clause) e é
   chamado exatamente onde deveria: `TryOpenAdrenotoolsDriver()` em
   `pcsx2/GS/Renderers/Vulkan/VKLoader.cpp:67`, antes do `dlopen` do `libvulkan.so` do sistema.
   Não precisamos inventar essa camada — precisamos **endurecê-la** (validação, SHA-256, por jogo,
   gating de GPU).
3. **O pipeline de apresentação já está isolado do núcleo.** O present acontece em um único ponto
   (`GSDeviceVK.cpp:1683-1731`), e já existe um gancho de frame generation ali
   (`GSLsfg::PresentWithGeneration`). Ou seja: a arquitetura que queremos
   (`PS2 → GS → Vulkan → FG → Surface`) **já é a arquitetura da base**, sem tocar em EE/IOP/VU.
4. **Atividade real e alta.** Último commit em 16/08/2026 (dois dias antes deste relatório),
   ~25k commits, rebases periódicos sobre o core PCSX2 2.7.
5. **Rastreabilidade com o upstream PCSX2.** O ARMSX2 preserva a árvore do PCSX2
   (`pcsx2/`, `common/`, `3rdparty/`, `pcsx2-qt/`), então merges do upstream continuam viáveis —
   é o mesmo trabalho que o ARMSX2 já faz por nós.

### Ressalva honesta (e por que ela não muda a decisão)

O próprio README do ARMSX2 declara que a tradução do JIT x86-64 → ARM64 foi feita com forte
auxílio de LLMs ("*mechanical translation*"). Isso é um **risco de precisão real**: bugs de JIT
aparecem como corrupção gráfica ou travamento em jogos específicos, não como erro de compilação.

Mitigação (que já faz parte do nosso plano de qualidade, e não é opcional):

- usar os *runners* que já existem na árvore (`pcsx2-gsrunner`, `pcsx2-eerunner`, `pcsx2-vurunner`,
  `tests/`) como *gate* de regressão antes de qualquer merge nosso;
- manter uma lista fixa de jogos-canário e comparar GS dumps antes/depois;
- **nunca** encostar em EE/IOP/VU: se o JIT quebrar, o culpado deve ser sempre o upstream, nunca nós.

Se em algum momento o upstream PCSX2 mergear recompiladores AArch64 oficiais, a estratégia de dois
remotos permite migrar a base sem reescrever nossos módulos — eles são plug-ins do pipeline de
apresentação, não patches no núcleo.

---

## 2. Comparação das bases candidatas

| Critério | **ARMSX2** | **PCSX2 upstream** | **Play!** | **NetherSX2 / Classic / Turnip** | PCSX2_ARM64 (Pontos/Trixarian), PSX2 (izzy2lost), pcsx2-aarch64 |
|---|---|---|---|---|---|
| Licença | GPL-3.0 | GPL-3.0 | MIT | AetherSX2 = **proprietário**; patch NetherSX2 = Unlicense | GPL-3.0 |
| Fonte integral | Sim | Sim | Sim | **Não** (base fechada) | Sim |
| Atividade (ago/2026) | Alta (commit 16/08/2026) | Alta | Moderada, mantenedor único | Morta (AetherSX2 encerrado em 2023) | Baixa/estagnada |
| JIT ARM64 | **Sim** (EE, IOP, VU, vtlb fastmem) | **Não** (só interpretador ARM64) | Sim (JIT próprio) | Sim (fechado) | Parcial/incompleto |
| Android in-tree | Sim (`platforms/android`) | Não oficial | Sim | Sim (APK) | Sim |
| Backend Vulkan | Sim (PCSX2 GSDeviceVK) | Sim | Sim (mais simples) | Sim | Sim |
| Maturidade da emulação | Herdada do PCSX2 (alta) + risco no JIT novo | **Máxima** | Média (menor compat. em 3D) | Alta (PCSX2 antigo) | Herdada, menos testada |
| Pipeline de apresentação modificável | **Excelente** (ponto único de present, gancho FG já existe) | Excelente | Bom | Impossível | Bom |
| Integração libadrenotools | **Já integrada** | Ausente | Existe em builds derivadas | Fechada | Ausente |
| Acompanhar upstream PCSX2 | Bom (mesma árvore) | Trivial | Irrelevante (core próprio) | Impossível | Fraco |
| Fork sustentável por anos | **Sim** | Sim, mas exigiria portar o JIT (multi-ano) | Sim, mas core diferente | **Não** | Não |

**Descartados e por quê:**

- **NetherSX2 / NetherSX2 Classic / NetherSX2-Turnip** — são *patches* sobre o AetherSX2, cujo
  código nunca foi publicado apesar de derivar de PCSX2 (GPL). Não há fonte para forkar, e
  redistribuir builds derivadas é exatamente a "engenharia de redistribuição questionável" que o
  projeto proíbe. **Excluído por licença, não por desempenho.**
- **PCSX2 upstream** — tecnicamente a melhor fundação de precisão, mas sem recompiladores AArch64
  os jogos rodam em interpretador (ordem de magnitude mais lento no Android). Forkar upstream
  significaria escrever o JIT ARM64 do zero antes de chegar à Fase 2. Mantemos como **remoto de
  referência**, não como base.
- **Play!** — MIT, multiplataforma, Vulkan, JIT ARM64, mantenedor ativo; excelente projeto, mas com
  core próprio (não PCSX2), compatibilidade 3D inferior e um único mantenedor. Contraria o objetivo
  explícito de "combinar a maturidade do ecossistema PCSX2".
- **PCSX2_ARM64 / PSX2 / pcsx2-aarch64** — ancestrais ou primos do ARMSX2, menos completos e menos
  ativos. O ARMSX2 é a continuação viva dessa linhagem.

---

## 3. Arquitetura atual do renderer Vulkan (base escolhida)

Fluxo real, do núcleo até a tela:

```
EE/VU (JIT ARM64) → GIF → GSRenderer (thread MTGS, pcsx2/GS/)
    → GSDeviceVK (pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp, 8.577 linhas)
        → VKSwapChain (VkSurfaceKHR sobre ANativeWindow)
            → vkQueuePresentKHR → Android Surface
```

Pontos-chave mapeados:

| Função / arquivo | Papel |
|---|---|
| `Vulkan::LoadVulkanLibrary()` — `VKLoader.cpp:124` | Carrega o ICD Vulkan. **É aqui que o driver é escolhido.** |
| `Vulkan::LoadVulkanInstanceFunctions/DeviceFunctions` — `VKLoader.cpp` | Resolve entry points (`VKEntryPoints.inl`). |
| `GSDeviceVK::Create()` | Cria instância/dispositivo, detecta GPU e publica capacidades (`GSDeviceVK.cpp:3455`). |
| `GSDeviceVK::DoBeginPresent()` — `GSDeviceVK.cpp:2865` | `vkAcquireNextImageKHR`, recriação de swapchain, frame skip. |
| `GSDeviceVK::EndPresent()` — `GSDeviceVK.cpp:3039` | Fecha o render pass do frame (ImGui/OSD incluído). |
| `GSDeviceVK::SubmitCommandBuffer()` — `GSDeviceVK.cpp:1683-1731` | `vkQueueSubmit` → **gancho de FG** → `vkQueuePresentKHR` → `AcquireNextImage()`. |
| `VKSwapChain` (`VKSwapChain.cpp`, 1.052 linhas) | Modo de present, imagens, semáforos, estatísticas (`IsPresentStatsEnabled`, `NotePresent`). |
| `GSGPUProfile*.cpp` (`Common/GSGPUProfileAdreno.cpp`, `…Mali.cpp`, `…PowerVR.cpp`, `GSGPUDriverProfile.cpp`) | Perfis por GPU/driver — base pronta para o nosso `gpu_capabilities`. |
| `VKShaderCache.cpp` | Cache de pipeline/shader em disco (relevante: **precisa ser invalidado por driver**). |
| `ImGuiOverlays.cpp` | Overlay de FPS/estatísticas (`ImGuiOverlays.cpp:132` já consome status de FG). |

Observação importante: o `VKLoader` usa `VK_NO_PROTOTYPES` + ponteiros resolvidos manualmente.
Isso é o que torna a troca de ICD viável — **nada é linkado estaticamente contra `libvulkan.so`**.

---

## 4. Turnip — onde o libadrenotools se conecta

### 4.1 O que já existe (e funciona)

```
Kotlin: CustomDriver.applyToNative()           (CustomDriver.kt:406)
   → NativeApp.setCustomVulkanDriver(...)      (NativeApp.java:368)
      → JNI native-lib.cpp:826
         → Vulkan::SetCustomDriverPath(...)    (VKLoader.cpp:57)
            → TryOpenAdrenotoolsDriver(...)    (VKLoader.cpp:67)
               → adrenotools_open_libvulkan(RTLD_NOW,
                    ADRENOTOOLS_DRIVER_CUSTOM | ADRENOTOOLS_DRIVER_FILE_REDIRECT, …)
            → fallback silencioso para o loader do sistema em qualquer falha
```

Complementos já presentes:

- `CustomDriver.kt` (420 linhas): instala pacotes `.zip` no schema AdrenoTools (`meta.json` +
  `libvulkan_freedreno.so`), com seis fontes remotas (K11MCH1, MrPurple, StevenMXZ, crueter,
  PojavLauncherTeam, ExynosTools) e import manual via SAF (`installFromUri`).
- `GpuInfo.kt`: sonda GL_VENDOR/GL_RENDERER/GL_VERSION via contexto GLES2 descartável e recomenda a
  fonte de driver por família Adreno (6xx / 7xx / 8xx). Já retorna `null` para não-Adreno.
- `DriverManagerSection.kt` (339 linhas): UI de gerenciamento.
- Build: `add_subdirectory(3rdparty/adrenotools)` + `useLegacyPackaging = true` (obrigatório para o
  bypass de namespace do linker).

### 4.2 O que **falta** para atender à especificação deste projeto

| Requisito | Estado | Onde implementar |
|---|---|---|
| SHA-256 do pacote/driver exibido e persistido | **Ausente** (existe só para texture packs) | `CustomDriver.installFromStream()` → gravar em `meta.json` estendido; exibir em `DriverManagerSection.kt` |
| Validação estrutural antes de carregar (ELF ARM64, soname, `vkGetInstanceProcAddr` presente) | Parcial | novo `DriverValidator.kt` + checagem nativa opcional em `TryOpenAdrenotoolsDriver` |
| Mesa version / Vulkan version / GPU **reais do ICD carregado** | **Ausente** (hoje só `meta.json`, que é auto-declarado) | novo `Vulkan::QueryLoadedDriverIdentity()` em `VKLoader.cpp` lendo `VkPhysicalDeviceProperties.driverVersion/apiVersion` + `VkPhysicalDeviceDriverProperties.driverName/driverInfo`, exportado por JNI |
| Seleção **por jogo** | **Ausente** (só global) | `config/Settings.kt` (já tem infraestrutura de override por jogo) + aplicar em pré-boot, antes do primeiro `MTGS::Open` |
| Gating rígido por GPU (esconder em Mali/PowerVR/Xclipse) | Parcial (recomendação, não bloqueio) | `GpuInfo.kt` → `isTurnipCapable()`; UI desabilita a seção inteira |
| Invalidação do shader cache ao trocar de driver | **Ausente** | `VKShaderCache.cpp` — incluir hash do driver na chave do cache |
| Retorno explícito e visível ao driver do sistema | Existe (`setCustomVulkanDriver("", "", "", "")`) | expor como item fixo "System Driver" no topo da lista |
| Relatar por que o driver falhou (hoje só vai ao log) | Parcial | propagar `Error` do `TryOpenAdrenotoolsDriver` para a UI |

### 4.3 Regras de licenciamento adotadas

- `libadrenotools` = BSD-2-Clause → *vendoring* permitido (já está, com atribuição).
- Mesa/Turnip = MIT → redistribuível, mas **não vamos embutir**: mantemos download/import pelo
  usuário, o que evita casar o APK com um driver específico e permite drivers futuros **sem
  recompilar** (requisito explícito, já satisfeito pela arquitetura de pastas `drivers/<id>/`).

---

## 5. Frame Generation — ponto exato de interceptação

**O ponto é `GSDeviceVK::SubmitCommandBuffer()`, `GSDeviceVK.cpp:1683-1731`**: depois do
`vkQueueSubmit` do frame real e **antes** do `vkQueuePresentKHR`. É o único lugar por onde todo
frame apresentado passa.

Dois detalhes da base que são exatamente o que a especificação pede:

1. `const bool has_new_frame = std::exchange(m_present_has_new_frame, false);` — a base **já
   distingue frame novo de frame repetido**. Esse é o mecanismo que impede FG de "inventar"
   suavidade em cima de emulação abaixo da velocidade: sem frame novo, não há interpolação.
2. O gancho retorna `bool`; retornando `false`, o present normal acontece. Ou seja, **a regra de
   segurança de desempenho (descartar o frame gerado e apresentar o real) é implementável sem
   nenhuma mudança estrutural**.

### Prior art na base: `GSLsfg` (e por que não vamos depender dele)

`pcsx2/GS/Renderers/Vulkan/GSLsfg.cpp` (1.259 linhas) implementa frame generation via **Lossless
Scaling**, alimentando a biblioteca com nossas próprias imagens por `AHardwareBuffer`. Tecnicamente
é um ótimo mapa do caminho — e confirma que o ponto de interceptação funciona na prática.

Porém, pelo próprio cabeçalho do módulo: exige que o usuário forneça um `Lossless.dll`
**proprietário** (os shaders de interpolação são lidos desse PE em runtime) e só é habilitado em
**Adreno 7xx+**. Para o nosso projeto isso é inaceitável como recurso principal — o requisito é
"não utilizar código fechado" e o fork precisa ser publicável e utilizável integralmente.

**Decisão:** manter `GSLsfg` intocado (é opcional e desligado por padrão) e construir nosso módulo
`frame_generation` como um **backend paralelo, aberto**, atrás da mesma interface de present.
Um `enum FrameGenBackend { Off, OpticalFlowOpen, Lsfg }` resolve a coexistência sem fork interno.

### Abordagens FG-A / FG-B / FG-C

| Abordagem | Viabilidade na base | Veredito |
|---|---|---|
| **FG-A — só imagem** (frame N-1 + N → optical flow em compute shader → frame intermediário) | Alta: os dois últimos backbuffers já estão disponíveis no swapchain; nenhum dado do core é necessário | **Começar por aqui.** Único caminho que funciona em 100% dos jogos e não toca no núcleo |
| **FG-B — dados do renderer** | O renderer HW tem depth buffer, e o GS expõe `DISPFB`/`DISPLAY` (offsets de scroll) e informação de merge/interlace. Mas o PS2 **não tem motion vectors** e boa parte do conteúdo é sprite 2D | Rejeitado como abordagem isolada — exigiria hacks no core, o que o projeto proíbe |
| **FG-C — híbrido** | Usar FG-A + apenas o que já é gratuito: depth do HW renderer (quando disponível) para rejeitar flow inconsistente, e o retângulo de `DISPFB` para detectar cortes de cena/scroll global. Zero mudança em EE/IOP/VU | **Meta de médio prazo (Fase 6+)**, depois que FG-A estiver estável |

Ganho decisivo do FG-C sem custo: **detecção de cena/HUD**. Regiões estáticas (HUD do PS2) são
justamente onde a interpolação produz artefato; máscara por diferença temporal + depth resolve
grande parte disso.

---

## 6. Tecnologias de Frame Generation — comparação

| Tecnologia | Licença | API/Plataforma | Entradas exigidas | Adreno/Turnip? | Veredito |
|---|---|---|---|---|---|
| **Arm NFRU** (Neural Graphics SDK for Game Engines) | MIT | Vulkan; Android AArch64 suportado | **motion vectors da engine** + optical flow + rede neural | Projetado para **GPUs Arm** com aceleração neural; em outras GPUs depende da *ML Emulation Layer* | **Não viável agora.** Falha em dois pontos: exige MVs que o PS2 não produz, e o alvo de hardware é Mali, não Adreno. Reavaliar quando/se houver caminho por extensões ML genéricas |
| **AMD FidelityFX Frame Interpolation (FSR3)** | MIT | D3D12 primeiro, Vulkan disponível | 2 backbuffers + **depth e MVs dilatados do FSR3Upscaler** + Optical Flow + **proxy swapchain** | Sem otimização móvel; orçamento de GPU de desktop | **Não adotar como está.** O acoplamento ao upscaler e ao proxy de swapchain conflita com nosso `VKSwapChain`. Útil como referência de algoritmo (disocclusion, frame pacing) |
| **Optical flow próprio** (compute shaders: block-matching hierárquico / Lucas-Kanade piramidal + warp bidirecional) | Nosso, GPL-3.0 | Vulkan 1.1, compute puro | Apenas 2 frames | **Sim** — sem extensões exóticas, roda em Turnip e no driver Qualcomm | **Escolha recomendada para FG-A.** Custo e qualidade controláveis por resolução do campo de fluxo |
| `VK_NV_optical_flow` | — | Vulkan | 2 frames | **Indisponível** em Adreno | Descartado |
| Redes tipo RIFE/GMFSS via ONNX/NNAPI | MIT/variadas | NNAPI/Vulkan compute | 2 frames | Possível, mas latência e memória altas | Descartado por ora; reavaliar como backend opcional |
| **LSFG** (já na base) | Biblioteca aberta, **shaders proprietários do usuário** | Vulkan/AHardwareBuffer | 2 frames | Adreno 7xx+ | Manter como backend opcional herdado; **não** como recurso principal |
| Fallback trivial: repetição de frame / BFI | — | — | — | Sim | Útil apenas como linha-base de comparação A/B |

**Conclusão da seção:** implementar `frame_generation` com backend próprio de optical flow
(FG-A), interface preparada para múltiplos backends, e NFRU/FSR mantidos como itens de pesquisa
com critério de entrada objetivo (rodar em Adreno sem camada de emulação ML e sem exigir MVs).

---

## 7. Arquitetura proposta

```
┌──────────────────────────────────────────────────────────────┐
│ EMULATION CORE  (EE · IOP · VU0/VU1 · JIT ARM64 · timers)    │  ← intocado
│ áudio · input · contadores sincronizados com FRAMES REAIS     │
└───────────────────────────┬──────────────────────────────────┘
                            │ GIF / GS packets
┌───────────────────────────▼──────────────────────────────────┐
│ GS RENDERER (GSRenderer / HW)                                │  ← intocado
└───────────────────────────┬──────────────────────────────────┘
                            │
┌───────────────────────────▼──────────────────────────────────┐
│ VULKAN RENDERER  (GSDeviceVK · VKSwapChain · VKShaderCache)  │
└───────────────────────────┬──────────────────────────────────┘
                            │ VkImage do frame REAL + semáforo
        ┌───────────────────▼───────────────────┐
        │  MÓDULO: presentation_metrics         │  real FPS, frametime,
        │  (frametime real vs apresentado)      │  1% low, dropped/dup
        └───────────────────┬───────────────────┘
                            │
        ┌───────────────────▼───────────────────┐
        │  MÓDULO: frame_generation             │  has_new_frame?
        │   ├─ backend: optical_flow (aberto)   │  dentro do budget?
        │   ├─ backend: lsfg (opcional/legado)  │  senão → bypass
        │   └─ política: Off / Auto / 2x        │
        └───────────────────┬───────────────────┘
              generated frame(s) + real frame, em ordem
                            │
┌───────────────────────────▼──────────────────────────────────┐
│ PRESENTATION PIPELINE  (frame pacing · vkQueuePresentKHR)     │
└───────────────────────────┬──────────────────────────────────┘
                            ▼
                    ANDROID SURFACE / DISPLAY

Carregamento do driver (fora do caminho por frame, executa 1x no boot):
  android_driver_manager (Kotlin) → JNI → adrenotools_backend (VKLoader)
      → gpu_capabilities (GSGPUProfile*) decide se Turnip é sequer oferecido
```

### Mapa de módulos → caminhos reais

| Módulo (nome pedido) | Onde vive | Natureza |
|---|---|---|
| `android_driver_manager` | `platforms/android/.../com/armsx2/driver/` (Kotlin) | novo, evolui `CustomDriver.kt` |
| `adrenotools_backend` | `pcsx2/GS/Renderers/Vulkan/VKLoader.cpp` + `.../cpp/3rdparty/adrenotools` | existente, a endurecer |
| `gpu_capabilities` | `pcsx2/GS/Renderers/Common/GSGPUProfile*.cpp` + `GpuInfo.kt` | existente, a estender |
| `frame_generation` | `pcsx2/GS/Renderers/Vulkan/FrameGen/` (novo diretório) | novo |
| `frame_interpolation` | `pcsx2/GS/Renderers/Vulkan/FrameGen/backends/` | novo |
| `presentation_metrics` | `pcsx2/GS/Renderers/Vulkan/PresentationMetrics.{h,cpp}` | novo, aproveita `VKSwapChain::PresentStats` |

**Regra de ouro do fork:** o diff contra o ARMSX2 deve ser majoritariamente *arquivos novos*.
Alterações em arquivos existentes ficam restritas a: 1 bloco em `SubmitCommandBuffer`, 1 bloco em
`VKLoader`, chaves de config, entradas de UI e strings de overlay.

---

## 8. Roadmap (fases pequenas e testáveis)

| Fase | Entrega | Critério de aceite |
|---|---|---|
| **0** | Fork criado, dois remotos configurados, CI de build Android arm64 (APK debug) | APK compila e inicializa a BIOS em um dispositivo Adreno |
| **1** | Linha-base de compatibilidade: lista de jogos-canário + GS dumps de referência + roteiro de teste | Baseline reproduzível registrada em `docs/` |
| **2** | `presentation_metrics`: frametime real, FPS real, dropped/duplicated, tempo de present | Overlay mostra números reais; **zero** impacto medível com métricas desligadas |
| **3** | `gpu_capabilities`: detecção Adreno 6xx/7xx/8xx, Vulkan, Android; gating rígido de Turnip | Em Mali/PowerVR a seção de driver não aparece |
| **4** | `android_driver_manager` v2: validação, SHA-256, identidade real do ICD (Mesa/Vulkan/GPU), erros na UI, invalidação de shader cache | Importar driver inválido é rejeitado antes do boot; troca de driver não reutiliza cache |
| **5** | Driver por jogo + retorno a System Driver em 1 toque | Override por jogo aplicado antes do primeiro `MTGS::Open` |
| **6** | `benchmark`: framework A/B (System vs Turnip A vs Turnip B) com FPS, frametime, 1% low, stutter, erros Vulkan, tempo de compilação de shader | Relatório CSV/JSON exportável pelo app |
| **7** | `frame_generation` esqueleto: gancho no present, política Off/Auto/2x, bypass e budget, **sem interpolar** (duplica frame) | FG ligado não altera FPS real nem áudio/input; overlay separa Real vs Presented |
| **8** | Backend `optical_flow` (FG-A) com warp bidirecional + máscara de oclusão | 30→60 estável em Adreno 7xx com custo de GPU medido e publicado |
| **9** | Máscara de HUD/cena (FG-C leve), perfis automáticos por GPU/jogo, 60→120 | Ganho comprovado em A/B; regressão zero na lista-canário |

Nada avança de fase sem A/B medido e sem *kill switch*.

---

## 9. Riscos e mitigações

| Risco | Impacto | Mitigação |
|---|---|---|
| **Latência de input** (FG segura o frame real para interpolar) | Alto — arruína jogos de ação | Medir e exibir latência adicional; usar apresentação do frame real **primeiro** quando possível; FG desligado por padrão |
| **Frame pacing ruim** | Suavidade pior do que sem FG | FG só habilita com frametime real estável (variância abaixo de limiar) por N frames |
| **Ghosting / artefatos em movimento rápido** | Qualidade visual | Máscara de oclusão + limiar de magnitude de fluxo; cortes de cena detectados descartam a interpolação |
| **Artefatos de HUD** (2D estático sobre 3D em movimento) | Muito visível no PS2 | Máscara temporal de regiões estáticas (FG-C); teste dedicado em jogos com HUD pesado |
| **Sincronização Vulkan** (semáforos/fences no caminho de present) | Deadlock/corrupção | Reusar a estrutura já provada do gancho existente; validação com camadas Vulkan habilitadas em builds de debug |
| **Swapchain** (recriação, rotação, `OUT_OF_DATE`) | Crash | FG desativa a si mesmo em qualquer recriação e só religa após N frames estáveis |
| **Incompatibilidade de Turnip** (versão de Mesa quebra jogo/dispositivo) | Regressão silenciosa | SHA-256 + identidade real do ICD nos logs; A/B obrigatório; volta ao System Driver em 1 toque; fallback automático já existe no `VKLoader` |
| **Regressões do upstream** (ARMSX2 rebaseia sobre PCSX2 2.7+) | Merge doloroso | Diff mínimo, módulos em arquivos novos, CI que roda a lista-canário a cada merge |
| **Risco de precisão do JIT ARM64 da base** | Jogos quebrados sem culpa nossa | Baseline de canários; bugs reportados ao ARMSX2, nunca corrigidos com hack no nosso lado |
| **Uso excessivo de GPU / throttling térmico** | FPS real cai — o cenário que a especificação proíbe | Budget por frame; FG se desliga sozinho quando o FPS real cai abaixo do alvo; log de temperatura/clock quando disponível |
| **Dependência proprietária herdada (LSFG)** | Publicação do fork | Não usar como recurso principal; permanece opcional e desligado |

---

## 10. Recomendação final

1. **Forkar `ARMSX2/ARMSX2` (GPL-3.0)** como base, com remotos `armsx2` e `pcsx2` para rebase.
2. **Não reimplementar a Fase 2 do zero.** O caminho `libadrenotools` já existe e está no lugar
   certo (`VKLoader.cpp:67`); nosso trabalho é torná-lo confiável: validação, SHA-256, identidade
   real do ICD, seleção por jogo, gating rígido de GPU e invalidação do shader cache.
3. **Construir `frame_generation` como backend aberto próprio**, com FG-A (optical flow em compute)
   primeiro, aproveitando o ponto de interceptação em `GSDeviceVK.cpp:1683-1731` e o flag
   `has_new_frame` que já garante que FG nunca mascare emulação lenta.
4. **NFRU e FidelityFX ficam em pesquisa**, com critério objetivo de entrada: rodar em Adreno sem
   camada de emulação ML e sem exigir motion vectors da engine. Hoje nenhum dos dois passa.
5. **Ordem de execução:** métricas antes de FG; gating de GPU antes de driver; benchmark A/B antes
   de qualquer default novo. Nenhum recurso entra ligado por padrão sem evidência medida.

**Próximo passo sugerido (Fase 0):** criar o fork, configurar os remotos e o CI de build Android
arm64, e registrar a lista de jogos-canário. Aguardo sua aprovação para iniciar.
