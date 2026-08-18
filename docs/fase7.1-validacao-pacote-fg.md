# Fase 7.1 — validar o pacote de Frame Generation na hora do import

O arquivo de Frame Generation é o **único** insumo do emulador que o usuário tem de comprar e
fornecer: o `Lossless.dll` do Lossless Scaling. Nada dele é redistribuído — o emulador lê os
shaders do arquivo do próprio usuário, em tempo de execução, do mesmo jeito que lê uma BIOS. O
`.gitignore` recusa `**/Lossless.dll` por nome (e só por nome: um `*.dll` genérico esconderia os
`.dll` legítimos que o upstream versiona).

## 1. O defeito

A tela de import validava a escolha assim:

```kotlin
NativeApp.lsfgAvailability(caminho) == DLL_UNREADABLE   // recusa
```

Só que `GSLsfg::GetUnavailableReason()` responde as travas de **hardware primeiro**:

```cpp
if (s_caps_known) {
    if (!s_is_vulkan)            return Unavailable::NotVulkan;
    if (s_adreno_generation < 7) return Unavailable::GpuUnsupported;
}
// só aqui o arquivo é aberto
```

Num aparelho incompatível — Mali, Adreno 6xx, renderer OpenGL — e **depois que qualquer jogo
subiu** (é quando as capacidades passam a ser conhecidas; num arranque frio elas não são, e aí a
validação funcionava), a resposta nunca era `DllUnreadable`. A validação aceitava qualquer arquivo:
um `.txt`, uma página de erro salva com nome de `.dll`, um download pela metade. O caminho ficava
gravado parecendo correto, e o erro só aparecia muito depois, em outro aparelho ou com outro
renderer, como "falha ao inicializar" — a mensagem que menos ajuda.

## 2. A correção

`pcsx2/Fork/ForkLsfgPackage.{h,cpp}`: um inspetor que responde **só sobre o arquivo** — sem GPU,
sem renderer, sem depender de o backend de LSFG estar compilado. E responde mais que sim/não: diz
qual família de shaders o arquivo carrega, porque *"você escolheu o arquivo errado"* e *"seu
Lossless Scaling é antigo demais"* são problemas diferentes e só um se resolve escolhendo outro
arquivo.

Os ids de recurso **não são digitados de novo**. A tabela nome→id saiu de dentro de `GSLsfg.cpp`
para `GS/Renderers/Vulkan/GSLsfgShaderTable.h`, e agora tem dois leitores: o extrator, que puxa os
blobs no primeiro uso, e o inspetor, que só confere presença. Duas cópias dos números divergiriam
na primeira versão do Lossless Scaling que renumerasse um recurso — e a cópia divergente seria
justamente a que diz ao usuário que o arquivo dele está bom quando não está.

Vereditos: `Ok`, `Missing`, `Unreadable`, `NotAPortableExecutable`, `Truncated`, `NoResources`,
`NoShaderFamily`. Magic antes de tamanho, pela mesma lição do inspetor de drivers: um arquivo curto
**sem** magic é "isto não é um DLL", nunca "seu download foi interrompido".

Arquitetura é registrada e **não** reprova. O extrator lê recursos, e recurso não tem arquitetura;
reprovar um PE32 inventaria uma restrição que o código que consome o arquivo não tem.

## 3. O que só apareceu medindo o arquivo real

O dono do fork forneceu a cópia dele para teste. Ela nunca entrou no repositório; foi lida no
disco local, e o que ela mostrou:

| arquivo | veredito | recursos RCDATA | 3.1 | 3.1p |
|---|---|---|---|---|
| `Lossless.dll` inteiro (7.521.280 bytes) | `Ok` | 300 | completa | completa |
| truncado em 400 KB | `NoShaderFamily` | 3 | ausente | ausente |
| truncado em 4 MB | `NoShaderFamily` | 129 | ausente | ausente |
| página de erro HTML | `NotAPortableExecutable` | 0 | ausente | ausente |
| `WinPixEventRuntime.dll` (outro DLL real) | `NoShaderFamily` | 0 | ausente | ausente |

A segunda linha é o achado. **Na primeira versão do inspetor ela dava `Ok`, com as duas famílias
"completas".** A árvore de recursos mora no começo do `.rsrc` e os blobs vêm depois: num arquivo
de 7,5 MB cortado em 400 KB, a árvore inteira e as 300 entradas de dados ainda cabem — só os
shaders sumiram. Conferir a *entrada* aprovava um arquivo com 95% do conteúdo faltando.

A presença passou a ser medida no **blob**: a entrada de dados é resolvida (RVA → offset pelas
seções) e o tamanho declarado tem de caber no arquivo. É uma correção que nenhum teste sintético
ia sugerir — o PE que eu montava tinha os blobs logo depois das entradas, então cortar uma coisa
cortava a outra. O arquivo de verdade tem 6,8 MB entre elas.

## 4. Verificação

`tests/ctest/core/fork/lsfg_package_tests.cpp` — 15 casos, **15/15**, sob ASan+UBSan. Os PEs são
montados byte a byte dentro do teste: o único `Lossless.dll` que existe é o que o usuário comprou,
ele não pode ser versionado, e um teste que depende de um arquivo que o CI não tem é um teste que
não roda.

Cobrem: cada família aceita sozinha (uma versão traz 3.1 **ou** 3.1p — exigir as duas recusaria
arquivos que funcionam); as duas reportadas quando ambas estão lá; um DLL real que não é este
recusado com motivo acionável; uma família a que falta **um** recurso não é família; arquivo curto
sem magic não é "truncado"; magic sem cabeçalho é; **a árvore intacta com os blobs cortados**;
recurso de tamanho zero não conta; PE32 não é recusado por ser de 32 bits; RVA de recursos fora de
toda seção para o caminhamento em vez de ler bytes arbitrários como se fossem recursos; e todo
veredito com nome e frase.

Mais 1 caso em `bridge_tests.cpp` (`lsfg.inspect` respondendo sobre o arquivo sozinho) e o total da
suíte do fork em **98/98**.

## 5. O que a tela faz agora

O import pergunta ao inspetor, não à disponibilidade. Três desfechos:

- arquivo bom → caminho gravado;
- `NoShaderFamily` → "este DLL não traz os shaders de frame generation; confira se é o Lossless.dll
  e se está atualizado";
- qualquer outro → "este arquivo não é um Lossless.dll".

Quando a ponte **não pode ser consultada** (biblioteca nativa ainda não subiu), a escolha é
mantida. Recusar ali deixaria o usuário preso numa tela que não sabe explicar o porquê; a linha de
motivo continua reportando o que o runtime encontrar quando subir.
