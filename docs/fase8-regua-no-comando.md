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

## 13. Adendo (8.6) — o contrato cumprido, e por que a corrida ainda não estava boa

Alpha 5 no Odin 2 Portal (Adreno 740, Turnip Mesa 26.3.0-devel, NFS Underground 2, upscale 3.00x,
higiene **limpa**). A sessão tem duas metades, e elas contam histórias opostas.

### A metade que fecha a Fase 8

Cinco janelas consecutivas, dez segundos cada:

```
@@FORK@@ real      fps=60.00 frametime_avg=16.73ms 1%low=19.14ms
@@FORK@@ presented fps=120.00 real_frames=60 generated=60 duplicated=0
@@FORK@@ framegen  engaged=100.0% transitions=0 dominant=Engaged gen_avg=6.90ms budget=8.00ms
```

`speed=99.7%` com `internal_fps=59.76`. Sessenta reais, cento e vinte apresentados, cinquenta
segundos sem uma única troca de estado. É o contrato inteiro da fase, medido: **o número
apresentado dobrou e o real não se mexeu**.

### A metade que não estava boa — e o motivo não é frame generation

Dentro da corrida, treze janelas seguidas com o mesmo desenho:

```
@@FORK@@ load      speed=95.3% ... internal_fps=27.56 shader_compiles=14
@@FORK@@ real      fps=29.00 frametime_avg=33.36ms 1%low=90.67ms min=24.19ms
@@FORK@@ framegen  engaged=6.0% transitions=18 dominant=Unstable gen_avg=20.70ms
```

Três fatos, e nenhum deles é "FG não funciona":

1. **`1%low=90ms` com média de 33 ms.** O pior 1% dos quadros demora quase **três vezes** a média.
   Isso é engasgo de verdade, e é o que o usuário sente. Nenhum frame generation conserta um
   quadro que demorou 90 ms — e a régua está certíssima em recusar: interpolar por cima disso
   piora.
2. **`shader_compiles` diferente de zero em quase toda janela** (104, 29, 26, 21, 14, 13, 8...).
   O engasgo tem nome, e a linha `load` o entrega de graça.
3. **`gen_avg=20ms` contra um orçamento de 8.** A 3.00x de upscale, gerar um quadro na cena de
   corrida custa 20 ms. Não cabe, não vai caber, e o degrau de orçamento está funcionando.

### O defeito que era meu: a régua piscava enquanto recusava

`transitions=18` a cada 10 s, janela após janela. Recusar era certo; **piscar dezoito vezes por
dez segundos enquanto recusa** não era. Ligar e desligar quase duas vezes por segundo é pior para
o usuário do que ficar desligado o tempo todo.

A carência da 8.5 (`FrameGen.CooldownFrames=30`) não resolveu sozinha, e o teste disse isso antes
do aparelho: **8 transições em 120 quadros**. Carência fixa não corta o pulso, ela só o espaça —
uma cena que não cabe no orçamento produz um número CONSTANTE de tentativas condenadas por
segundo.

O que corta é a carência **dobrar** a cada engate fracassado (`FrameGen.MaxCooldownDoublings=5`,
30 → 60 → 120 → … → 960 quadros). As tentativas caem pela metade a cada fracasso, então o total
cresce com o logaritmo do tempo em vez de linearmente. Medido no simulador da própria cena:

| | 10 s | 20 s | 30 s | 40 s | 50 s | 60 s |
|---|---|---|---|---|---|---|
| antes | 18 | 18 | 18 | 18 | 18 | 18 |
| agora | 8 | 2 | 0 | 2 | 2 | 0 |

E o contrapeso, sem o qual isso seria uma troca ruim: **calma sustentada solta a carência na
hora**. Dentro da carência, a régua calcula o contrafactual — este quadro teria engatado? — e
conta os SEGUIDOS. Uma carência base inteira de quadros que teriam engatado (meio segundo) zera
tudo. A oscilação nunca consegue produzir isso, porque metade dos quadros dela é ruim; uma cena
que acabou produz em 30 quadros. Sair da corrida não custa os 16 s acumulados.

Um engate que **durou** mais que a carência também zera o contador: desengatar por um motivo
legítimo não é fracasso e não pode endurecer a régua.

### O outro engasgo: o cache de shaders morria na troca de driver

```
Pipeline cache failed validation: Incorrect UUID
Mismatched pipeline cache header in '.../vulkan_shaders.idx' (GPU/driver changed?)
Removing existing index file '.../vulkan_shaders.idx'
```

A Fase 4 deu nome por driver ao cache de **pipeline** e deixou o de **shader** com nome fixo. O
índice dele carrega um `VK_PIPELINE_CACHE_HEADER`, cujo UUID muda entre drivers — então cada troca
Qualcomm ↔ Turnip apagava o blob inteiro.

O que estava sendo jogado fora é **SPIR-V**, saído do shaderc a partir do GLSL: não depende de
driver nenhum. As variações de fonte que dependem do aparelho já entram na chave do cache, que é o
hash da fonte. A invalidação custava segundos de recompilação e não protegia nada — e o A/B de
drivers, que é o uso previsto deste fork, pagava esse pedágio a cada troca.

Agora o cache de shader usa a mesma chave por driver, com a mesma poda (mantém 4, nunca poda o
ativo). Consequência prática para o usuário: **a segunda volta na mesma pista deve engasgar bem
menos que a primeira**, e trocar de driver para comparar não zera mais o trabalho.

### O bloco de identidade mentia no primeiro quadro

```
@@FORK@@ identity  gpu='Adreno 740' turnip=Supported requested='system' active=Qualcomm proprietary
```

Impresso aos 391,2 s — e aos 402,8 s o mesmo bloco dizia `requested='libvulkan_freedreno.so'
active=Mesa Turnip mesa=26.3.0`, que é a verdade. O quadro em branco apresentado durante a criação
da swapchain chega ao diagnóstico antes de `ForkDriverIdentity::Publish`. O bloco agora espera a
sondagem terminar: um diagnóstico que erra a identidade do driver é pior que um que fica calado,
porque todo o resto do log é lido à luz dele.

### O que fica para medir

- `internal_fps` na corrida é ~28-32: o jogo renderiza a ~30 nativamente ali. Isso é o caso em que
  FG mais ajudaria — mas a 3.00x de upscale ele custa 20 ms e não cabe. **A comparação a fazer é
  2.00x com FG contra 3.00x sem**, e ela é do usuário, não minha: é preferência, não medição.
- A régua nunca escondeu velocidade errada em nenhuma janela da sessão.

## 14. Adendo (8.7) — o orçamento perguntava a coisa errada

Alpha 6 no Odin 2, três jogos a **2.00x** de upscale, cerca de 40 minutos de log. A sessão contém
o experimento controlado que faltava: **NFS Underground 2 e God of War II, mesmo aparelho, mesmo
upscale, ambos travados em 30 fps a 100% de velocidade, e resultados opostos.**

God of War II, doze janelas seguidas:

```
speed=100% internal_fps=29.97
real fps=30.00 frametime_avg=33.4ms
presented fps=60.00 real_frames=30 generated=30
framegen engaged=100.0% transitions=0 gen_avg=5.7ms
```

NFS Underground 2, **onze minutos** seguidos:

```
speed=100% internal_fps=29.9
real fps=29.00 frametime_avg=33.4ms 1%low=33.9ms
presented fps=29.00 generated=0
framegen engaged=3.3% transitions=15 dominant=Cooldown gen_avg=13.3ms
```

O `1%low=33.9` contra média de `33.4` diz que o NFS a 2.00x roda **liso** — praticamente sem
tremor. Não havia nada de errado com o jogo. A régua é que estava recusando.

### Defeito 1: o custo que a régua lia era produzido pela própria recusa

Cruzando todas as janelas da sessão, o padrão é limpo e não admite outra leitura:

| gerações na janela | custo médio |
|---|---|
| 30-60 (sequência) | **5,6-6,6 ms** |
| 1 (isolada) | **10-19 ms** |

A primeira geração depois de uma interrupção custa **cerca do dobro** — reconstruir histórico,
pipeline frio. E enquanto a régua recusa, a única amostra que entra na janela de 1 s é exatamente
essa: um quadro frio isolado. **A medição que decide o recurso passava a ser produzida pelo
recurso estar sendo negado.** Recusa → histórico cai → a próxima tentativa é fria → custa o dobro
→ estoura o teto → recusa. Onze minutos.

O GoW II não era melhor: só teve a sorte de passar dos primeiros quadros antes de um engasgo
derrubar a geração, e a partir daí mediu o próprio regime — 5,7 ms — e ficou.

A correção é na **medição**, não na régua, e é onde ela tinha que ser: `GSPresentationMetrics`
agora marca cada geração com a posição dela dentro da sequência e publica dois números.
`generation_avg_ms` continua com tudo — foi comparar os dois que revelou o mecanismo, e jogar essa
informação fora seria apagar a evidência. `generation_warm_avg_ms` traz só o regime, e é com ele
que a régua decide. **Zero significa "ainda não sei", não "de graça"** — e sem evidência o
orçamento não barra, porque quem protege a emulação de verdade é o degrau de velocidade, que vale
em todos os quadros. O orçamento sempre foi um guarda preditivo.

O log passa a mostrar os dois: `gen_avg=` e `gen_warm=`.

### Defeito 2: 8 ms significam coisas diferentes a 30 e a 60

O teto era um número absoluto de milissegundos. Oito milissegundos dentro de um quadro de 60 Hz
(16,7 ms) são metade do tempo disponível; os mesmos 8 ms dentro de um quadro de 30 Hz (33,3 ms)
são um quarto. Um teto absoluto trata os dois como iguais — e acaba proibindo, no jogo de 30,
exatamente o caso em que frame generation mais rende.

O orçamento passa a ser uma **fração do intervalo real** (`FrameGen.BudgetFraction`, 0,5):

| taxa do jogo | intervalo | orçamento | veredito sobre o custo medido |
|---|---|---|---|
| 60 fps | 16,7 ms | 8,35 ms | igual ao teto antigo — nada muda |
| 30 fps | 33,3 ms | 16,7 ms | admite os 13 ms frios do NFS |
| 30 fps a 3.00x | 33,3 ms | 16,7 ms | continua recusando os 20 ms medidos |

`FrameGen.BudgetMs` sobrevive como **teto absoluto** (20 ms), que só morde quando o intervalo real
é muito longo — para a régua não depender da ordem dos degraus para estar correta.

### O que já tinha funcionado

O cache de shaders por driver da 8.6 está confirmado no aparelho:

```
Read 287 entries from '.../vulkan_shaders_972c1a6fedf27758.idx'
Read 391 entries from '.../vulkan_shaders_972c1a6fedf27758.idx'
Read 679 entries from '.../vulkan_shaders_972c1a6fedf27758.idx'
```

Nome com chave de driver, três reinícios do app, **nenhum** `Incorrect UUID` e nenhum
`Removing existing index file` — e o cache acumulando de 287 para 679 entradas em vez de voltar a
zero. Era isso que a 8.6 prometia.

E a 2.00x, com o que este adendo corrige, o quadro esperado é o do God of War II: 30 reais, 60
apresentados, `engaged=100% transitions=0`.

## 15. Adendo (8.8) — cada release exigia desinstalar a anterior

A alpha 7 não instalou por cima da 6: *"Como o pacote tem um conflito com um pacote já existente,
o app não foi instalado."* Isso é conflito de **assinatura**, e a causa era do próprio workflow.

Sem keystore configurada, o Gradle cai em `signingConfigs.debug`, que usa a `~/.android/debug.keystore`
do ambiente de build. No runner do GitHub esse arquivo é gerado do zero a cada job — então **toda
release saiu com uma chave diferente**, e o Android trata a nova como um app estranho tentando se
passar pelo instalado.

O custo não é o clique a mais de desinstalar. É que a desinstalação apaga
`Android/data/com.armsx2/files/` inteiro: memory cards, `PCSX2-Android.ini`, o pacote do driver
Turnip e o cache de shaders — o mesmo cache que a 8.6 acabou de ensinar a sobreviver a trocas de
driver, e que a sessão medida mostrou acumulando de 287 para 679 entradas.

A correção assina com chave estável vinda de segredo (`ARMADA_KEYSTORE_BASE64` e os três
companheiros), materializada fora da árvore e apagada no fim do job — `if: always()`, para não
ficar no disco quando um passo posterior falha. `tools/fork/make-signing-key.sh` gera a chave e
imprime os quatro segredos para colar; a chave nunca entra no repositório, porque um fork público
com a chave privada dentro é uma chave que qualquer um usa para assinar "atualizações".

Sem os segredos o build continua funcionando, mas passa a **avisar**: um `::warning` no job e um
bloco na descrição da release dizendo que aquela versão não instala por cima e que a pasta de
dados precisa de backup antes. Um alpha que não atualiza é problema de distribuição, não detalhe
de empacotamento — e ficar em silêncio sobre isso foi o que fez o usuário descobrir no aparelho.

## 16. Adendo (8.9) — a calma provava o que não podia provar, e dois padrões novos

Alpha 7 no Odin 2, dois jogos a 2.00x. A 8.7 funcionou onde tinha que funcionar, e o log mostra o
orçamento se adaptando sozinho — `budget=8.35ms` nas janelas de 60 fps, `budget=16.68ms` nas de 30.

**God of War II, os dois regimes, impecáveis.** Trinta fps:

```
real fps=30.00 presented fps=60.00 generated=30 engaged=100.0% transitions=0 gen_warm=5.43ms budget=16.70ms
```

E sessenta, por **vinte e cinco janelas seguidas** — mais de quatro minutos:

```
real fps=60.00 presented fps=120.00 generated=60 engaged=100.0% transitions=0 gen_warm=6.43ms budget=8.35ms
```

**NFS Underground 2 na corrida, ainda não.** Vinte janelas com o mesmo desenho, e desta vez o log
novo entrega a causa de graça:

| | com `generated=1` | com `generated=0` |
|---|---|---|
| `1%low` | **75-84 ms** | **33-39 ms** |
| `shader_compiles` | 0 em várias | 0 |

Média de quadro nos dois casos: 33,4 ms. Com `shader_compiles=0` dos dois lados, não sobra outra
leitura: **uma única geração fria acrescenta ~45 ms ao tempo do quadro** — bem mais do que os
13-16 ms que o nosso cronômetro mede, porque o cronômetro cobre o `wait_idle` e o `present` do
backend e não o que a inserção do quadro extra faz ao ritmo do FIFO.

### O defeito: a régua usava a ausência do problema como prova de que ele acabou

A sequência, inteira, saiu do log: engata → gera um quadro frio → +45 ms → o degrau de
estabilidade derruba → carência. E aí, **desengatada**, ela mede a cena: `1%low=34ms` contra média
de `33,4ms`. Calma perfeita. A "calma sustentada" da 8.6 solta a carência, e trinta quadros depois
tudo se repete.

A calma era real e mesmo assim enganosa: **a cena está calma porque a geração não está lá.** A
carência crescente, que existe justamente para extinguir esse pulso, nunca chegava a crescer —
`transitions=10` por janela, estável, do começo ao fim da corrida.

A correção separa os dois casos pela única coisa que a oscilação não consegue falsificar:

- Um engate que **durou** (≥ 5 quadros) viu a cena com a geração ligada. A calma que ele mede
  depois vale, e solta a carência como antes.
- Um engate que **piscou** não viu nada. Depois dele, calma não basta: é preciso que o **regime**
  tenha mudado — o intervalo real diferir em mais de 15% do que era no desengate. Sair da corrida
  para um menu vai de 33,4 ms para 16,7 e solta na hora; piscar dentro da corrida mede 33,4 dos
  dois lados e não solta.

Simulando a corrida medida, transições por 10 s ao longo de um minuto:

| | 0-10s | 10-20s | 20-30s | 30-40s | 40-50s | 50-60s |
|---|---|---|---|---|---|---|
| antes | 10 | 10 | 10 | 10 | 10 | 10 |
| agora | 6 | 2 | 0 | 2 | 0 | 0 |

O que sobra é o correto: numa cena em que gerar custa 45 ms de ritmo, a resposta certa é **não
gerar** — e parar de tentar, em vez de piscar dez vezes a cada dez segundos anunciando que não vai
dar.

### Dois padrões novos, a pedido

- **Resolução interna 2.00x.** O número vem da medição: a 2.00x os três jogos ficaram em
  velocidade correta com ritmo regular (`1%low=33,9ms` contra média de `33,4ms` no NFS); a 3.00x
  os mesmos jogos deram `1%low` de 83-95 ms e a geração passou a custar 20 ms. `lowEndPreset`
  continua forçando resolução nativa, então aparelho fraco não herda isto.
- **Patches de widescreen 16:9.** O alvo é um handheld 16:9, e 4:3 com tarjas desperdiça um terço
  do painel. A ressalva fica registrada: são patches por CRC, nem todo jogo tem um, e os que têm
  podem esticar HUD ou revelar geometria nas bordas — por isso a opção continua visível,
  desligável, e com override por jogo.

Só instalações novas são afetadas: quem já tem configuração salva mantém a escolha dele.

## 17. Adendo (8.10) — o número que faltava: a cadência do que sai para a tela

Relato do testador na alpha 8: *"não gerou quadro algum que fizesse diferença real"*. E o log
concorda com ele de um jeito que precisa ser lido com cuidado — porque, pelos números que a gente
tinha, **estava tudo certo**:

```
real fps=30.00 frametime_avg=33.40ms 1%low=33.50ms
presented fps=60.00 real_frames=30 generated=30
framegen engaged=100.0% transitions=0 gen_warm=14.29ms budget=16.70ms
```

Doze janelas seguidas assim. Trinta reais viram sessenta apresentados, sem uma troca de estado. Se
o contrato da Fase 8 fosse só esse, estaria cumprido. Não estava, e o motivo é que **nunca medimos
a cadência dos quadros APRESENTADOS** — só a dos reais.

### Por que a contagem não basta

O painel do aparelho roda a 120 Hz em FIFO (confirmado no log: `present=VK_PRESENT_MODE_FIFO_KHR`,
e `presented fps=120` foi atingido em outras sessões, o que sob FIFO só é possível se o painel dá
120). Num jogo de 30 fps com um quadro gerado, `presented fps=60` pode significar duas coisas
opostas:

- quadros a cada **16,7 ms** — o recurso funcionando, movimento visivelmente mais fluido;
- quadros em **pares**, 8,3 ms e depois 25,0 — mesmo `presented fps=60`, mesma contagem, e o olho
  vê 30 fps com a latência da interpolação de brinde.

O segundo caso é o que o mecanismo prevê. O backend produz o quadro gerado *depois* que o quadro
real N existe (é entre N-1 e N que ele interpola), e então os dois presents são enfileirados no
mesmo instante. Sob FIFO a apresentação entrega um por vblank: o gerado no vblank seguinte, o real
no vblank depois — 8,3 ms de distância — e aí 25 ms de silêncio até o próximo quadro do jogo.

O `lsfg-vk` no Linux não sofre disso porque lá a própria fila FIFO segura a aplicação: gerando o
dobro de quadros, o jogo é freado pela contrapressão e os presents saem espaçados sozinhos. Aqui a
taxa do produtor é fixada pelo vsync do PS2, não pela swapchain — a contrapressão não existe, e o
truque de "deixar o compositor pacear" não se aplica.

**Isto ainda é hipótese.** É por isso que a 8.10 não muda a geração: ela mede.

### O que passa a existir

`pace_avg`, `pace_min` e `pace_max` na linha `presented` — média, menor e maior intervalo entre
quadros que efetivamente foram para a tela, na janela. Calculados a partir das amostras que o
módulo já guardava, sem nenhum registro novo no caminho quente.

A leitura é direta: **min e max colados na média** significam cadência regular e o recurso
entregando; **afastados** — algo como `pace_avg=16.7 pace_min=8.3 pace_max=25.0` — significam
quadros saindo grudados, e aí o `presented fps` está contando o que não se vê.

Dois testes fixam exatamente essa distinção, e o segundo existe para provar que a média sozinha
não serve: os dois casos têm `pace_avg` idêntico.

### O caminho, se a hipótese se confirmar

Não é atrasar o quadro real para paceá-lo — isso custa latência e trabalho de sincronização que o
Android não facilita (sem semáforo entre dispositivos, o bloqueio já é o mecanismo).

É **casar a taxa apresentada com o painel**. Num painel de 120 Hz:

| jogo | multiplicador | apresentado | cadência sob FIFO |
|---|---|---|---|
| 30 fps | x4 | 120 | um quadro por vblank — regular por construção |
| 60 fps | x2 | 120 | um quadro por vblank — regular por construção |
| 30 fps | x2 | 60 | dois presents por quadro de jogo, e a lacuna |

Com um quadro para cada vblank não sobra lacuna para o pacing errar. E o multiplicador **já
existe**: o backend aceita 2 a 4, a UI já oferece x2/x3/x4, e a régua do fork gateia apenas
ligado/desligado — o número de quadros vem de `GSConfig.LsfgMultiplier`. Ou seja, **x4 num jogo de
30 fps é testável hoje, sem mudança de código**. O que falta é a medição que diz se valeu, e é ela
que este adendo entrega.

O custo é a incógnita: a x2 a geração custou 14,3 ms de um orçamento de 16,7. A x4 reaproveita o
mesmo campo de fluxo para os três quadros, então não deve triplicar — mas "não deve" não é medida,
e o degrau de orçamento vai barrar se não couber. O que é exatamente o comportamento desejado.

## 18. Adendo (8.11) — o modo "Performance Cores" era uma armadilha

Avaliando o que já existe no emulador para virar padrão, o candidato óbvio era o *Affinity Control
Mode*, hoje em `Disabled`. Num big.LITTLE, deixar a thread do EE migrar para um núcleo pequeno
custa dezenas de milissegundos, e o modo 7 ("Performance Cores") parecia a escolha segura.

Era o contrário. A regra era *"o cluster que contém o núcleo de maior frequência"*, e no QCS8550 do
Odin 2 a topologia é **1x Cortex-X3 + 4x A715/A710 + 3x A510**: o cluster do topo tem **um núcleo
só**. O modo confinava EE, GS e VU aos três num único core — pior que não fixar nada. A opção com
o nome mais tranquilizador era a mais perigosa.

A regra passa a ser **todos os clusters exceto o mais lento**: 5 núcleos naquele SoC, os 4 grandes
num 4+4, tudo num chip de cluster único. É o que as pessoas querem dizer com o nome, mantém os
núcleos pequenos fora do caminho, e nunca colapsa em um core. Quando o cpuinfo reporta 0 MHz para
todos os núcleos — comum no Android — não há como saber qual é o cluster lento, e adivinhar fixaria
o emulador nos pequenos metade das vezes: nesse caso a máscara sai vazia e o modo cai no caminho
ordenado, que ao menos usa a ordem do próprio cpuinfo.

**O padrão continua `Disabled`**, e isso é deliberado: a regra do projeto é que nenhuma otimização
vira global sem comparação A/B, e não há uma única medição de affinity ainda. O que mudou é que
agora a opção faz o que o nome promete — dá para medir de verdade.

## 19. Adendo (8.12) — a alpha 9 mediu, e a medição tinha um defeito meu

Três sessões com `pace_*`. Nas janelas de 30 fps com FG engatado:

```
real fps=30.00  presented fps=60.00  generated=30  engaged=100.0%  transitions=0
pace_avg=16.42ms  pace_min=0.00ms  pace_max=33.6ms
```

`pace_min=0,00 ms` — dois quadros apresentados no MESMO instante. E `pace_max=33,6`, um quadro de
jogo inteiro de silêncio depois. Comparado com a mesma cena de FG desligado (`pace_avg=33,39
pace_min=30,13 pace_max=36,84`) e com 60 fps sem FG (`pace_avg=16,69 pace_min=16,17
pace_max=17,20`, que é o que cadência regular parece), o desenho é inconfundível.

**Mas parte desse `0,00` é defeito meu, não do recurso.** `NoteFramesDisplayed(1, generated)` era
chamada uma única vez, no fim do quadro, e registrava os gerados em bloco — todos os
`NotePresented` caíam no mesmo instante. O número descrevia a nossa contabilidade, não o que foi
para a tela. Uma métrica de RITMO tem que ser registrada onde o ritmo acontece, e agora cada quadro
gerado se anota no instante do próprio `vkQueuePresentKHR`.

O que a medição corrigida **não** vai mudar é a conclusão, porque ela já está no código: o quadro
gerado espera um slot de display (o `vkAcquireNextImageKHR` com timeout é a pacing dele), e o
quadro real é apresentado **imediatamente depois, sem esperar nada**. Sob FIFO isso põe o gerado
num vblank e o real no vblank seguinte — 8,3 ms de distância — e deixa ~25 ms de lacuna até o
próximo par. Metade da suavidade prometida, com a latência inteira cobrada.

### O caminho que o próprio código já indica

O comentário do laço de geração diz a coisa certa: *"esperar por um slot de display É o mecanismo
aqui"*. Com **x2** só existe uma espera, então só um quadro é espaçado e o par sai grudado. Com
**x4** num jogo de 30 fps há três esperas, cada uma num vblank: gerado, gerado, gerado, real — **um
quadro por vblank, 120 Hz preenchidos, regular por construção**. O mecanismo não precisa de código
novo; precisa de multiplicador suficiente para se completar.

### E o custo estava 2,7x acima do necessário — por uma opção

As três sessões rodaram com `flow 100%`. As sessões boas anteriores rodavam com `flow 25%`:

| flow scale | custo de regime medido |
|---|---|
| 25% | **5,2-5,4 ms** |
| 100% | **14,2-14,7 ms** |

Mesmo aparelho, mesmo jogo, mesmo x2. O `flowScale` é o divisor da pirâmide de fluxo óptico, e a
100% ele processa em resolução cheia. Foi isso que levou o custo de 5,4 para 14,5 ms — e é também
por isso que `x4` parecia impossível: a 14,5 ms por quadro gerado, três não caberiam em 33 ms. A
25%, três quadros gerados custariam algo perto de 10-11 ms e caberiam com folga.

**A combinação a testar é `flow 25%` + `x4` num jogo de 30 fps.** Nenhuma linha de código nova: o
backend aceita 2 a 4, a UI já oferece as duas opções, e a régua gateia apenas ligado/desligado.

## 20. Adendo (8.13) — o que dá e o que não dá para trazer do ARMSX3

O ARMSX3 0.9.3 (porte Android do RPCS3 pelos mesmos fundadores do ARMSX2) traz uma prática que
descreve exatamente o nosso pior engasgo medido:

> *"Shader compilation moved off cores the frame depends on; compiler threads use little cluster on
> big.LITTLE phones."*

É o mesmo raciocínio da nossa 8.11 — tirar do caminho do quadro o que não é o quadro. **Mas não
transfere como está**, e vale dizer por quê em vez de anunciar que copiamos: no PCSX2 a compilação
não roda em threads de compilador. `VKShaderCache::CompileShaderToSPV` é chamada de forma
**síncrona, na própria thread do GS**, quando um pipeline novo é necessário. Não há thread para
mover; mover exigiria compilação assíncrona, e num emulador isso significa desenhar o quadro com o
shader errado ou travar esperando — as duas opções são piores que o engasgo.

O que resolve o mesmo problema aqui, e já está feito, é outro: a chave por driver no cache de
SPIR-V (8.6), que faz a compilação acontecer **uma vez na vida** em vez de uma vez por sessão. O log
do aparelho confirmou o cache acumulando de 287 para 679 entradas ao longo de três reinícios.

Duas outras notas da 0.9.3 valem como princípio, não como código:

- *"Running out of memory while compiling now says so"* — falhar com nome em vez de travar. É a
  mesma regra que nos fez trocar `verdict=Ok` por sete veredictos distintos no inspetor de pacote.
- *"Cleared debug profiler settings previously shipped enabled"* — vale como checagem, e a fiz:
  `OsdShowGSStats` e `OsdShowGPUStats` estão desligados por padrão (o segundo dispara consultas de
  pipeline por quadro), e o que ligamos de propósito — `PresentationMetrics.Enabled` e
  `Diagnostics.Log` — custa uma carga atômica por quadro e um bloco de texto a cada 10 s.

O resto das notas da 0.9.3 é PS3: reserva atômica do PPU, instalação de PKG, teclado USB emulado.
Não há analogia no PS2.

## 21. Adendo (8.14) — nomes de driver que não distinguiam nada

Os pacotes de uma release costumam vir em mais de uma variante — o `.zip` comum e o `_oneUI.zip`
ao lado. A lista de download mostrava `releaseName` como título e `fonte · tag` como subtítulo, e
como as duas variantes compartilham release e tag, **as duas linhas ficavam idênticas**: escolher
virava sorteio.

A informação que distingue já existia (`assetName`), só não estava sendo mostrada. O subtítulo passa
a ser `arquivo · tag`, e a fonte sai — estas linhas já vivem dentro do grupo dela, e repeti-la
gastava justamente a largura que o nome do arquivo precisa. Vale para toda fonte da lista, sem
depender de ninguém renomear nada.

## 22. Adendo (8.15) — o que o ARMSX3 rendeu quando eu li o código em vez das notas

A avaliação anterior (8.13) foi feita a partir das **notas de release**, e a conclusão foi "não
transfere". Lendo o **código** (`git clone` do repositório, HEAD `da26a45`), a conclusão muda em
dois pontos e se confirma num terceiro.

### 1. A afinidade deles é melhor que a nossa, por dois motivos concretos

`thread_ctrl::get_affinity_mask` em `Utilities/Thread.cpp`:

- **A fonte.** Eles leem `/sys/devices/system/cpu/cpuN/cpu_capacity` — a capacidade normalizada
  que o próprio EAS usa para escalonar. Nós líamos frequência do cpuinfo, que **reporta 0 MHz em
  boa parte dos SoCs Android**; era o caso em que a nossa máscara saía vazia e o modo não fazia
  nada. `cpu_capacity` existe em todo DynamIQ/big.LITTLE.
- **O critério.** Nós agrupávamos por cluster; eles agrupam por capacidade, com uma folga de 25%:
  *"anything within 25% of the fastest core counts as fast, so a mid cluster (A715/A710) joins the
  prime core rather than being lumped in with the A510s"*. Chega ao mesmo lugar no QCS8550 e
  continua correto em topologias que a gente não conhece — inclusive a que nos mordeu, o X3 sozinho
  num cluster de um núcleo.

E uma terceira ideia que é inteiramente deles, com a medição junto:

> *"Reserve the single fastest core for RSX where there is one to spare. […] RSX is the thread the
> frame waits on: it was measured spending about 10ms per frame inside its own loop without
> running, not blocked on the GPU and not faulting, simply waiting for a core."*

Aqui a análoga da RSX é a **MTGS** — é ela que submete e apresenta. O modo 7 passa a ser:

| thread | máscara | no Odin 2 (QCS8550) |
|---|---|---|
| MTGS | rápidos, **incluindo** o prime | 5 núcleos (`0xf8`) |
| EE | rápidos **menos** o prime | 4 núcleos (`0x78`) |
| VU1 (MTVU) | idem | 4 núcleos |

Verificado com as capacidades típicas do SoC: `threshold=768`, `fast=0xf8`, `slow=0x7` (os três
A510, fora), `prime=0x80` (o X3). A regra anterior — a que eu tinha escrito ontem — dava os mesmos
5 núcleos para as três threads; a de antes dela dava **um** núcleo para as três.

### 2. Eles têm frame generation, bateram no NOSSO problema, e escreveram a saída

`rpcs3/Emu/RSX/VK/VKPresent.cpp` descreve exatamente a correção de cadência que a 8.12 deduziu:

> *"The pipelined path holds the real frame back by one present so the generated image can go out
> ahead of it, which means one more image in flight than normal presentation needs […] Below that,
> present_generated_frame's zero-timeout acquire would fail every frame and frame generation would
> compute images nothing ever puts on screen — worse than the serialised path it replaces, and
> silently so. So fall back rather than degrade."*

Duas coisas saem daí, e as duas importam:

- **O pré-requisito é contagem de imagens da swapchain.** Eles só habilitam o caminho pipelinado
  com `get_swap_image_count() >= 4`. Nós pedimos **2** e recebemos **3** (o mínimo do driver), o
  que é insuficiente até para x2 — e a x4 seriam quatro presents disputando três imagens. Agora,
  com FG ligado, pedimos `multiplier + 2`.
- **Eles desligaram o caminho pipelinado** (`k_framegen_pipelining_enabled = false`) por um
  travamento no reciclo de contexto de quadro que não conseguiram fechar — *"'already retired' still
  fired twice on device, so at least one more reclaim path exists and has not been found"*. O que
  shipou neles é o caminho serializado, que é o mesmo que o nosso. **Ninguém resolveu isso ainda**,
  nem eles.

O aviso fica registrado: a correção de cadência que a 8.12 apontou é a certa e é a que eles
tentaram; a arquitetura de present do PCSX2 é mais simples que a deles (não há anel de
`frame_context`), mas o risco tem nome e endereço.

### 3. O que continua não transferindo, e por quê

A nota da 0.9.3 sobre compilação de shader **é** sobre um pool de threads que eles têm e nós não:
`vk::pipe_compiler::operator()` fixa os workers em `thread_class::general` e dimensiona o pool pelos
núcleos a que eles estão fixados, não pela máquina. No PCSX2 a compilação é síncrona na thread do
GS — não há worker para fixar. Isso não mudou por eu ter lido o código; ficou mais claro.

### O que foi aplicado desta leitura

- Modo 7 de afinidade reescrito sobre `cpu_capacity`, limiar de 25% e prime reservado para a MTGS.
- Swapchain pede `multiplier + 2` imagens quando frame generation está ligado.

O padrão de afinidade **continua `Disabled`**: segue sem uma única medição A/B, e a regra do
projeto não abre exceção porque a ideia veio de um projeto que a mediu no aparelho dele.

## 23. Adendo (8.16) — dois padrões que a medição já tinha decidido

A alpha 10 rodou com `flow 25%` e `x2`, e os números fecham a questão do custo:

| | 30 fps (NFS) | 60 fps |
|---|---|---|
| `gen_warm` a **25%** | **5,3-5,5 ms** | **7,7-8,2 ms** |
| `gen_warm` a 100% (alpha 9) | 14,2-14,7 ms | — |
| `budget` efetivo | 16,7 ms | 8,35 ms |

Quase três vezes mais barato, **trocando só a opção**. E é a diferença entre caber e não caber: a
100% a geração comia 14,5 ms de um orçamento de 16,7 e a régua vivia recusando; a 25% o jogo de 30
ficou em `engaged=100% transitions=0` por janelas seguidas, e o de 60 chegou a `presented fps=120`
com `engaged=100%`.

O que se paga é resolução do CAMPO DE FLUXO, não da imagem: o que borra é a estimativa de
movimento, e num quadro que fica na tela 8 ms isso é menos visível que a alternativa — não ter o
quadro. `LsfgFlowScale` passa a 25 por padrão; quem quiser fluxo em resolução cheia sobe a opção.

**E o controle na tela passa a vir desligado.** O alvo é um handheld com controles físicos, onde o
overlay não é recurso: é uma camada de botões desenhada por cima do jogo que ninguém vai tocar,
comendo área de tela e recebendo toque acidental de palma. Ligar, para quem joga em celular sem
controle, é uma linha nas configurações; descobrir onde DESLIGAR é o atrito que o padrão errado
cobra de todo mundo que usa o aparelho a que este fork se destina.

A guarda contra o pior caso já existia no upstream e foi verificada antes de mexer: o botão de
pausa é desenhado ACIMA do early-return de `showPad`, com o comentário dizendo exatamente por quê
— *"with on-screen controls = Never (visMode 0, the RP6 case) ... a pause button drawn only by that
loop would vanish and lock a controller user out of the menu"*. Ninguém fica trancado fora do menu.

Os dois só afetam instalação nova; quem tem preferência salva mantém a escolha.

### O que continua em aberto, e agora tem nome

A cadência com `x2` ainda é bimodal. Na maioria das janelas o par sai grudado (`pace_min≈0,10ms`,
`pace_max≈34ms`); em algumas — t=699, 729, 769, 1199 — sai **certo** (`15,70/17,82`, `14,21/19,32`),
com `pace_avg=16,68` em vez de 16,40. A diferença é se o `vkAcquireNextImageKHR` do quadro gerado
bloqueou ou não, e com 3 imagens de swapchain isso é uma corrida.

Foi por isso que a 8.15 passou a pedir `multiplier + 2` imagens quando FG está ligado — o número
que o próprio RPCS3/ARMSX3 documenta como piso. **Essa mudança ainda não rodou no aparelho**: a
alpha 10 foi construída antes dela. É o que a próxima sessão precisa medir.
