# Fase 3 — `gpu_capabilities`

**Objetivo:** um veredito único e explicável sobre o que este aparelho suporta, para que a troca de
driver Turnip nunca seja oferecida onde ela não pode funcionar.

**Critério de aceite:** em Mali/PowerVR a seção de driver não aparece.

---

## 1. O que já existia (e não foi reimplementado)

A base resolve bastante coisa. `GpuProfileDetector::Resolve` devolve `GpuProfileSelection` com
arquitetura (`Adreno6xx/7xx/8xx`, famílias Mali, PowerVR), identidade de driver
(`MesaTurnip`, `QualcommProprietary`, `ArmProprietary`…), versão do driver e uma tabela de defeitos
conhecidos. No lado Android, `GpuInfo.kt` sonda `GL_VENDOR/GL_RENDERER/GL_VERSION`.

**O que faltava não era detecção — era a conclusão.** "Podemos oferecer Turnip aqui?" estava
implícita, espalhada entre a UI e o carregador. Espalhar essa decisão é exatamente como se acaba
tentando carregar um driver freedreno em uma Mali.

## 2. A regra

```
não é Android            → NotAndroid
fabricante != Adreno     → UnsupportedVendor              (Mali, PowerVR, Xclipse, Apple, desconhecida)
Adreno < 6xx             → UnsupportedAdrenoGeneration
Android < API 29         → UnsupportedAndroidVersion
resto                    → Supported
```

Três decisões que valem explicação:

**O fabricante é checado ANTES da versão do Android.** Uma Mali em Android 9 é recusada por ser
Mali, não por ser Android 9 — dizer "atualize o sistema" ao dono de uma Mali é mandá-lo perseguir
algo que nunca vai funcionar.

**API 29 é o piso porque é o piso do carregador**, não uma estimativa: `VKLoader` chama
`adrenotools_open_libvulkan` com `tmpLibDir = nullptr`, ou seja, pelo caminho memfd. Abaixo de 29 a
funcionalidade não é arriscada, é inexistente.

**SDK desconhecido não esconde a funcionalidade.** `android_sdk == 0` significa "não consegui ler a
propriedade", não "versão 0". Recusar aí puniria aparelhos capazes por causa de uma leitura falha —
e o carregador ainda tem o próprio fallback para o driver do sistema.

**Xclipse não entra.** Existem pacotes para Xclipse em formato AdrenoTools, mas não são Turnip.
Confundir os dois levaria a oferecer freedreno para uma GPU RDNA.

## 3. Onde vive

| Peça | Arquivo |
|---|---|
| Regra + veredito (nativo) | `pcsx2/Fork/ForkGpuCapabilities.{h,cpp}` (novo) |
| Publicação do veredito | `GSDeviceVK::Create`, logo após o perfil ser resolvido (+1 chamada) |
| Espelho para a UI pré-boot | `com.armsx2.fork.ForkGpuCapabilities` (novo) |
| Gate da seção de driver | `DriverManagerSection.kt` (+1 condicional) |

O espelho em Kotlin existe por necessidade, não por preguiça: o driver é escolhido com o emulador
**parado**, antes de qualquer renderer, então não há dispositivo Vulkan a quem perguntar. A fonte
ali é a sonda GL que `GpuInfo` já faz — que lê o driver do **sistema**, que é justamente o que se
quer para decidir.

> Se a regra mudar, mude nos dois lugares. O teste nativo é quem define o comportamento correto.

## 4. "Não sei" não é "não"

`TurnipSupport::Unknown` (nativo) e `UNKNOWN` (Kotlin) existem separados de "não suportado". Antes
de qualquer sonda, a UI não deve afirmar ao usuário que o aparelho dele não serve. E a seção só
desaparece com **certeza negativa** — com a sonda falhando ela continua visível, porque sumir por
causa de uma leitura falha é indistinguível de um bug para quem está olhando.

Quando ela desaparece, aparece **o motivo escrito**. Uma seção que some sem explicação vira relato
de bug.

## 5. Verificação

`tests/ctest/core/fork/gpu_capabilities_tests.cpp` — 11 casos, arch-neutral, sem GPU. Executados
localmente: **11/11**.

Cobertura: as três gerações Adreno suportadas; **todo** fabricante não-Adreno recusado cruzando 5
fabricantes × 4 arquiteturas (com Android novo, para provar que nenhuma versão de sistema torna
isso aceitável); Xclipse fora; Adreno 2xx-5xx recusada pela geração; ordem fabricante-antes-de-
versão; piso de API 29 nos dois lados da fronteira; SDK desconhecido não escondendo a
funcionalidade; plataformas não-Android; todo estado tendo string e motivo; formatação de versão do
Vulkan; e "não sondado" diferente de "não suportado".

## 6. O que isto habilita

A Fase 4 (endurecer o gerenciador de driver: SHA-256, validação, identidade real do ICD) e a Fase 5
(driver por jogo) passam a ter de quem perguntar antes de oferecer qualquer coisa. E `DescribeForLog`
já entrega a linha que o relatório de compatibilidade da Fase 7 precisa:

```
GPU Adreno 750 (Adreno, Adreno7xx) | Vulkan 1.3.281 | Android SDK 35 | driver ativo MesaTurnip | Turnip: Supported
```
