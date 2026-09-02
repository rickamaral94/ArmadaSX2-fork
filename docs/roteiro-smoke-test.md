# Roteiro de smoke test no aparelho

Existe porque toda medição desta fase depende de hardware, e "testei e pareceu bom" não é
resultado — não distingue duas builds, não sobrevive a uma semana, e não permite que outra pessoa
confirme. Cada linha aqui produz uma **evidência observável**: uma linha de log, um número, ou um
comportamento binário que não depende de julgamento.

Os jogos-canário e o subsistema que cada um estressa estão em [jogos-canario.md](jogos-canario.md).

---

## 0. Antes de começar — o cabeçalho da sessão

Sem estes campos, o relato não pode ser comparado com nenhum outro. Anote **antes**:

| campo | como obter |
|---|---|
| SHA-256 do APK | está no corpo da release; confira o que instalou |
| commit | idem |
| aparelho e SoC | ex.: AYN Odin 2 Portal · Snapdragon 8 Gen 2 (QCS8550) |
| versão do Android | Configurações → Sobre |
| driver | Turnip (versão Mesa) ou Qualcomm proprietário |
| modo de energia | performance / balanceado — **o mesmo em toda a sessão** |
| temperatura inicial | OSD, com o aparelho parado há ≥5 min |
| bateria | ≥50% e **na tomada ou fora dela, sempre igual** |

Throttling térmico invalida comparação: uma medição a 40 °C e outra a 60 °C não são o mesmo
experimento. Se a temperatura final passar 15 °C da inicial, marque o resultado como suspeito.

---

## 1. Smoke — o app funciona?

Ordem deliberada: cada passo depende do anterior. Pare no primeiro que falhar e relate.

| # | Ação | Evidência observável |
|---|---|---|
| 1 | Abrir o app | chega na biblioteca sem fechar sozinho |
| 2 | Biblioteca lista os jogos | as capas carregam |
| 3 | Bootar um jogo | chega ao menu inicial |
| 4 | Pausar e retomar | áudio volta sincronizado, sem repetir trecho |
| 5 | Ir para segundo plano e voltar | imagem volta; **não** pode ficar em preto |
| 6 | Salvar estado e carregar | volta ao mesmo ponto, sem corrupção gráfica |
| 7 | Trocar de renderer (se suportado) | reinicia o GS sem derrubar a VM |
| 8 | Encerrar a VM e voltar à biblioteca | sem travar |
| 9 | Girar a tela / recriar a superfície | reapresenta sem piscar preto permanente |
| 10 | Conectar a segunda tela | tiles aparecem com dados do jogo em cena |
| 11 | Trocar de jogo com a segunda tela ligada | **nenhum dado do jogo anterior aparece** |
| 12 | RetroAchievements: logar | contagem aparece; o tile de último desbloqueio fica em `—` |
| 13 | Desbloquear uma conquista | só ela é anunciada |
| 14 | Fechar o app logo após uma rajada de log | `native.log` contém as últimas linhas antes do fim |

O passo 11 é o que fecha o bug corrigido nesta fase. O 14 é o que valida o coalescimento do
`native.log`: sem ele, o arquivo terminaria antes do fim da rajada.

---

## 2. Barreiras de textura — o teste que destrava as Etapas 2-4

**Este é o teste de maior valor pendente.** Ele decide se a regra
`vk-turnip-attachment-self-read` — provada no Adreno 650 com Mesa 26.1.2, sem limite de versão
nem de modelo — está custando desempenho no Adreno 740 sem motivo.

1. Fixes → *Disable Framebuffer Fetch* **desligado**
2. Fixes → *Sobrescrever Barreiras de Textura* **Ligado**
3. Bootar um jogo com efeito de framebuffer (God of War II ou Kingdom Hearts II)
4. Procurar no `native.log`:

```
fbfetch=yes(in-tile)   texbarrier=on
```

| resultado | significa |
|---|---|
| `fbfetch=yes(in-tile)` e `texbarrier=on` | o Adreno 740 **suporta**, e a regra estava custando uma cópia de render target por draw com feedback |
| `fbfetch=no` ou corrupção gráfica | a regra está certa para este modelo também; registrar e encerrar a hipótese |

Rodar **os dois lados**: com a opção ligada e desligada, mesmo jogo, mesma cena, mesmo savestate.
Anotar frame time médio, P95 e 1% low de cada lado. Sem os dois lados não é A/B, é impressão.

---

## 3. A/B de upscaler

Marcador no log: `@@ANDROID_SGSR@@`.

Ordem: Desligado → FSR 1 → SGSR → SGSR Edge, **mesma cena, mesmo savestate**, 60 s cada.
Anotar por passagem: FPS renderizado, frame time médio, P95, 1% low, temperatura final.

Não use FPS apresentado por frame generation nesta comparação — ele mede outra coisa.

---

## 4. LSFG — e por que 25% não é uma recomendação universal

O padrão 25% do `lsfgFlowScale` saiu de medição **em um único aparelho** (Odin 2, Adreno 740,
Turnip): 5,3-5,5 ms contra 14,2-14,7 ms a 100%. Isso justifica o padrão **naquela classe de GPU**
e não em geral.

Para cada valor — 25%, 50%, 75%, 100% — anotar:

* FPS **renderizado** e FPS **apresentado**, sempre os dois e nunca um no lugar do outro;
* custo médio de geração, P95, P99;
* quadros recusados e transições do controlador (`engaged=% transitions=N` no log);
* temperatura e se houve throttling;
* estabilidade após 20 min contínuos.

E olhar, não só medir: HUD, texto pequeno, movimento rápido, partículas, transparência, mudança
de cena, câmera girando, vídeos, objetos surgindo. Um número melhor com HUD fantasmagórico é uma
regressão, não um ganho.

---

## 5. Formato do relato

```
APK sha256 ......
commit ......            aparelho ......        Android ......
driver ......            modo de energia ......  temp inicial/final ...... / ......

passo 1  ok
...
passo 11 FALHOU — tile de conquistas mostrou "12/40" do jogo anterior por ~1 s
         evidencia: native.log linha 4412, @@ANDROID_...
```

Um passo sem evidência observável conta como **não testado**, não como aprovado.
