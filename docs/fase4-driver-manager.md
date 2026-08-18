# Fase 4 — endurecer o gerenciador de driver Vulkan

**Objetivo:** que o emulador só entregue ao `adrenotools_open_libvulkan` um arquivo que ele já
verificou, e que saiba dizer exatamente **qual** binário está rodando.

Estado: **item 1 concluído**. Os demais estão listados na seção 4.

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

## 3. O que isto ainda não faz

Validar **não é** carregar. O módulo não abre o driver nem consulta o dispositivo Vulkan; ele
decide se vale a pena tentar. A identidade real do ICD (Mesa, versão do Vulkan, GPU) é o item 2.

## 4. Itens restantes

| # | Item | Onde |
|---|---|---|
| 2 | Identidade real do ICD carregado (`VkPhysicalDeviceDriverProperties`) em vez do `meta.json` auto-declarado | novo `Vulkan::QueryLoadedDriverIdentity` + `GSDeviceVK::Create` |
| 3 | Chave do shader cache incluindo o driver — um cache compilado pelo Qualcomm não pode ser servido ao Turnip | `VKShaderCache` |
| 4 | Erro de carga visível na UI (hoje o `Error` morre no log) e "System Driver" fixo no topo | `TryOpenAdrenotoolsDriver` → JNI → UI |
| 5 | Ligar o validador ao fluxo de import do Kotlin | `CustomDriver.installFromStream` + JNI |

O item 5 depende de uma função JNI nova — a primeira desde a superfície de configuração, e vale
avaliar se ela deve ser genérica (validar caminho → JSON) para não repetir o custo a cada
verificação futura.
