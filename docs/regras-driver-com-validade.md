# Regras de driver com prazo de validade

> Filosofia: **Compatibility first. Performance measured. Features optional.**

## O problema

Toda lista de defeitos de driver em emulador é *write-only*. Um bug é encontrado, uma regra é
escrita, e ela fica lá para sempre. Ninguém volta para perguntar se ainda é verdade — não por
descuido, mas porque **não existe mecanismo que faça a pergunta**.

O caso concreto deste fork, medido:

| | |
|---|---|
| regra | `vk-turnip-attachment-self-read` |
| onde foi provada | Adreno 650, Turnip/Mesa 26.1.2 (ARMSX2 #442, Tales of the Abyss) |
| onde ela dispara hoje | **Adreno 740, Turnip/Mesa 26.3.0-devel** |
| o que ela custa | desliga `texture_barrier`, o que desliga `framebuffer_fetch`, o que força **uma cópia inteira do render target por draw de feedback** |
| o que o driver oferece | `VK_EXT_rasterization_order_attachment_access`, presente e habilitado |

Outra geração de GPU, outro driver, e a regra não tem faixa de versão nem de modelo. O comentário
do próprio upstream admite o custo: *"Kept as-is so a driver that stops carrying the bug recovers
the fast path for free."* Só que "de graça" nunca chega, porque nada avisa que o driver mudou.

Isto não é crítica ao upstream: generalizar conservadoramente é a decisão CERTA para quem atende
Mali, PowerVR e cinco gerações de Adreno. É justamente por isso que sobra para nós — um fork com
**um** aparelho pode ser preciso onde eles precisam ser prudentes.

## O que já existe, e o que falta

Existe (`GSGPUDriverProfile.cpp`):

- `DriverRule` com `min_version` e **`max_version_exclusive`** — a tabela JÁ sabe dizer "corrigido
  na versão X". Nenhuma regra usa.
- `RuleMatches()` e o laço que aplica `bugs` e `workarounds`.

Falta, e é o alicerce de tudo:

- **quais** regras casaram. O resolvedor só incrementa `profile.matched_rule_count`. Quando o log
  diz `rules=1`, não há como saber qual regra foi sem ler a tabela à mão — foi exatamente o que
  precisei fazer para descobrir a `vk-turnip-attachment-self-read`. Não se avisa sobre uma regra
  velha sem antes saber que ela é que disparou.

## Desenho

### Etapa 1 — identidade das regras casadas

`MobileDriverProfile` passa a carregar os ids das regras que casaram, não só a contagem. Vetor
pequeno de `const char*` para os literais da tabela: sem alocação, sem cópia de string.

O log do `@@FORK@@ identity` passa de `rules=1` para `rules=vk-turnip-attachment-self-read`. Sozinha,
esta etapa já elimina a arqueologia manual que eu tive de fazer.

### Etapa 2 — proveniência

Cada regra ganha um campo de onde foi verificada pela última vez:

```cpp
struct RuleProvenance
{
    const char* gpu;      // "Adreno 650"
    const char* driver;   // "Turnip/Mesa 26.1.2"
    u32 year, month;      // quando
    const char* evidence; // "ARMSX2 #442"
};
```

Não é decorativo, é o dado que a Etapa 3 compara. E preencher isso obriga quem escreve uma regra a
responder "onde você provou isto?" — pergunta que hoje ninguém faz.

Regras sem proveniência são permitidas e tratadas como "nunca verificadas", que é o estado honesto
para as 28 que já existem. Preencher retroativamente é trabalho de arqueologia, e só vale para as
que custam caro.

### Etapa 3 — o aviso

Quando uma regra dispara num driver **materialmente mais novo** que a proveniência dela, o log
avisa:

```
@@FORK_RULE_STALE@@ vk-turnip-attachment-self-read provada em 'Adreno 650 / Turnip Mesa 26.1.2'
  (2026-06), disparando em 'Adreno 740 / Turnip Mesa 26.3.0-devel' — custo: RT-copy por draw de
  feedback. Para testar sem ela: Fixes -> Sobrescrever Barreiras de Textura -> Ligado.
```

"Materialmente mais novo" precisa de critério, e o critério tem de ser conservador: **major do
driver diferente, ou família de GPU diferente**. Diferença de patch não é evidência de nada.

**ARMADILHA JÁ MEDIDA, e ela vale para qualquer regra com faixa de versão:** uma build `-devel` do
Mesa reporta o `driverVersion` do Vulkan como *<minor anterior>.99*, não como o número do nome. No
Odin 2, em 26 sessões, o Mesa se anuncia "Mesa 26.3.0-devel" e o PCSX2 casa regras contra
`version=26.2.99`. Uma comparação escrita contra "26.3" não casaria — e o silêncio seria total,
porque regra que não casa não loga nada. O comparador tem de usar o `raw` que o resolvedor já usa,
nunca o texto do nome.

### Etapa 4 — a saída

Para a `vk-turnip-attachment-self-read` a saída já existe: `OverrideTextureBarriers = 1` vence o
workaround, e o próprio código do fork diz que foi deixado assim "for A/B-ing this workaround's
cost". O aviso da Etapa 3 aponta para ela.

Para regras sem escape pronto, o desenho **não** propõe um botão genérico de "ignorar esta regra".
Um interruptor que desliga qualquer workaround é uma máquina de gerar relatos de bug inúteis. O
que se propõe é: o aviso nomeia a regra, e quem quiser testar abre uma issue com o log — o custo
de fricção aqui é uma feature, não um defeito.

## O que este desenho NÃO é

- **Não é** desligar workarounds automaticamente. Uma regra continua valendo até ser desmentida
  por medição. O mecanismo faz a pergunta; ele não responde sozinho.
- **Não é** telemetria. Nada sai do aparelho. O aviso é uma linha de log que o usuário escolhe
  mandar.
- **Não é** um sistema de expiração por data. "Seis meses" não é evidência. O gatilho é distância
  de hardware/driver, não calendário.

## Por que isto é nosso

Verificado contra `armsx2/master` em 2026-09-02: o upstream não tem nada disso, e a estrutura
`DriverRule` deles é a mesma que herdamos — a capacidade de expressar limite superior existe e está
sem uso, o que sugere que a pergunta nunca foi feita ali também.

E é estruturalmente algo que um projeto multi-vendor não faz: eles não têm o aparelho para
desmentir a própria regra. Nós temos um, e a sessão de 22/08 já produziu a evidência de que a
regra mais cara do nosso perfil pode estar obsoleta.

**Ressalva de escopo:** verifiquei contra ARMSX2 e PCSX2. Não survey ei todo o ecossistema Android.
A confiança é alta sobre o upstream e média sobre o resto.

## Ordem de execução, e a dependência

1. **Etapa 1** (ids das regras) — pequena, útil sozinha, sem risco. Pode ir a qualquer momento.
2. **O teste das barreiras no Odin 2** — pendente. É ele que decide se este mecanismo tem um caso
   real ou é engenharia especulativa.
3. **Etapas 2-4** — só depois de (2). Se o ROAA funcionar no 740, a primeira proveniência a ser
   escrita é a que desmente a regra, com número. Se não funcionar, a regra está certa, a
   proveniência confirma isso, e o mecanismo continua valendo para a próxima.

Escrever 2-4 antes de (2) seria construir a moldura antes de saber se há quadro.
