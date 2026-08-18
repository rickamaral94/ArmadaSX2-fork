# Fase 4 — endurecer o gerenciador de driver Vulkan

**Objetivo:** que o emulador só entregue ao `adrenotools_open_libvulkan` um arquivo que ele já
verificou, e que saiba dizer exatamente **qual** binário está rodando.

Estado: **itens 1 e 2 concluídos**. Os demais estão listados na seção 5.

---

## 1. O problema

O caminho `libadrenotools` funciona (ver [Fase 1](fase1-avaliacao-tecnica-base.md), seção 4), mas
confia no pacote. Um driver importado é aceito pelo que o `meta.json` **diz** ser — e o `meta.json`
vem dentro do próprio zip, escrito por quem o empacotou, sem ser confrontado com nada.

As consequências reais:

- um `.so` de outra arquitetura passa pela importação e só falha dentro do `dlopen`, no meio do
  boot, com uma mensagem que não chega ao usuário;
- um download interrompido produz um arquivo truncado que parece instalado;
- um link expirado devolve uma página de erro HTML que o navegador salva com nome de `.so`;
- dois builds diferentes com o mesmo nome ("Turnip 25.2") são indistinguíveis — o que torna o
  A/B da Fase 6 uma comparação de **rótulos**, não de binários.

## 2. Item 1 — validação e identidade (feito)

`pcsx2/Fork/ForkDriverPackage.{h,cpp}` responde duas perguntas antes de qualquer carregamento:

**É mesmo uma biblioteca compartilhada AArch64?** Leitura do cabeçalho ELF, com veredito
específico: `NotAnElf`, `NotElf64`, `NotLittleEndian`, `NotAarch64`, `NotSharedObject`,
`Truncated`, `MissingLibrary`, `LibraryUnreadable`. Cada um tem uma frase pronta para a UI —
um import recusado sem motivo legível vira relato de bug.

**Qual o SHA-256 exato deste arquivo?** Calculado em streaming (blocos de 64 KiB; um Turnip passa
de 10 MB e o aparelho já está com pouca RAM por causa do emulador). O hash não é segurança, é
**identidade**: é o que permite ao relatório de compatibilidade da Fase 7 dizer *qual* build
estava rodando, e ao A/B da Fase 6 comparar binários em vez de nomes.

Reusa o SHA-256 já vendored em `3rdparty/lzma` (LZMA SDK), que o PCSX2 já linka — sem
criptografia nova escrita à mão.

### A ordem das checagens é sobre a mensagem, não sobre eficiência

O magic ELF é conferido **antes** do tamanho. O caso real mais comum de import errado é uma página
de erro HTML salva com nome de `.so`: ela tem poucas dezenas de bytes, então checar tamanho
primeiro a classificaria como "download interrompido" — mandando o usuário rebaixar um arquivo que
nunca foi um driver. Só depois de o magic conferir é que um cabeçalho incompleto significa
truncamento de verdade.

Da mesma forma, a ordem de bytes é conferida antes da arquitetura: em um ELF big-endian o campo
`e_machine` está na ordem trocada, então reprová-lo por "arquitetura errada" reportaria a causa
errada.

> Essa ordem foi corrigida **porque um teste a pegou**. A primeira versão checava tamanho primeiro
> e classificava a página de erro como truncada.

### Verificação

`tests/ctest/core/fork/driver_package_tests.cpp` — 11 casos, executados localmente **11/11** e no
gate ARM64 (o alvo `fork_config_tests` já os inclui).

Cobertura: aceita `.so` AArch64; recusa x86-64, ARM32 e RISC-V; recusa 32 bits e big-endian; ordem
de bytes antes de arquitetura; executável não passa por biblioteca; página HTML não é chamada de
truncada; truncamento só para ELF de verdade; SHA-256 contra os vetores do FIPS 180-2; hash sensível
a um caractere; arquivo ausente distinto de ilegível; todo veredito com nome e frase.

Validar **não é** carregar: o módulo decide se vale a pena tentar, não abre o driver. Quem
responde "o que subiu de fato" é o item 2.

## 3. Item 2 — identidade real do ICD (feito)

`pcsx2/Fork/ForkDriverIdentity.{h,cpp}` responde **qual driver está realmente rodando**,
perguntando ao dispositivo em vez de ao `meta.json`.

### O problema que ninguém estava vendo

O carregamento de driver customizado tem um **fallback silencioso por desenho**: se
`adrenotools_open_libvulkan` falha, o `VKLoader` cai no loader do sistema e o boot continua. Essa é
a decisão certa — derrubar o emulador porque um driver importado não abriu seria pior. O efeito
colateral é que o usuário que selecionou Turnip e está rodando o blob da Qualcomm **não tinha como
saber**: a única pista era um `Console.Warning` no meio do log.

Isso contamina tudo o que vem depois. Um A/B da Fase 6 comparando "System vs Turnip A" pode estar
comparando o driver do sistema **com ele mesmo** e concluindo que "Turnip não muda nada". Um
relatório de compatibilidade da Fase 7 registraria um driver que nunca rodou.

### Como o veredito é formado

Três fontes cruzadas:

1. **o que foi pedido** — `Vulkan::GetCustomDriverLoadOutcome()`, novo em `VKLoader`: havia
   seleção? o handle abriu? qual foi o erro?
2. **o que o dispositivo diz ser** — `VkPhysicalDeviceDriverProperties` (`driverName`,
   `driverInfo`), já capturado pelo `GSDeviceVK`, classificado pelo `GpuProfileDetector` que a base
   já tinha;
3. **a versão do Mesa** — extraída do `driverInfo`, que é onde o freedreno a publica
   (`"Mesa 25.2.0-devel (git-1a2b3c4)"`).

| Veredito | Significado |
|---|---|
| `SystemDriverByChoice` | nenhum driver customizado selecionado — o do sistema é o esperado |
| `CustomDriverActive` | pedido, aberto, e o dispositivo se identifica como Turnip |
| **`FellBackToSystem`** | **pedido, não abriu, o sistema assumiu** — o caso silencioso |
| **`CustomOpenedButNotTurnip`** | abriu, mas o ICD ativo não é Turnip — pacote repackado ou renomeado |

Os dois últimos sobem como **Warning** no log e respondem `IsUnexpected() == true`, que é o que a
UI vai usar para avisar em vez de mentir por omissão.

### Uma decisão explícita: não gritar lobo

Sem `VK_KHR_driver_properties` não há como saber *quem* abriu. Nesse caso o módulo **não acusa
divergência** — um aviso falso em aparelhos cujo driver simplesmente não reporta identidade ensina
o usuário a ignorar os avisos verdadeiros. Mas a falha de **abrir** continua sendo detectada, porque
essa é fato do carregador e não depende de extensão nenhuma.

### Verificação

`tests/ctest/core/fork/driver_identity_tests.cpp` — 10 casos, **10/10** localmente: a matriz
completa de vereditos, o caso sem `driverProperties`, o parsing da versão do Mesa (com prefixo,
sem patch, caixa alta, `"Mesa"` solto sem número, string da Qualcomm) e a publicação preservando o
SHA-256 do pacote informado antes do renderer subir.

A linha que isso produz para o log e para o relatório da Fase 7:

```
CustomDriverActive | driver MesaTurnip (turnip) | Mesa 25.2.0 | info "Mesa 25.2.0-devel (git-1a2b3c4)"
 | Vulkan 1.3.281 | GPU Adreno (TM) 750 | pedido libvulkan_freedreno.so | sha256 a1b2c3...
```

## 5. Itens restantes

| # | Item | Onde |
|---|---|---|
| 3 | Chave do shader cache incluindo o driver — um cache compilado pelo Qualcomm não pode ser servido ao Turnip | `VKShaderCache` |
| 4 | Erro de carga visível na UI (hoje o `Error` morre no log) e "System Driver" fixo no topo | `TryOpenAdrenotoolsDriver` → JNI → UI |
| 5 | Ligar o validador ao fluxo de import do Kotlin | `CustomDriver.installFromStream` + JNI |

O item 5 depende de uma função JNI nova — a primeira desde a superfície de configuração, e vale
avaliar se ela deve ser genérica (validar caminho → JSON) para não repetir o custo a cada
verificação futura.
