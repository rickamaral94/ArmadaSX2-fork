# Fase 2 — `presentation_metrics`

**Objetivo:** medir a camada de apresentação antes de mexer nela. Nada de frame generation entra
sem que exista um número confiável de *quanto* a emulação está rodando e *quanto* está chegando à
tela — e sem que esses dois números sejam impossíveis de confundir.

**Critério de aceite:** os números aparecem no overlay e nos logs; com a métrica desligada, o
caminho de apresentação paga uma carga atômica e um branch, nada mais.

---

## 1. Por que um módulo separado de `PerformanceMetrics`

`PerformanceMetrics` (upstream) mede **velocidade de emulação**: FPS interno, VPS, uso de thread,
tempo de GPU. `GSPresentationMetrics` (nosso) mede **o que chegou ao display**.

A separação não é organizacional, é o ponto do projeto:

```
FPS real      = quadros com conteúdo NOVO do jogo que foram apresentados
FPS aparente  = tudo que chegou ao display (real + repetido + gerado)
```

Um único campo de "FPS" não consegue dizer as duas coisas, e é exatamente confundindo os dois que
frame generation vira maquiagem de emulação lenta. Aqui eles são campos distintos, contados a
partir de fontes distintas, e o overlay **sempre** mostra os dois lado a lado e rotulados — mesmo
quando são iguais, porque ler "60" sozinho não diz se o jogo está em velocidade.

## 2. O que é medido

| Métrica | Observação |
|---|---|
| FPS real / FPS apresentado | contados por tipo de quadro: `Real`, `Duplicate`, `Generated` |
| frametime real: média, mín, máx | só entre quadros reais consecutivos |
| 1% low (ms e FPS) | média do 1% pior da janela — o pico que a média esconde |
| engasgos | intervalos acima de 2× a mediana da janela |
| quadros repetidos / gerados | separados, nunca somados ao real |
| presents pulados | só o *frame skip* deliberado; falha de swapchain é erro, não pulo |
| custo da chamada de present | média e máximo, em ms |
| custo da geração | média em ms — zero até as fases 7-8 |
| erros de present | `vkQueuePresentKHR` recusado |

Janela deslizante de 1 s, que é o mesmo horizonte mostrado no overlay: o número exibido e o número
medido são o mesmo número.

## 3. Onde ele engancha

Arquivos novos: `pcsx2/GS/Renderers/Common/GSPresentationMetrics.{h,cpp}` — sem Vulkan, sem ImGui,
para que o caminho OpenGL do Android possa usar o mesmo módulo depois.

Pontos de contato em `GSDeviceVK.cpp` (todos pequenos, todos no caminho de apresentação):

| Local | O que faz |
|---|---|
| `SubmitCommandBuffer`, após o tratamento de erro do present | conta o quadro como `Real` ou `Duplicate` conforme `has_new_frame` |
| `SubmitCommandBuffer`, ao redor de `vkQueuePresentKHR` | mede o custo da chamada (reaproveita o cronômetro que já existia para as estatísticas do WSI) |
| `SubmitCommandBuffer`, ramo do LSFG | conta o quadro real; ver a limitação abaixo |
| `DoBeginPresent`, `if (frame_skip)` | conta o pulo deliberado |
| `EnableExtendedStats` | liga/desliga junto com as estatísticas estendidas |
| `GetExtendedStats` | acrescenta as linhas detalhadas |

E uma linha no `ImGuiOverlays.cpp`.

**O quadro só é contado depois do tratamento de erro.** Um present recusado não chegou à tela, e
contá-lo inflaria o FPS apresentado justamente quando o dispositivo está em apuros — que é quando
o número precisa ser confiável.

**Limitação conhecida, deliberada:** com LSFG ligado, os quadros interpolados que ele apresenta por
dentro não são contados, porque ele não os reporta. O FPS real continua correto; o apresentado fica
**subestimado**. Preferimos um número faltando a um número inventado. Nosso backend de frame
generation (fases 7-8) reportará os próprios quadros via `NotePresented(Generated)` — a API já está
no formato certo para isso.

## 4. Custo quando desligado

`SetEnabled(false)` é o padrão. Cada `Note*` faz uma carga atômica *acquire* e retorna. Nenhuma
alocação acontece no caminho quente nem quando ligado: os buffers são reservados uma vez e a janela
descarta pela ponta. Há um `std::mutex` protegendo o estado (a apresentação escreve na thread MTGS,
os leitores podem ser outra thread), tomado **apenas** com a métrica ligada — um mutex sem
contenção a 60 Hz é ruído comparado a um present.

Existe um teste dedicado a isso (`DisabledRecordsNothing`): se ele falhar, medir passou a custar
para quem nunca ligou a medição.

## 5. Verificação

`tests/ctest/core/presentation/presentation_metrics_tests.cpp` — 10 casos, alvo
`presentation_metrics_tests`, arch-neutral, sem GPU.

O módulo aceita um relógio injetado (`Detail::SetClockForTesting`) porque toda a lógica que importa
é janela e percentil: verificar isso com o relógio de parede exigiria dormir de verdade, e um teste
lento e intermitente é o pior tipo de fundamento para decisões de desempenho.

Cobertura: desligado não registra nada; real e apresentado nunca se misturam (30 reais + 30 gerados
→ 30 e 60); repetidos contam como apresentado e não como real; média/mín/máx de frametime; 1% low e
engasgo expõem um pico de 100 ms que a média esconde; expiração da janela; presents pulados e erros;
custo de geração; o overlay nomeia os dois números; `Reset` limpa tudo.

Executados localmente antes do commit: **10/10**.

## 6. O que fica para depois

**Interruptor dedicado na UI.** Hoje a métrica pega carona no `OsdShowGPUDebug`, que fora do
Windows não desenha nada. Um campo próprio custaria acrescentar um bit em `Config.h` e propagá-lo
por `Pcsx2Config.cpp`, `GS.cpp`, `FullscreenUI`, o Qt, o `Settings.kt` do Android e o
`native-lib.cpp` — sete arquivos do upstream, e de novo a cada opção futura (driver por jogo, FG
on/auto/2x).

A decisão a tomar antes da Fase 4 é criar **uma superfície de configuração própria do fork**, com
suas opções em um espaço de nomes separado, em vez de somar bits ao `Config.h` do upstream a cada
recurso. Isso mantém o diff de merge pequeno e é o que torna as fases seguintes baratas.
