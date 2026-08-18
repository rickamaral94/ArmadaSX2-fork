# Fase 7 — esqueleto de Frame Generation

**Objetivo:** provar o contrato antes de sintetizar qualquer pixel.

**Critério de aceite:** FG ligado não altera FPS real, áudio nem input; o overlay separa real de
apresentado.

---

## 1. O que esta fase NÃO faz

Ela não interpola. **E também não apresenta quadro duplicado.**

Isso corrige o plano inicial, que previa um backend duplicando quadros para exercitar o caminho.
Ao implementar, duas coisas ficaram claras:

1. apresentar um quadro extra é mudança real no caminho de present — aquisição de imagem,
   semáforos, ritmo — e pertence ao backend da Fase 8, junto com os pixels;
2. contar como "gerado" um quadro que não foi apresentado corromperia exatamente a métrica que a
   Fase 2 construiu para não deixar ninguém se enganar.

Então esta fase entrega a **régua**: a política decide, registra e mostra. Nenhum quadro a mais
chega à tela.

## 2. Por que a régua vem antes dos pixels

Referência que o projeto adotou: o frame generation do GameHub e do LSFG-Android é a **mesma
tecnologia** — Lossless Scaling portado para Vulkan. A consistência que se atribui a eles vem, em
boa parte, dos controles de **pacing**: bypass, alinhamento com vsync, presets de ritmo, teto de
FPS, profundidade de fila, suavização de jitter. São controles de *ritmo*, não de qualidade de
interpolação.

Ou seja: o que faz FG parecer bom é o pacing, e é isso que esta fase constrói primeiro.

> Nota de licença e de origem: o LSFG-Android é MIT no wrapper, mas a interpolação exige que o
> usuário forneça um `Lossless.dll` **proprietário** comprado por ele — os shaders são extraídos
> desse arquivo. O app tem ainda licença própria proibindo distribuição em loja. Sobre o GameHub
> não há fonte nem licença confirmadas, e circulam relatos de redistribuição questionável. Nada
> disso pode virar código nosso: a referência é **conceitual**, e nenhuma linha foi copiada.
>
> Vale registrar que a base já integra LSFG (`GSLsfg.cpp`) de forma **melhor** que o app avulso:
> como o ARMSX2 é dono da própria swapchain, não precisa de MediaProjection nem de overlay do
> sistema — evitando os ~50-80 ms de latência que essa abordagem acrescenta. Continua opcional e
> dependente do DLL do usuário, então não pode ser o recurso principal.

## 3. A régua

A política é pura e sem estado, exercitável sem GPU. A **ordem** dos degraus é o desenho:

| # | Condição | Estado | Por que nessa posição |
|---|---|---|---|
| 1 | Modo `off` / GPU incompatível | `Disabled` | caminho rápido; desligado não paga nem a leitura das métricas |
| 2 | **Sem quadro novo do jogo** | `Waiting` | protege contra o pior cenário: números subindo com o jogo congelado |
| 3 | **FPS real abaixo do mínimo** | `Waiting` | vem **antes** da estabilidade porque emulação lenta pode ser perfeitamente regular — e passaria no teste de ritmo violando a regra que mais importa |
| 4 | Ritmo instável (p99 > 1,5× a média) | `Waiting` | interpolar sobre frametime irregular piora a percepção |
| 5 | Geração estourou o orçamento | `Suspended` | estado distinto de `Waiting`: houve tentativa e ela custou caro |

O degrau 3 é a regra do projeto escrita em código: **22 FPS reais mostrando 44 não é sucesso.**

## 4. Estado sempre visível

O overlay mostra o estado sempre que FG está ligado — inclusive, e principalmente, quando **não**
está engatado. Sem isso o usuário liga a opção, não vê nada acontecer e não consegue distinguir
"não engatou" de "está quebrado". Desligado é o único caso silencioso.

O aviso obrigatório é constante do módulo (`ForkFrameGen::USER_WARNING`), não texto solto na UI —
assim ela não pode esquecê-lo nem reescrevê-lo de forma mais otimista:

> Frame Generation melhora a fluidez percebida, mas NÃO aumenta a velocidade da emulação.

## 5. Configuração

```ini
[Fork]
FrameGen.Mode = off        ; off | auto | 2x
FrameGen.BudgetMs = 6.0
FrameGen.MinRealFps = 25.0
```

Com override por jogo, pelas camadas (Fase 5). Modo inválido cai em `off`: o padrão seguro é não
fazer nada.

## 5.1 A tela

Política e backend ficam na **mesma seção** da UI — a de Frame Generation, no *Performance* e no
menu de pausa — e nessa ordem: o modo (Off / Auto / 2x), o aviso obrigatório logo abaixo dele, o
estado ao vivo quando ligado, e só então o backend LSFG com o seletor do `Lossless.dll` que o
usuário fornece. Separar as duas coisas em telas diferentes seria convidar exatamente a leitura
errada: que ligar um backend basta, e que a régua é um detalhe de outro lugar.

O aviso exibido vem do núcleo pela ponte (`framegen.status` → `warning`), não de uma string da
UI. Uma cópia no frontend pode ser suavizada — por uma edição bem-intencionada ou por uma
tradução — até virar "FG aumenta o desempenho", que é precisamente a frase proibida. Quando a
ponte não responde (biblioteca nativa ainda não subiu, núcleo antigo), a UI mostra o texto de
reserva dela: sem aviso é o único estado que a seção não pode ter.

`FrameGen.BudgetMs` e `FrameGen.MinRealFps` **não** estão na tela. São as travas da régua, e
oferecê-las ao lado do botão que liga o recurso é oferecer o caminho de afrouxá-las até FG engatar
em cima de uma emulação lenta. Quem precisa mexer nelas tem a seção `[Fork]` do INI — e aí é uma
decisão consciente, não um deslize de dois toques.

O modo viaja pela mesma máquina de configuração das opções do upstream (`Settings.forkFrameGenMode`
→ `put("Fork", "FrameGen.Mode", …)`), o que lhe dá persistência, exportação e override por jogo sem
código novo. A escrita fica em `applyTo()`, não em `writeGsToNative()`: o caminho de GS ao vivo
pula o `commitSettings()`, e é ele que faz `VMManager::LoadSettings` — logo `ForkRuntime::LoadSettings`
— reler a seção `[Fork]`. Uma chave nossa escrita pelo caminho de GS seria persistida sem tomar
efeito, que é o pior dos dois resultados.

## 6. Verificação

`tests/ctest/core/fork/framegen_tests.cpp` — 11 casos, **11/11**: engate em quadro saudável;
`off`/incompatível não gerando nada; **nunca gerar sem quadro novo, nem em modo 2x**; recusa a
suavizar emulação lenta; lento-mas-regular ainda recusado por ser lento (prova a ordem dos
degraus); recusa a ritmo instável; orçamento estourado suspendendo em vez de esperar, com o limite
exato ainda cabendo; parsing de modo caindo em `off`; a linha de status falando sempre que ligado;
todo motivo com texto; e o aviso dizendo o que precisa dizer.

`tests/ctest/core/fork/bridge_tests.cpp` — 11 casos, **11/11**, dois deles novos: `framegen.status`
levando o aviso obrigatório verbatim (se ele parar de vir por ali, a tela fica sem aviso), e a
consulta respondendo "desligado" — não erro — antes de qualquer renderer, porque a tela de
configuração abre antes de qualquer jogo rodar e é ali que o modo é escolhido.

## 7. Próximo

Fase 8: o backend de verdade — optical flow em compute shader (FG-A), apresentando de fato o quadro
sintetizado. A régua desta fase é o que vai impedir esse backend de "ter sucesso" às custas da
emulação.
