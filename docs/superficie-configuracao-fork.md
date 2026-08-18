# Superfície de configuração do fork

**Problema:** cada opção nova no modelo do upstream custa um bit em `Config.h` mais a propagação
por `Pcsx2Config.cpp`, `GS.cpp`, `FullscreenUI`, o Qt, o `Settings.kt` do Android e o
`native-lib.cpp` — **sete arquivos do upstream, repetidos a cada recurso**. Com driver Turnip por
jogo, frame generation (off/auto/2x), métricas e perfis por GPU no roadmap, esse caminho tornaria
cada merge do upstream mais caro que o recurso que o motivou.

**Solução:** uma tabela declarativa, uma seção de INI, e nenhuma nova ligação por opção.

---

## 1. Como funciona

```
                         pcsx2/Fork/ForkConfig.cpp
                         ┌──────────────────────────────┐
                         │ TABELA: chave, tipo, padrão, │
                         │ descrição — uma linha por     │
                         │ opção do fork                 │
                         └───────────────┬──────────────┘
                                         │
   INI do usuário  [Fork]                │  leitura sem lock
   INI do jogo     [Fork]  ──► camadas ──┤  (escalares em atômicos)
   (o PCSX2 já mescla)                   │
                                         ▼
             VMManager::LoadSettings ──► ForkRuntime::LoadSettings ──► observadores
                    (1 linha)                                          (aplicam nos módulos)
```

**Override por jogo sai de graça.** A leitura passa pelo `SettingsInterface` em camadas que o
PCSX2 já monta (base + jogo). Uma chave `[Fork]` no INI do jogo prevalece sobre a global
exatamente como qualquer opção do upstream — sem uma linha de código nossa para isso.

**A escrita é sempre na camada base.** Persistir por jogo é atribuição do frontend, que é quem
sabe onde o INI do jogo vive; a leitura acima respeita o que ele escrever.

## 2. Acrescentar uma opção

Duas edições, **ambas em arquivos nossos**:

1. um valor no `enum class Option` (`pcsx2/Fork/ForkConfig.h`);
2. uma linha na tabela `OPTIONS` (`pcsx2/Fork/ForkConfig.cpp`) com chave, tipo, padrão e descrição.

E, se algum módulo precisar reagir na hora, uma linha em `ForkRuntime::ApplyToModules`.

Nada mais. Sem `Config.h`, sem JNI nova, sem UI nova para a opção existir e valer. Um
`static_assert` trava o alinhamento entre o enum e a tabela — desalinhados, uma opção leria o valor
da vizinha, sem erro e sem log.

## 3. Android: zero mudanças no upstream

`NativeApp.setSetting(section, key, type, value)` e `NativeApp.gameIniPut(section, key, value)` já
são **genéricos**. O wrapper `com.armsx2.fork.ForkSettings` (arquivo nosso) escreve na seção
`[Fork]` global ou no INI do jogo usando essas funções que já existem.

Resultado: a superfície funciona ponta a ponta — UI Android grava, núcleo lê com override por jogo
— **sem tocar em nenhum arquivo Java ou JNI do upstream**.

## 4. Contato com o upstream

| Arquivo | Mudança |
|---|---|
| `pcsx2/VMManager.cpp` | +1 include, +1 chamada em `LoadSettings` |
| `pcsx2/CMakeLists.txt` | +4 linhas (fontes) |
| `pcsx2/ImGui/ImGuiOverlays.cpp` | a linha de overlay passa a ler a opção do fork |

Uma linha em `VMManager` é o **único** ponto de contato com o carregamento de configuração, e ele
não cresce: o próximo módulo do fork se registra em `ForkRuntime`, não em `VMManager`.

## 5. Opções existentes

| Chave (`[Fork]`) | Tipo | Padrão | O que faz |
|---|---|---|---|
| `PresentationMetrics.Enabled` | bool | `false` | mede FPS real x apresentado, frametime, 1% low |
| `PresentationMetrics.Overlay` | bool | `false` | desenha a linha de cadência no OSD |

As duas são separadas de propósito: medir para o log e mostrar na tela são decisões diferentes, e o
benchmark A/B da Fase 6 vai querer medir sem poluir a captura de tela.

Exemplo de INI:

```ini
[Fork]
PresentationMetrics.Enabled = true
PresentationMetrics.Overlay = true
```

## 6. Decisões que valem registrar

- **Valor inválido cai no padrão, não em zero.** Um INI editado à mão com `sim` não deve significar
  silenciosamente "desligado".
- **Cada carregamento começa do padrão.** Sem isso, uma opção ligada em um jogo continuaria ligada
  no próximo — o bug clássico de override por jogo mal feito. Tem teste dedicado.
- **`SetAndSave` valida antes de escrever.** Gravar lixo no INI do usuário e descobrir no próximo
  boot é pior que recusar agora.
- **Escalares em atômicos, strings sob mutex.** O caminho de apresentação lê booleanos por quadro;
  strings (qual driver, qual perfil) são lidas em decisões, não por quadro.

## 7. Verificação

`tests/ctest/core/fork/fork_config_tests.cpp` — 9 casos: alinhamento tabela/enum, chave desconhecida
ignorada, padrões, leitura da seção, cada load partindo do padrão, valor inválido, round-trip de
texto, observador disparando, e `SetAndSave` recusando valor inválido sem sujar o estado.

Compilados e executados localmente: **9/9**. A partir de agora o gate da Fase 0.5 também compila e
roda as suítes do fork na perna *candidate*, com critério de **zero falhas** — a regra diferencial
existe para a suíte herdada do upstream, não para a que escrevemos hoje.
