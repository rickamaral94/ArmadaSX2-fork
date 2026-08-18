# Fase 6 — framework de benchmark A/B

**Objetivo:** que "Turnip é melhor" e "Frame Generation ajuda" sejam afirmações com número atrás, e
que um número inválido nunca passe por válido.

**Critério de aceite:** relatório exportável em CSV/JSON, com FPS real e FPS apresentado medidos e
apresentados **separadamente**.

---

## 1. O que torna um A/B inválido

Três coisas, e todas acontecem em silêncio:

**Medir o driver errado.** O fallback do carregador é silencioso (Fase 4, item 2), então a execução
rotulada "Turnip A" pode ter rodado no driver do sistema. Cada execução grava a identidade **real**
e o SHA-256 do pacote, e a comparação avisa quando o driver não foi o pedido.

**Medir compilação em vez de regime.** A primeira execução de um driver compila shaders; a segunda
não. O tempo de compilação é medido por diferença de contadores cumulativos e reportado à parte —
se houve compilação durante a medição, a comparação diz que aquilo foi primeiro boot.

**Misturar FPS real com FPS apresentado.** Frame generation muda um e não o outro. São campos
distintos do começo ao fim, e a comparação os imprime em **linhas separadas** — há teste garantindo
que nenhuma linha contenha os dois.

## 2. Como mede

`GSPresentationMetrics` ganhou **acumulação de sessão**, ao lado da janela de 1 s que já existia.
A janela responde "como está agora", que é o que o overlay precisa; um benchmark precisa do trecho
inteiro. Amostrar a janela periodicamente daria contagem dupla (janelas se sobrepõem), então a
sessão tem contadores próprios, alimentados pelos mesmos `Note*`.

Percentis vêm de um **histograma de 0,25 ms** (1000 baldes, 4 KB fixos). Guardar cada amostra daria
percentis exatos, mas meia hora a 60 Hz são ~108 mil floats — memória que o emulador não tem
sobrando em um celular. O erro de meio balde (0,125 ms) está muito abaixo da diferença que um A/B
de driver precisa distinguir.

Um bug encontrado por teste: `Reset()` não limpava a sessão. Como ele é chamado ao ligar/desligar a
métrica e **ao recriar a swapchain**, uma sessão sobrevivente passaria a reportar números de antes
do evento como se fossem da medição atual. Um benchmark que herda contagens é pior que um que
reporta zero — o zero é visível, a herança não.

## 3. Uso

Pela ponte (Fase 4), então serve tanto à UI Android quanto a scripts:

```
benchmark.begin:System      → inicia a medição sob esse rótulo
benchmark.end               → encerra, guarda e devolve o resultado
benchmark.status            → há execução em andamento? quantas já foram feitas?
benchmark.runs              → todas as execuções, em JSON
benchmark.clear             → descarta
```

Cada execução guarda: FPS real, FPS apresentado, frametime (médio/mín/máx/p95/p99), 1% low,
engasgos, quadros reais/duplicados/gerados, presents pulados, erros de present, custo de geração,
shaders compilados e tempo gasto neles, mais GPU, driver ativo, veredito do carregamento, versão do
Mesa, versão do Vulkan e SHA-256 do pacote.

## 4. Comparação

`CompareLines(baseline, candidate)` imprime cada métrica com delta percentual e um rótulo
(`melhor`/`pior`/`igual`), seguido dos avisos de validade.

`ValidityWarnings` reprova a comparação quando: o driver não foi o pedido; houve compilação de
shader durante a medição; houve erros de present; a execução durou menos de 30 s; as GPUs diferem;
ou as durações diferem em mais de 25% (provavelmente trechos diferentes do jogo, e aí a diferença
medida é da cena, não do driver).

Exemplo do caso que o projeto **proíbe** chamar de sucesso:

```
FG Off x FG On
FPS real       : 30.00 -> 22.00 (-26.7%, pior)
FPS apresentado: 30.00 -> 44.00 (+46.7%, melhor)
```

O apresentado quase dobra, e a queda do real aparece na linha de cima, rotulada como pior. Tem
teste para exatamente esse cenário.

## 5. Exportação

CSV com `real_fps` e `presented_fps` como **colunas distintas e nomeadas** — quem abrir em uma
planilha não deve conseguir somá-las por engano. Campos de texto são citados no padrão CSV, porque
nome de GPU contém vírgula.

JSON com o mesmo conteúdo, para ferramentas.

## 6. Verificação

- `tests/ctest/core/fork/benchmark_tests.cpp` — 9 casos, **9/9**: separação real/apresentado em
  linhas distintas, o cenário de FG que desacelera a emulação, fallback invalidando a comparação,
  compilação de shader sinalizada, execuções curtas e durações discrepantes, GPUs diferentes,
  comparação limpa sem avisos, ciclo begin/end pela ponte e cabeçalhos de CSV/JSON.
- `tests/ctest/core/presentation/presentation_metrics_tests.cpp` — 4 casos novos (**14/14** no
  arquivo): sessão contando o trecho inteiro e não a janela, real e apresentado separados na
  sessão, percentis expondo um pico de 120 ms que a média esconde, e a sessão inerte antes de
  `BeginSession` e depois de `EndSession`.

## 7. O que falta

O framework mede; **quem executa o protocolo é humano com hardware**. O roteiro está em
[docs/jogos-canario.md](jogos-canario.md): três execuções do mesmo trecho, mediana, 5 minutos de
descanso térmico entre elas. Sem dispositivo, o que existe aqui é a garantia de que os números,
quando vierem, serão comparáveis — e que os inválidos virão marcados.
