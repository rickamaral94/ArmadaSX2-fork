# Fase NR-0 — reconstrução temporal clean-room para Adreno 740

**Baseline:** `6f0c8c66bc13d583cfca7ae5957699ab1ca0674f` (`v0.19.0-alpha.19`).

**Branch:** `fork/phase-neural-rendering-a740`.

**Estado:** prévia experimental, instalada no APK e **desligada por padrão**.

**Filosofia:** *Compatibility first. Performance measured. Features optional.*

## 1. O que isto é — e o que não é

Esta fase introduz o **Amaral Temporal Reconstruction (ATR)**, um pós-processamento temporal aberto
para a imagem final do ArmadaSX2. Ele é a fundação mensurável de um renderer neural futuro, não uma
porta do DLSS e não usa código, DLL, modelo, peso ou documentação vazada da NVIDIA.

DLSS 5 recebe dados que um jogo moderno produz para o próprio renderer. Um PS2 não produz motion
vectors, e o emulador não pode inventá-los sem evidência. A primeira versão usa somente entradas
que o pipeline já possui legitimamente:

- quadro final atual;
- quadro original anterior (`OriginalHistory1`);
- resultado espacial anterior (`ATRSpatialFeedback`);
- tamanho da fonte, tamanho de saída e contador de quadros.

Nenhuma mudança em EE, IOP, VU, temporização, áudio ou input. O recurso vive depois de
ShadeBoost/FXAA, no caminho `.slangp` que o renderer Vulkan já executa antes da apresentação.

## 2. Pipeline

```
quadro PS2 final
  -> ATR Spatial (viewport)
       reconstrução bilinear + detalhe local limitado + guarda contra ringing
  -> ATR Temporal (viewport)
       diferença atual/anterior -> confiança de movimento
       clamp do histórico na vizinhança atual -> mistura conservadora
  -> present normal do ArmadaSX2
```

### Passe espacial

Cinco amostras formam um Laplaciano local. O ganho cai nas bordas fortes e o resultado é limitado
ao mínimo/máximo da vizinhança, com uma expansão pequena. Isso recupera contraste sem criar halos
brancos em HUD, texto ou linhas de alto contraste.

### Passe temporal

Compara luminância e crominância do quadro original atual e anterior. História só participa onde a
diferença está abaixo do limiar. Antes da mistura, o histórico é limitado à vizinhança do quadro
atual; em uma desoclusão ou corte, um pixel antigo não pode puxar a saída para uma cor que o quadro
novo não contém. No primeiro quadro o peso temporal é zero.

Sem motion vectors, não existe reprojeção. Por isso o padrão temporal é 18%, o máximo oferecido é
45% e o shader prefere perder estabilidade a produzir ghosting.

## 3. Instalação e uso

O APK copia `assets/shaders/amaral-temporal-reconstruction/` para
`<dataRoot>/shaders/amaral-temporal-reconstruction/` na inicialização. A cópia preserva as relações
relativas do preset e é idempotente.

No aplicativo:

1. **Configurações -> Renderer -> Shader Chain**;
2. ligar a cadeia;
3. escolher `amaral-temporal-reconstruction-a740`;
4. abrir os parâmetros somente se quiser sair do perfil conservador.

Parâmetros:

| Parâmetro | Padrão | Papel |
|---|---:|---|
| `ATR_DETAIL` | 0,32 | contraste/detalhe recuperado pelo passe espacial |
| `ATR_EDGE_GUARD` | 0,70 | reduz halos em bordas e HUD |
| `ATR_STABILITY` | 0,18 | peso máximo do histórico em região estática |
| `ATR_MOTION_THRESHOLD` | 0,060 | quanta diferença ainda conta como região estável |
| `ATR_HISTORY_CLAMP` | 0,025 | tolerância do clamp contra ghosting/desoclusão |

O caminho tecnicamente funciona em outros backends cobertos por librashader, mas esta fase qualifica
somente **Vulkan em Adreno 740**. Nenhum default global seleciona o preset.

## 4. Medição

O bloco `@@FORK@@ hygiene` passa a informar `atr=on|off`. ATR ligado não contamina a medição; é o
lado candidato do A/B. Ele continua registrando upscale, blending e qualquer condição que torne o
número inválido.

Protocolo mínimo no Odin 2 Portal:

1. driver, resolução, blending e trecho do jogo idênticos;
2. Frame Generation desligado para isolar ATR;
3. três rodadas de 60 s desligado e três ligado, com a ordem AB/BA alternada;
4. cinco minutos de descanso entre rodadas;
5. comparar FPS real, 1% low, stutters, GPU e temperatura;
6. capturar a mesma cena para procurar ringing, ghosting, flicker e perda de texto.

Canários prioritários:

- **Final Fantasy X / Persona 4:** HUD e texto estáticos;
- **Burnout 3 / Gran Turismo 4:** movimento rápido;
- **Shadow of the Colossus:** câmera, vegetação e pior caso de carga;
- **Silent Hill 3:** névoa, baixo contraste e transições escuras.

Critério para continuar à NR-1:

- zero crash, frame preto ou erro de compilação do preset;
- nenhuma regressão visual bloqueante nos canários;
- queda de FPS real menor que 3% e sem piora material de 1% low no mesmo trecho;
- estabilidade perceptível em regiões estáticas sem rastro visível em movimento.

Sem essa rodada, a conclusão é apenas **“pipeline implementado para medição”**.

## 5. Verificação automatizada

`tools/fork/check-bundled-shaders.py` roda localmente e no workflow Android. Ele valida:

- contagem e numeração dos passes;
- todos os caminhos relativos e confinados à pasta do preset;
- presença dos estágios vertex/fragment;
- bindings de descriptor sem duplicidade;
- todo `*Feedback` apontando para um passe anterior com alias.

No CI, o mesmo gate exige `glslangValidator` e compila os dois estágios para SPIR-V Vulkan 1.1.

O build do APK prova empacotamento e compilação C++/Kotlin. A compilação real do preset por
librashader e a qualidade/custo continuam sendo gates de dispositivo.

## 6. Próximas fases possíveis

- **NR-1:** medidor de custo específico da cadeia e perfis Performance/Balanced/Quality baseados no
  A/B real;
- **NR-2:** campo de movimento aberto em baixa resolução, reusando o estudo de optical flow da
  Fase 9, com rejeição de HUD e cortes;
- **NR-3:** pequeno modelo treinado com dados próprios/públicos e convertido para compute Vulkan ou
  QNN, somente se superar o shader determinístico dentro do orçamento da A740.
