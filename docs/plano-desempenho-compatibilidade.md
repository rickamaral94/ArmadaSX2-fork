# Plano: desempenho e compatibilidade no Android ARM64

> Filosofia: **Compatibility first. Performance measured. Features optional.**

## O objetivo, afiado

"O melhor emulador de PS2" precisa de um recorte para virar trabalho em vez de slogan. O PCSX2 no
desktop tem vinte anos de maturidade e não é onde há espaço. **O espaço é Android ARM64**, e ali a
disputa é real: os concorrentes são recentes, a tradução para ARM é jovem, e quase ninguém mede
direito.

Então o objetivo operacional é: **no Odin 2 e em aparelhos da mesma classe, rodar mais jogos, mais
rápido e com menos variação de quadro do que qualquer outro emulador de PS2 para Android** — e ter
o número que prove isso.

---

## Etapa 0 — a trava que precisa sair primeiro

Toda medição que fizemos até hoje foi "jogue e torça por cair na mesma cena". É por isso que o
protocolo da Fase B exige cinco execuções por cenário: para afogar a variação em repetição.

**A ferramenta que elimina a variação já está no repositório e nunca foi usada: `pcsx2-gsrunner`.**
Ele reproduz *GS dumps* (`.gs`) — gravações determinísticas do fluxo de comandos do GS. Mesmo
input, mesmos desenhos, toda vez.

O que isso destrava, em ordem de importância:

1. **A/B de renderer com número confiável**, sem jogo, sem input, sem variação térmica de cena.
   `CoalesceRenderPasses` deixa de ser palpite e vira medição em uma tarde.
2. **Comparação de imagem por quadro** (hash perceptual). É o que pega `CoalesceRenderPasses`
   quebrando render *automaticamente*, que é o risco real dele.
3. **Roda no CI**, nos runners arm64 que já usamos. Correção de renderer deixa de depender do
   aparelho estar na mão.

O Dolphin faz exatamente isto há anos (eles chamam de FIFO logs) e é metade do motivo de
conseguirem mexer no renderer sem quebrar tudo.

**Entregável:** um corpus de dumps dos jogos-canário + um alvo de CI que roda o gsrunner, compara
pHash contra referência e publica p50/p95/p99 de tempo de quadro por dump.

**Sem isto, todo o resto avança no escuro.** É a primeira coisa.

---

## Trilha A — tradução para ARM64

O que já sabemos, e corrige uma suposição minha: **o rec arm64 já faz block linking** (`iR5900-arm64.cpp`
emite um `B` como patch site, com desvio para o linker quando o alvo ainda não existe) e já tem
otimização de cadeia de flags. Não é um rec ingênuo. Então o trabalho aqui **não é assumir lacuna, é
medir onde ele perde**.

Referências que valem leitura de código, e por quê:

| Projeto | O que olhar | Por que transfere |
|---|---|---|
| **Dolphin `JitArm64`** | alocador de registradores, dobramento de constantes | o rec ARM64 mais maduro em emulação de console |
| **PPSSPP `IRJit`** | IR intermediária antes do emit | otimiza uma vez, emite para vários backends; o nosso vai direto de MIPS para ARM64 |
| **box64 / FEX-Emu** | avaliação preguiçosa de flags, caminho rápido de acesso à memória | são tradutores x86→ARM64: a obsessão deles é exatamente a nossa |
| **RPCS3** | LLVM como backend | outro ponto do trade-off tempo-de-compilação x qualidade |

O caminho de acesso à memória (`recVTLB-arm64.cpp`) merece atenção especial: é o que o FEX e o
box64 mais otimizam, é o que a alpha 16 acabou de corrigir, e num emulador de PS2 cada load/store
do EE passa por ali.

**A rede de segurança já existe e é boa:** `recompiler_tests` roda 1.909 casos em runner arm64
nativo a cada push, com veredito baseline-x-candidato que reprova regressão, teste sumido e teste
desativado. É exatamente a cobertura que a regra "não mexer no rec sem reprodução, A/B e teste de
regressão" exige. **Podemos mexer no rec — desde que passando por ela.**

**Entregável:** um relatório de onde o EE/VU rec gasta tempo no Odin 2 (perfil por bloco), com as
três maiores oportunidades nomeadas e uma delas implementada com A/B.

---

## Trilha B — configurações padrão baseadas em evidência

Auditei o conjunto atual contra o seu log. **A maior parte já está no melhor valor medido**, e vale
saber para não procurar ganho onde não tem: `blend=1`, `af=0`, `preloading=2`, `userhacks=0`,
`hpo=0`, MTVU, `vu1Instant`, `vuFlagHack`, `intcStat`, `waitLoop`. Duas escolhas já foram feitas
contra o upstream e estão certas: `hwRov=false` (inerte no Turnip) e `adrenoFbFetch=true`
(blending in-tile).

Sobram três candidatos, nenhum com medição:

1. **`CoalesceRenderPasses`** (hoje `false`) — agrupa desenhos no mesmo alvo num render pass só. Em
   GPU de tile cada fronteira de passe custa load+store de tile inteiro, que é largura de banda,
   que é o que satura a 3x. Risco real: é um escalonador que adia e reordena desenhos, 238 linhas,
   **sem teste**. Exige a Etapa 0 para validar imagem.
2. **`Upscaler = FSR1`** com resolução interna menor — a troca "2x + FSR" contra "3x + bilinear".
   Renderiza 2,25× menos pixels. **Maior retorno com menor risco dos três.**
3. **`GSBackThreadMode = 3`** (Pipelined) — GS em 74-82% nas janelas lentas; é a única alavanca de
   paralelismo não explorada.

**Entregável:** cada um medido em GS dump *e* em jogo, com veredito de virar padrão ou não.

---

## Trilha C — compatibilidade

Compatibilidade aqui vem quase toda do GameDB, que já aplica em massa e sozinho. O que falta não é
correção, é **descoberta**: não sabemos quais jogos quebram nem por quê, porque só temos os seus
relatos.

Duas coisas concretas:

1. **Corpus de canários maior**, com um dump por jogo, rodando no CI. Quebra de renderer passa a ser
   pega antes de chegar em você.
2. **`armsx2_overrides.yaml`** — a sobreposição de GameDB do fork já existe e já é usada (a série
   NASCAR entrou por ali no sync). É o mecanismo para correção nossa por jogo, sem esperar upstream.

---

## Trilha D — limpeza de código

Já achamos uma classe de sujeira e ela era real: **9 de 15 `DriverWorkaround` não tinham consumidor
nenhum** — o bit acendia, ia para o log e não fazia nada. Isso me fez atribuir custo a driver que
não pagava. Três já viraram vivos; o resto está classificado.

O padrão a procurar é esse mesmo: **estado que parece ativo e não é**. Candidatos conhecidos:

- a linha `Settings commit: vuThread=...` reporta valor pré-VM e mostrou `0` com MTVU ligado —
  quase me fez recomendar ligar algo já ligado;
- flags de configuração que o app Android escreve e o núcleo ignora (ou o contrário);
- caminhos de código atrás de `#ifdef` que nenhuma build ativa.

**Entregável:** um teste que reprova bit de workaround sem consumidor e sem marcação explícita de
"informativo" — para a sujeira não voltar.

---

## Coisas que a comunidade faz e nós não

1. **Banco público de compatibilidade e desempenho alimentado por telemetria opt-in.** Já emitimos
   blocos `@@FORK@@` estruturados com aparelho, driver, jogo, velocidade e percentis. Ninguém no
   Android tem uma base assim. Transformaria "padrão medido em n=1" em "padrão medido em n=mil", e
   seria um diferencial que nenhum concorrente tem. Precisa ser **opt-in explícito e anônimo**.
2. **Perfis de desempenho por jogo embarcados.** O GameDB distribui correção; ninguém distribui
   "SotC quer 2x no Adreno 740". O mecanismo já existe (`armsx2_overrides.yaml`).
3. **Percentil de tempo de quadro como portão de CI** — não só "os testes passam", mas "o p99 não
   regrediu neste dump". É o que impede a morte por mil cortes.

---

## Ordem de ataque, e por quê

1. **Etapa 0 (GS dumps + CI)** — destrava tudo. Sem medição determinística, as trilhas A e B
   produzem opinião.
2. **Trilha B** — retorno rápido, risco contido, e já temos os candidatos nomeados.
3. **Trilha A** — o ganho maior e mais duradouro, e o mais caro. Só depois de saber medir.
4. **Trilhas C e D** — contínuas, em paralelo.

O que **não** entra: geração de quadros (parada, a pedido), e qualquer coisa que mexa em
temporização lógica sem reprodução e A/B.

## O que eu preciso de você

- **Dumps.** Um `.gs` por jogo-canário, capturado no aparelho. É o insumo da Etapa 0, e é a única
  parte que eu não consigo produzir sozinho.
- **A keystore.** `tools/fork/make-signing-key.sh`, rodado aí. Enquanto a assinatura mudar a cada
  build, toda alpha apaga suas configurações, saves e cache — e é a causa real de você ficar
  reconfigurando.
