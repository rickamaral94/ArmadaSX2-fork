# Jogos-canário e protocolo de teste

Lista fixa de títulos usada como *gate* de regressão. Ela existe por um motivo específico: o JIT
ARM64 da base é recente e foi traduzido do x86-64 com forte auxílio de LLM (declarado no README do
ARMSX2). Bugs de JIT não aparecem como erro de compilação — aparecem como corrupção gráfica,
travamento ou desvio de física em jogos específicos. Sem uma linha-base, é impossível saber se uma
regressão veio de um merge do upstream ou de uma mudança nossa.

**Nada aqui é distribuído.** BIOS e jogos são dumps do próprio hardware do testador.

---

## 1. A lista

| # | Jogo | Por que está na lista | Alvo primário |
|---|---|---|---|
| 1 | Metal Gear Solid 2: Sons of Liberty | Citado nos comentários do próprio build Android da base como sensível a codegen (crash "JIT-adjacent") | JIT / EE |
| 2 | Metal Gear Solid 3: Snake Eater | Engine pesada, streaming, muitos efeitos de tela cheia | JIT / GS |
| 3 | Shadow of the Colossus | VU1 pesado, framerate instável, muito movimento de câmera | VU / FG (pior caso) |
| 4 | God of War II | Um dos usos mais pesados do GS no catálogo | GS / texture cache |
| 5 | Gran Turismo 4 | 60 fps travados, movimento rápido e uniforme | FG 60→120 / frame pacing |
| 6 | Burnout 3: Takedown | 60 fps com movimento extremo e blur | FG (ghosting) |
| 7 | Grand Theft Auto: San Andreas | 30 fps, mundo aberto, streaming constante | FG 30→60 / estabilidade longa |
| 8 | Final Fantasy X | HUD 2D estático sobre 3D, FMV, texto pequeno | FG (artefato de HUD) |
| 9 | Persona 4 | HUD e texto densos, UI 2D | FG (artefato de HUD) / legibilidade |
| 10 | Kingdom Hearts II | Efeitos de partícula e blending intensos | GS / blending |
| 11 | Devil May Cry 3 | 60 fps, ação que depende de timing de input | latência de input |
| 12 | Tekken 5 | 60 fps, janela de input curtíssima | latência de input |
| 13 | Ratchet & Clank 3 | VU-heavy, geometria densa | VU |
| 14 | Jak II | Streaming + VU, mundo contínuo | VU / estabilidade |
| 15 | Okami | Cel-shading, blending incomum, pós-processamento | GS / renderer HW |
| 16 | Silent Hill 3 | Névoa, profundidade, iluminação escura | depth / FG (FG-C) |
| 17 | Resident Evil 4 | Mistura pré-renderizado + 3D | GS |
| 18 | SSX 3 | 60 fps, movimento vertical rápido | FG / frame pacing |

Cobertura por subsistema: JIT/EE (1, 2), VU (3, 13, 14), GS/blending (4, 10, 15, 17), 30 fps para
FG (7, 8, 9), 60 fps para FG (5, 6, 11, 12, 18), HUD (8, 9), depth (16).

---

## 2. Protocolo de execução

Cada rodada = **3 execuções** do mesmo trecho, resultado reportado como mediana.

1. **Trecho fixo de 60 s** por jogo, a partir de um save state versionado pelo testador
   (não do começo do jogo — carregamento poluiria a medição).
2. Dispositivo em estado controlado: bateria acima de 50%, sem carregar, modo avião,
   **5 minutos de descanso entre execuções** (throttling térmico invalida comparação).
3. Registrar sempre, e nunca misturar:
   - **FPS real** (velocidade da emulação) e **FPS apresentado** (o que chega à tela);
   - frametime médio, 1% low, stutter (frames acima de 2× o frametime alvo);
   - erros/avisos de validação Vulkan;
   - tempo de compilação de shader no primeiro minuto;
   - crashes e artefatos visuais (com print).
4. Comparação de imagem: GS dump do mesmo trecho antes/depois, comparado quadro a quadro.
   `pcsx2-gsrunner` (já na árvore) reproduz dumps de forma determinística — é a única comparação
   que não depende de reflexo humano.

## 3. Regra de decisão

- **Regressão em qualquer canário = bloqueio.** Não se avança de fase.
- Regressão que reproduz também no ARMSX2 upstream, sem nossas mudanças: reportar ao upstream,
  registrar aqui como "conhecida/upstream", não corrigir com hack local.
- Ganho de desempenho sem A/B nas mesmas condições não conta como ganho.

## 4. Registro de execuções

Preencher a cada fase concluída. Vazio até o primeiro dispositivo real entrar no circuito.

| Data | Fase | Commit | Dispositivo | SoC / GPU | Android | Driver | Resultado |
|---|---|---|---|---|---|---|---|
| — | 0 | — | — | — | — | — | pendente (requer hardware) |
