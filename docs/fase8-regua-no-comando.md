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
