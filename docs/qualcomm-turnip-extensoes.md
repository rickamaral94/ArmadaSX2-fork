# Extensões Qualcomm no Turnip — o que dá para usar no Adreno 740

> Filosofia: **Compatibility first. Performance measured. Features optional.**

Levantamento feito em 2026-09-01 a partir de uma lista de tecnologias Qualcomm candidatas a
integração (SGSR, AFME, extensões `VK_QCOM_*`, denoisers, SDKs de IA). O resultado é
majoritariamente **negativo**, e está registrado por isso: resultado negativo não documentado é
trabalho que alguém refaz — a pergunta "por que não usamos o Adreno Frame Motion Engine?" vai
voltar, e a resposta precisa estar escrita em vez de ser redescoberta.

Aparelho de referência: **AYN Odin 2 Portal — QCS8550, Adreno 740, Turnip Mesa 26.3.0-devel**.

## O filtro que decide tudo

Não é "a Qualcomm publicou?" nem "a licença permite?". É **o driver que rodamos implementa?**

O Turnip é o driver Mesa aberto para Adreno, e implementa um subconjunto pequeno das extensões
de fornecedor da Qualcomm. Verificado em `src/freedreno/vulkan/tu_device.cc` do Mesa (`main`):

```
QCOM_fragment_density_map_offset
QCOM_image_processing
QCOM_multiview_per_view_render_areas
QCOM_multiview_per_view_viewports
QCOM_render_pass_shader_resolve
```

**Cinco.** Todo o resto da lista Qualcomm não existe neste driver.

| candidata | Turnip | observação |
|---|---|---|
| `QCOM_frame_extrapolation` (AFME) | ausente | exclusiva do blob proprietário |
| `QCOM_motion_estimation` | ausente | exclusiva do blob proprietário |
| `QCOM_tile_shading` | ausente | e é hardware **Adreno 840+**; o 740 não qualifica |
| `QCOM_tile_memory_heap` | ausente | idem |
| `QCOM_image_processing2` | ausente | a versão 1 existe; a 2 não |
| `QCOM_filter_cubic_weights` / `_clamp` | ausente | |
| `QCOM_render_pass_transform` | ausente | |
| `QCOM_rotated_copy_commands` | ausente | |
| `QCOM_ycbcr_degamma` | ausente | |
| `QCOM_tile_properties` | ausente | |
| `QCOM_image_processing` | **presente** | ver abaixo |
| `QCOM_fragment_density_map_offset` | presente | ver abaixo |

As duas peças que a pesquisa original apontou como chave — extrapolação de quadros e estimação de
movimento — **são as duas que o Turnip não tem**. Não há trabalho no emulador que as alcance por
este driver. Só trocando para o blob proprietário, o que reabre a discussão de ROAA lenta e perde
o driver que hoje entrega a melhor compatibilidade.

## `VK_QCOM_image_processing` está disponível, e ainda assim não vale

O gate no Turnip é `device->info->props.has_image_processing`. Em
`src/freedreno/common/freedreno_devices.py`, `has_image_processing = True` aparece em `a7xx_gen2`
e `a7xx_gen3`, e **não** em `a7xx_gen1`. O `GPUId(740)` é declarado com `[a7xx_base, a7xx_gen2]`,
então **o Adreno 740 tem a extensão**.

Ela expõe três coisas, e nenhuma resolve um problema que temos:

- **`textureWeightedQCOM`** — amostragem ponderada por uma *imagem de pesos*. Serve a filtros de
  ordem alta com pesos **fixos** (bicúbico, Lanczos estático). Os pesos do SGSR 1 são calculados
  por pixel a partir da direção da borda (`weightY` recebe `std`, derivado da vizinhança), então
  não há imagem de pesos para dar a ela. Não mapeia.
- **`textureBlockMatchQCOM`** — block matching, primitiva de estimação de movimento. É o caminho
  de frame generation, que este fork deixou de perseguir; e esbarra no mesmo obstáculo estrutural
  de sempre: no PS2 o HUD vem misturado ao framebuffer.
- **`textureBoxFilterQCOM`** — filtro caixa acelerado. Não usamos filtro caixa em lugar nenhum do
  caminho de apresentação.

Registrado como **hipótese arquivada, não como impossibilidade**: se um dia existir um passe nosso
com pesos estáticos numa vizinhança grande, `textureWeightedQCOM` é o primeiro lugar a olhar, e a
Qualcomm reporta ganhos grandes contra shader manual. Hoje esse passe não existe.

## As outras três, e por que são inertes aqui

- **`QCOM_fragment_density_map_offset`** — foveação: sombrear menos em regiões da tela. É o
  **oposto** do objetivo de qualidade de imagem deste fork, e a imagem do PS2 não tem região de
  menor importância como um HMD tem periferia.
- **`QCOM_multiview_per_view_viewports` / `_render_areas`** — estéreo e VR. Um emulador de PS2 tem
  uma vista.
- **`QCOM_render_pass_shader_resolve`** — resolve de MSAA no shader. O caminho de apresentação
  não faz MSAA da imagem do PS2.

## SGSR

- **SGSR 1** — implementado. É passe único, precisa só da textura de cor, e entrou como
  `GSUpscaler::SGSR1` ao lado do FSR 1. Ver o cabeçalho de `bin/resources/shaders/vulkan/sgsr1.glsl`
  para os três desvios do original e o commit que o introduziu.
- **SGSR 2** — **não implementar por enquanto.** Exige motion vectors, depth, matriz de câmera e
  jitter. O PS2 não fornece nada disso: EE e VU transformam os vértices antes do GS, que recebe
  coordenadas já em tela. A saída sugerida — gerar motion vectors por optical flow — é frame
  generation com outro nome, com a mesma fragilidade (ghosting, cortes de câmera, HUD no mesmo
  framebuffer). E custa: ~1 ms no caminho de dois fragment shaders **mais** o optical flow, num
  aparelho que as sessões medem com `gpu=100%` justamente nas cenas que precisariam dele.

  **Experimento que mudaria a decisão:** medir, com o profiler no aparelho, o custo de um passe de
  optical flow em 1/4 de resolução na cena pesada de Shadow of the Colossus. Se couber em menos de
  1 ms com a GPU já saturada, a conta muda. Antes disso é especulação.

## O que da Qualcomm vale mais, e não é código

**Snapdragon Profiler.** Captura de frame e contadores de hardware no Adreno real. Responde o que
`tools/fork/gsdump-stats.py` estruturalmente não consegue — ele lê o fluxo de comandos, não o
custo na GPU. Três hipóteses abertas viram número com ele:

1. quanto custa o caminho de cópia de render target que `UseRenderTargetCopyForFeedback` força;
2. quanto custa o SGSR 1 por frame no 740 (a Qualcomm publica ~0,3 ms no 8 Gen 2, a medir aqui);
3. quanto custaria subir `accurate_blending_unit` de Basic para Full.

## Conclusão

Da lista inteira, **nada é para implementar agora**. O maior ganho ligado à Qualcomm não é integrar
tecnologia nova: é **parar de recusar a que o driver já oferece**. O `VK_EXT_rasterization_order_
attachment_access` está presente e habilitado no aparelho, e a regra `vk-turnip-attachment-self-read`
o descarta em favor de uma cópia de render target por draw de feedback — com base em um A/B feito
em Adreno 650 / Mesa 26.1.2, generalizado para todo Turnip sem faixa de versão nem de modelo.

Esse teste continua pendente e vale mais que qualquer item desta página.

## Fontes verificadas

- `tu_device.cc` e `freedreno_devices.py` do Mesa (`main`), lidos em 2026-09-01 via
  gitlab.freedesktop.org.
- `github.com/SnapdragonStudios/snapdragon-gsr`, commit `d926f074bc`, BSD-3-Clause.
- Log de sessão do Odin 2 de 2026-08-22: `ROAA=yes fbfetch=NO(barrier-fallback) texbarrier=off`.
