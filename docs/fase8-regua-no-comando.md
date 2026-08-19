# Fase 8 — a régua no comando do backend que apresenta

A Fase 7 construiu a régua: quando é seguro apresentar um quadro gerado, e por quê não, quando
não é. Ela calculava, registrava e a UI mostrava — e o backend que **de fato apresenta** quadros
gerados seguia apresentando do mesmo jeito, sem consultá-la.

Esta fase fecha os três buracos entre a régua e a realidade. Nenhum deles é hipótese: os três
estão no código de antes, e cada um tem uma linha exata onde se lê o problema.

## 1. A régua não mandava em nada

```cpp
ForkFrameGen::EvaluateAtPresent(/*supported=*/true, has_new_frame);   // calcula...
if (GSLsfg::IsActive() && GSLsfg::PresentWithGeneration(...))          // ...e é ignorada
```

O degrau que existe para impedir **"22 FPS reais mostrando 44"** não impedia nada. Agora:

```cpp
const bool generation_allowed = backend_active && framegen.state == State::Engaged;
```

Recusar devolve o quadro ao caminho normal, que o conta como `Real` ou `Duplicate`. É uma condição
só, e é a fase inteira.

### O buraco no histórico

Recusar tem uma consequência que o gate criava sozinho: `PresentWithGeneration` zerava o histórico
de quadros quando via um intervalo sem conteúdo novo — só que ele não vê os quadros para os quais
não é chamado. Sem tratar isso, a régua recusaria por segundos (emulação lenta, ritmo instável) e,
ao retomar, o backend costuraria o quadro de **antes** da recusa com o de **depois**, apresentando
exatamente um intermediário inventado — no instante em que o usuário voltou a ter fluidez para
julgar. Daí `GSLsfg::NoteGenerationDeclined()`, chamado só quando quem recusa é a régua: se quem
declina é o backend, ele já cuidou do próprio histórico lá dentro.

## 2. O FPS apresentado estava subestimado

O comentário que estava no código dizia o problema com todas as letras:

> O quadro REAL é contado aqui; os interpolados que o LSFG apresenta por dentro não, porque ele não
> os reporta. (…) com LSFG ligado o FPS real continua correto e o apresentado fica SUBESTIMADO.

Preferir um número faltando a um número inventado era a decisão certa enquanto não havia como
contá-los. Mas havia: o backend já conta `presented_generated` — **contados, não deduzidos do
multiplicador**, com as lições difíceis já aprendidas (`VK_SUBOPTIMAL_KHR` é sucesso; um acquire com
timeout zero derruba todos os quadros sob FIFO). Ele só guardava o número para o overlay dele.

Agora o mesmo funil alimenta `GSPresentationMetrics::NotePresented(Generated)`, um por quadro
confirmado na tela. Os **reais** continuam sendo contados só pelo chamador: reportá-los aqui também
dobraria justamente o número que não pode ser tocado.

## 3. O orçamento de tempo era decorativo

`FrameGen.BudgetMs` compara contra `snapshot.generation_avg_ms`. `NoteGenerationCost()` existia,
estava documentado, tinha teste — e **ninguém a chamava**. O degrau `OverBudget`, que suspende FG
quando gerar passa a roubar tempo da emulação, nunca podia disparar.

O custo passou a ser medido de ponta a ponta, incluindo as duas esperas de idle. Elas *são* o
custo: sem semáforo entre dispositivos no Android, o bloqueio é o mecanismo, e cronometrar só a
chamada do backend contaria a parte barata e esconderia a cara. Laço fechado:

```
custo medido -> média na janela -> Decide -> Suspended
```

## 4. O que NÃO mudou, e é o ponto

O FPS **real** não é tocado por nada disto. `GSPresentationMetrics` só registra intervalo entre
quadros quando o tipo é `Real`, então quadro gerado não entra em média, mínimo, máximo nem no 1%
low — por construção, não por cuidado. O teste
`GeneratedFramesDoNotMoveASingleRealNumber` roda a **mesma** linha do tempo real duas vezes, uma
limpa e uma com um quadro gerado intercalado, e exige igualdade exata nos seis números reais e
crescimento no apresentado. Se essa igualdade quebrar, o número que o projeto usa para julgar
desempenho passou a ser contaminado pelo recurso que ele deveria julgar.

## 5. Configuração e migração

`FrameGen.Mode` continua com padrão `off`, e `off` agora significa **nenhum quadro gerado** — não
mais "a régua não opina". Consequências tratadas:

- Ligar a chave de Frame Generation na tela grava `auto` junto, quando o modo ainda estava `off`.
  Sem isso o usuário liga o recurso, o backend sobe, a régua fica em `off` e nada é gerado — a
  chave estaria mentindo. Desligar volta os dois para `off`.
- Política e backend saem na **mesma escrita**. Dois callbacks separados seriam dois
  `apply(s.copy(...))` a partir do mesmo estado, e o segundo desfaria o primeiro.
- Config gravada **antes** desta chave existir, com o backend já ligado, migra para `auto` uma
  única vez. Um `off` herdado faria o recurso parar de gerar para quem já o usava, sem mensagem e
  sem pista — e `auto` é o que essa pessoa já tinha na prática, menos os casos que o projeto chama
  de fracasso.

`EvaluateAtPresent` ganhou caminho rápido para `!supported`: sem backend capaz de apresentar,
nenhuma política muda o resultado, e ler as métricas por quadro para concluir isso cobraria de todo
mundo por um recurso que ninguém ligou. O motivo continua separando "desligado" (escolha do
usuário) de "incompatível" (do aparelho).

## 6. Verificação

**115/115** na suíte (fork + apresentação), com casos novos para: os quadros gerados não moverem
nenhum número real; todo estado que não seja `Engaged` pedir zero quadros — que é exatamente o
número que o `GSDeviceVK` consulta; e "sem backend" ser distinguível de "desligado".

O que **não** dá para verificar aqui: se o custo real da geração cabe no orçamento em um Adreno de
verdade, e se o `Auto` engata e desengata em um ritmo agradável em vez de piscar. Isso é log de
aparelho — e agora os números para julgar existem e estão separados.

## 7. Próximo

Fase 9: backend próprio de optical flow (FG-A), sem depender de arquivo de terceiro. A régua desta
fase é o que vai impedir esse backend de "ter sucesso" às custas da emulação — e, agora, é o que
governa qualquer backend que entre depois dele.

## 8. Adendo (8.1) — o log não tinha o que se pedia

Ao pedir os primeiros logs de aparelho, o defeito apareceu do lado errado da câmera: **o binário
nunca escreveu no log os números que a pergunta exigia**. As métricas da Fase 2 e a decisão da
Fase 7 só existiam no *overlay*, o benchmark da Fase 6 não tem UI que o acione, e o Android nem
arquivo de log tem — `SetFileOutputLevel` nunca é chamado, então o `logcat` é o único canal.
Pedir "mande o log" para responder "a geração cabe no orçamento?" era pedir um dado que não
existia.

`ForkDiagnostics` escreve um bloco a cada `Diagnostics.IntervalSeconds` (10 s por padrão):

```
@@FORK@@ identity  gpu='Adreno 740' turnip=Supported requested='turnip' active=Turnip unexpected=no mesa=25.2.0 sha256=…
@@FORK@@ real      fps=48.30 frametime_avg=20.70ms 1%low=31.40ms min=19.90ms max=33.10ms
@@FORK@@ presented fps=92.70 real_frames=48 generated=44 duplicated=1
@@FORK@@ framegen  mode=auto engaged=91.7% transitions=1 dominant=Ativo. gen_avg=3.80ms gen_worst=7.90ms budget=6.00ms floor=25.0fps
```

Quatro decisões de formato, cada uma por um motivo:

- **Prefixo `@@FORK@@` fixo.** O `Console` sai por stdout e o Android o redireciona para o
  `logcat` sob a tag `STDOUT` — filtrar por tag não acha nada, filtrar por texto acha.
- **Real e apresentado em linhas separadas.** A mesma regra do overlay, e ela vale mais aqui: um
  log é lido fora de contexto, meses depois, por quem não participou da conversa.
- **Transições contadas, não despejadas.** "O Auto fica piscando?" é a pergunta; registrar cada
  mudança produziria centenas de linhas por segundo exatamente no caso patológico. `transitions=1`
  é estável, `transitions=39` é piscando.
- **`gen_worst` ao lado de `gen_avg`.** A média esconde o pico, e é o pico que estoura o orçamento
  e suspende a geração — no exemplo acima, média 3,80 ms dentro do teto de 6,00 e ainda assim
  suspensões, porque o pior caso foi 7,90.

Alimentado com a **mesma** decisão que governa o backend, não com uma recalculada: um diagnóstico
que refizesse a conta poderia explicar uma suspensão com um número que não a causou.


## 9. Adendo (8.2) — o piso era o número errado

O primeiro log de aparelho (Odin 2 Portal, Adreno 740, Turnip Mesa 26.3.0-devel) fechou a Fase 0
— BIOS booto, jogo booto, `CustomDriverActive` sem fallback — e de quebra expôs um erro de projeto
na régua.

O degrau contra mascarar lentidão comparava **FPS real absoluto** contra um piso de 25. Só que:

| situação | FPS real | velocidade | o piso em FPS dizia | o certo |
|---|---|---|---|---|
| jogo nativo de 30, rodando perfeito | 30 | 100% | passa (por pouco) | **passa** — é onde FG mais ajuda |
| jogo de 60, rodando pela metade | 30 | 50% | **passa** | recusar — é a regra do projeto |
| jogo nativo de 20, rodando perfeito | 20 | 100% | recusa | passar |

Um número não distingue "o jogo é assim" de "o aparelho não dá conta" — os três casos acima
produzem FPS reais parecidos. O que distingue é a **velocidade**: `fps / taxa alvo da máquina`,
que é exatamente o que `PerformanceMetrics::GetSpeed()` já calculava contra 59,94 / 50.

Agora são dois degraus, com papéis distintos:

- **`FrameGen.MinSpeedPercent`** (90%) — o degrau principal, e o que a regra do projeto realmente
  quer dizer. `BelowFullSpeed`.
- **`FrameGen.MinRealFps`** (15) — sobrevive com outro papel: **latência**. Interpolar segura o
  quadro novo até produzir o do meio, então o atraso em milissegundos cresce quando a taxa cai —
  ~17 ms a 60 FPS, ~67 ms a 15. Abaixo daqui a fluidez não paga o input lag. `BelowMinimumRealFps`.

O valor de 90% é um ponto de partida declarado, não uma medição: será ajustado com os dados da
alpha 2. O que não é chute é a mudança de GRANDEZA — essa o aparelho provou.


## 10. Adendo (8.3) — contexto suficiente para o número significar algo

A primeira análise de log real gastou uma rodada inteira descobrindo, na unha, que a sessão media
estava com o despejo de texturas ligado — 246 arquivos gravados em disco durante a partida. Todo
número daquela sessão era lixo, e o log não dizia isso.

Duas linhas novas no bloco:

```
@@FORK@@ hygiene   MEDICAO CONTAMINADA: despejo-de-texturas(grava em disco por draw) | upscale=2.75x blend=1
@@FORK@@ load      speed=87.5% vps=52.40 cpu=96% gs=41% gpu=38% internal_fps=29.97 shader_compiles=7
```

**`hygiene`** lista o que faz o NÚMERO MENTIR — não o que faz o jogo renderizar errado, que é
outra lista e o PCSX2 já tem a dele. Curta de propósito: lista longa vira ruído e ninguém lê. Sai
também com o contexto mínimo (upscale, nível de blend), porque um frametime sem saber o upscale é
ininterpretável.

**`load`** responde a PRIMEIRA pergunta de qualquer relato de lentidão — *é CPU ou GPU?* — que o
bloco anterior não respondia de jeito nenhum. `cpu=96% gs=41%` e `cpu=40% gpu=99%` pedem trabalhos
opostos, e sem esses números os dois chegam como "está lento". `shader_compiles` é DELTA do
intervalo, não acumulado: o total da sessão não diz em qual intervalo houve o engasgo, que é
justamente a pergunta. E `internal_fps` só aparece quando o método consegue medi-lo — um `0.00`
indistinguível de "não sei" faria o leitor concluir que o jogo renderiza a zero.

A linha de carga não repete FPS apresentado nem quadros gerados. Ela fala de **custo**; misturar
ali o número que o usuário vê na tela seria começar a confundir os dois de novo.

`ForkDiagnostics` não lê o config global: a higiene entra por parâmetro, preenchida pelo
`GSDeviceVK`, que já vive nesse mundo. É o que mantém o módulo inteiro exercitável sem VM — as
funções de formato são puras e todos os casos acima têm teste.


## 11. Adendo (8.4) — o que a alpha 2 mediu no Odin 2

Primeira sessão com o bloco `@@FORK@@` no aparelho (NFS Underground 2, SLUS-21065). Três defeitos,
e o primeiro invalidava os outros dois.

### 1. A medição estava desligada por padrão

```
@@FORK@@ real      fps=0.00 frametime_avg=0.00ms 1%low=0.00ms min=0.00ms max=0.00ms
PerfLog: 58.9 fps | EE 41% GS 12% VU 2% GPU 11%
```

Zero, com o jogo a 59 fps. `PresentationMetrics.Enabled` era `false` por padrão, e
`NotePresented` retorna na primeira linha quando a métrica está desligada — o snapshot inteiro sai
zerado.

O estrago não parava no log. **A régua também estava meio cega**: os degraus de estabilidade e de
orçamento comparam contra `frametime_avg_ms` e `last_generation_ms`, e ambos só disparam com valor
`> 0`. Com tudo zerado, só o degrau de velocidade funcionava — o único que não depende das nossas
métricas, porque vem do `PerformanceMetrics` do PCSX2. FG estava engatando com base em um terço da
régua.

Agora liga por padrão. Um fork cuja premissa é *"performance measured"* não pode ter a medição
como opt-in.

### 2. O estado vazava entre jogos

```
[626.3038] @@FORK@@ framegen mode=auto engaged=92.4% transitions=3 ...
```

Impresso no instante da criação da swapchain, **antes de o jogo apresentar um único quadro**. O
estado do `ForkDiagnostics` é global e ninguém chamava `Reset()`: o primeiro bloco de cada sessão
trazia os números da sessão anterior. Agora zera junto com as métricas na criação do renderer —
uma vez por jogo.

### 3. O `auto` piscava, e o número provou

```
[726.3950] engaged=30.0% transitions=20 dominant=BelowFullSpeed
[736.4016] engaged=27.5% transitions=19 dominant=BelowFullSpeed
```

**20 trocas de estado em 10 segundos.** Numa cena pesada (`PerfLog: EE 45% GS 79% VU 61% GPU 64%`)
a velocidade oscilava em torno do limiar único de 90%, e cada oscilação ligava e desligava a
geração. Piscar é pior que ficar desligado: o usuário vê a fluidez aparecer e sumir sem entender
por quê.

`FrameGen.SpeedHysteresis` (5 pontos): engata em 90%, mas só larga abaixo de 85%. Um teste
reproduz a oscilação medida — 10 amostras balançando entre 86% e 95% — e exige **1** transição
onde antes havia dezenas.

### O que funcionou

Tudo o que a régua tinha de dizer, ela disse: `dominant=NoNewFrame` durante os FMVs, `engaged=100%`
com `transitions=0` nos trechos estáveis, e `BelowFullSpeed` exatamente quando o `PerfLog`
independente mostrava a carga subindo. A grandeza nova estava certa; faltavam os números para
alimentá-la.

## 12. Adendo (8.5) — o custo de geração, medido

Primeira sessão com a métrica ligada (Odin 2 Portal, Adreno 740, Turnip Mesa 26.3.0-devel, NFS
Underground 2, upscale 3.00x, higiene **limpa**). O número que faltava desde a Fase 7:

```
@@FORK@@ load      speed=99.9% vps=59.86 cpu=40% gs=9% vu=3% gpu=11% shader_compiles=0
@@FORK@@ real      fps=60.00 frametime_avg=16.74ms 1%low=21.17ms
@@FORK@@ presented fps=61.00 real_frames=60 generated=1 duplicated=0
@@FORK@@ framegen  engaged=3.3% transitions=20 dominant=OverBudget gen_avg=6.45ms gen_worst=7.23ms budget=6.00ms
```

**LSFG 3.1p a 1080p x2, flow 25%, custa ~6,5 ms em cena leve e ~16 ms em cena pesada.** O teto de
6,00 ms era chute meu, feito sem aparelho — e caiu **em cima** do custo normal. Três defeitos
saíram daí, cada um pior que o anterior.

### 1. O teto estava no lugar errado

8 ms separa os dois regimes com folga: aceita a cena leve, recusa a pesada. Não é ajuste fino, é o
valor que a medição obrigou.

### 2. O degrau de orçamento oscilava sozinho

`transitions=20` a cada 10 s, com `speed=99.9%` — não era o degrau de velocidade (esse a Fase 8.4
já tinha estabilizado), era o **orçamento**. O mecanismo está no próprio dado: suspenso por custo,
nenhuma amostra nova entra na janela de 1 s, a média decai até passar no teto, engata, registra o
custo alto de novo e suspende. Um oscilador com período de ~0,5 s.

`FrameGen.BudgetHysteresis` (25% do teto): uma vez suspenso, só reengata abaixo de 6 ms.

### 3. O pior: a régua engatava e nada era gerado

`engaged=3.3%` com `generated=1`. Vinte quadros autorizados, **um** produzido.

A causa é minha, da Fase 8: `NoteGenerationDeclined()` derrubava o histórico do LSFG a **cada**
quadro recusado. Com o orçamento oscilando, quase todo quadro era recusado, então toda vez que a
régua finalmente autorizava, o backend não tinha par para interpolar — semeava um slot e
apresentava o quadro simples. O recurso estava, na prática, desligado.

A correção separa os dois tipos de recusa, que nunca deveriam ter sido tratados igual:

- **Buraco de conteúdo** (`NoNewFrame` — menu de pausa, tela preta, borda de FMV): derruba o
  histórico na hora. Os quadros dos dois lados não têm relação, e costurar inventa movimento que o
  jogo nunca desenhou.
- **Recusa de política**: tolera dois quadros. Frame N-2 e frame N ainda são a mesma cena, e
  interpolar por cima de um pulo de um quadro é muito melhor que não gerar nada.

### O que já funcionava

A linha `load` provou o valor dela na primeira sessão: `1%low=205.30ms` numa janela com
`shader_compiles=133` — o engasgo era compilação de shader, não carga nem frame generation. Sem
esse campo, aquele 1% low seria lido como problema de desempenho e teria custado uma investigação
inteira na direção errada. E `hygiene limpo` confirmou que a medição valia, o que na sessão
anterior não era verdade.
