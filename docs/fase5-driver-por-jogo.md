# Fase 5 — driver Vulkan por jogo

**Objetivo:** um jogo específico poder usar outro driver — ou forçar o do sistema — sem mudar a
escolha global.

**Critério de aceite:** override por jogo aplicado antes do primeiro `MTGS::Open`.

---

## 1. Por que quase não custou

A [superfície de configuração do fork](superficie-configuracao-fork.md) lê pelo `SettingsInterface`
**em camadas** que o PCSX2 já monta (base + jogo). Então bastou a seleção de driver **morar na
configuração** em vez de num estado próprio do frontend: uma chave `[Fork]` no INI do jogo passa a
prevalecer sobre a global exatamente como qualquer opção do upstream, sem uma linha de código nossa
para mesclar camadas.

O que mudou de desenho: antes o Kotlin chamava `setCustomVulkanDriver` direto. Agora ele também
grava a seleção na configuração, e **o nativo aplica** — em `ForkRuntime`, chamado de
`VMManager::LoadSettings`.

## 2. Por que o momento está certo

`VMManager::LoadSettings` roda na inicialização da VM, **antes** do primeiro `MTGS::Open` — e é
esse primeiro Open que dispara o `LoadVulkanLibrary` onde o `VKLoader` lê o driver.

Isso resolve o critério de aceite sem ninguém coordenar ordem de chamadas: o ponto de aplicação é o
mesmo em que o upstream reconstrói toda a configuração, então o override chega a tempo **por
construção**, e não por sorte de sequência.

## 3. Os três estados, e por que "vazio" não bastava

| `Driver.Mode` | Significado |
|---|---|
| `inherit` (padrão) | este jogo não opina — segue o global |
| `system` | força o driver do sistema **neste jogo**, mesmo com Turnip global |
| `custom` | usa os caminhos configurados |

`inherit` existe porque "vazio" não distingue *"este jogo não opina"* de *"este jogo quer o driver
do sistema"*. E é justamente forçar o sistema em **um** jogo — o que quebra no Turnip — enquanto o
global segue Turnip que é o caso de uso principal do override. Sem o terceiro estado, esse caso
seria inexprimível.

## 4. Configuração quebrada cai em `system`, não em `inherit`

`custom` sem diretório, soname ou hook dir vira `System`. Acontece de verdade: INI editado à mão,
pacote apagado por fora, migração incompleta.

Cair em `System` deixa o jogo rodando de forma previsível e visível. Cair em `Inherit` o deixaria
com **o driver de outro jogo** — pior, porque parece que funcionou.

## 5. Como o frontend grava

```kotlin
ForkSettings.selectDriverGlobally(paths)   // ou null para o driver do sistema
ForkSettings.selectDriverForGame(paths)    // ou null para forçar o sistema neste jogo
ForkSettings.clearGameDriverOverride()     // volta a seguir o global
```

Nenhuma função JNI nova: `NativeApp.setSetting` e `NativeApp.gameIniPut` já são genéricos.

## 6. Verificação

4 casos novos em `fork_config_tests.cpp` (**13/13** no arquivo): os três estados resolvidos; vazio e
lixo significando "não opino"; `custom` sem caminhos caindo em `System` e não em `Inherit` (os três
caminhos obrigatórios testados um a um); e uma camada de jogo com `system` vencendo um global
`custom`.

O que não dá para verificar sem dispositivo é o efeito final — o driver certo subindo para o jogo
certo. Isso o card de status da Fase 4 mostra, e o log registra.
