# Fase 0 — Setup do fork, CI e política de sincronização

**Objetivo da fase:** ter o fork realmente criado (com história completa), um CI que compila o APK
Android arm64 a partir do código-fonte C++, e a base de teste de compatibilidade registrada.

**Critério de aceite:** o workflow `Fork · Android arm64 (debug APK)` conclui com sucesso e publica
um APK arm64; a lista de jogos-canário está versionada. *A verificação de boot da BIOS em
dispositivo real é manual e depende de hardware — não é feita pelo CI.*

---

## 1. Como este fork está montado

Este repositório **não** é um snapshot: contém a história completa do ARMSX2 (24.960 commits em
16/08/2026, commit-base `2a98726`). Nosso trabalho fica por cima dela, o que mantém `git log`,
`git blame` e merges do upstream funcionando.

### Remotos

| Remoto | URL | Uso |
|---|---|---|
| `origin` | `github.com/rickamaral94/Ps2-fork` | nosso fork |
| `armsx2` | `github.com/ARMSX2/ARMSX2` | upstream direto — origem dos merges |
| `pcsx2` | `github.com/PCSX2/pcsx2` | upstream de origem — referência e arqueologia |

Reproduzir em um clone novo:

```sh
git clone https://github.com/rickamaral94/Ps2-fork
cd Ps2-fork
git remote add armsx2 https://github.com/ARMSX2/ARMSX2.git
git remote add pcsx2  https://github.com/PCSX2/pcsx2.git
git fetch armsx2 master
```

Ou simplesmente: `./tools/fork/sync-upstream.sh --setup`

### Política de sincronização

- **Merge, não rebase**, ao trazer o upstream (`./tools/fork/sync-upstream.sh --merge`).
  Rebase de 25k commits reescreveria a base compartilhada; merge preserva a linha do tempo e
  torna cada conflito rastreável.
- Antes de qualquer merge: rodar `./tools/fork/fork-diff.sh` e revisar a superfície de contato.
- Depois de qualquer merge: CI verde + lista de jogos-canário reexecutada.

---

## 2. Superfície de contato com o upstream

Toda mudança nossa em arquivo existente do upstream é listada aqui. Se a tabela crescer sem
justificativa, o fork está saindo do trilho.

| Arquivo | Mudança | Motivo |
|---|---|---|
| `.github/workflows/nightly.yml` | `schedule` removido (só `workflow_dispatch`) | O nightly do upstream constrói macOS/Windows/Linux arm64 + iOS (~2 h) e **publica releases**. No nosso repositório ele rodaria diariamente no branch padrão, gastando CI e criando releases que não são nossas. |
| `pcsx2/CMakeLists.txt` | +6 linhas | registra os fontes de `GSPresentationMetrics` e de `Fork/` |
| `pcsx2/VMManager.cpp` | +1 include, +1 chamada | `ForkRuntime::LoadSettings` no mesmo ponto em que o upstream reconstrói a configuração — é o que dá override por jogo de graça. Ver docs/superficie-configuracao-fork.md |
| `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp` | 6 ganchos pequenos | pontos de medição da apresentação — o único lugar por onde todo quadro apresentado passa. Ver docs/fase2-presentation-metrics.md |
| `pcsx2/ImGui/ImGuiOverlays.cpp` | +1 linha de overlay | exibe FPS real x apresentado |
| `pcsx2/GS/Renderers/Vulkan/VKShaderCache.{h,cpp}` | nome do cache de pipeline por driver + poda | com nome fixo, alternar drivers sobrescreve o cache do outro e o A/B da Fase 6 mediria compilação a frio (Fase 4, item 3) |
| `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.h` | +1 acessor `GetDeviceDriverProperties` | a chave do cache precisa do `driverID` |
| `pcsx2/GS/Renderers/Vulkan/VKLoader.cpp` | registra o resultado do carregamento do driver | o fallback para o driver do sistema é silencioso; sem esse registro não há como saber que driver está rodando (Fase 4, item 2) |
| `platforms/android/.../ui/common/DriverManagerSection.kt` | +1 condicional, +1 card de status | esconde a seção em GPU incompatível (Fase 3); mostra o driver ativo e avisa quando não é o pedido (Fase 4, item 4) |
| `platforms/android/.../kr/co/iefriends/pcsx2/NativeApp.java` | +1 declaração `forkQuery` | porta única do fork; consultas novas não custam JNI nova |
| `platforms/android/app/src/main/cpp/native-lib.cpp` | +1 função JNI | implementação da porta única |
| `platforms/android/.../com/armsx2/CustomDriver.kt` | valida o `.so` e grava o SHA-256 antes de instalar | Fase 4, item 5 |
| `tests/ctest/core/CMakeLists.txt` | +2 linhas | `add_subdirectory(presentation)` |
| `.github/dependabot.yml` | removido | O Dependabot do upstream mantém as versões das *actions* de um pipeline que não é nosso. Cada PR semanal dele dispararia `build-all.yml` (~2 h de CI arm64) no nosso repositório, para atualizar workflows que só o upstream usa. As atualizações chegam pelo merge do upstream. |
| `.github/workflows/upload-moonstore.yml` | gatilho `workflow_run` removido (só `workflow_dispatch`) | Publica builds em uma loja de terceiros do upstream. Nossos builds não devem ir para lá. |

`build-all.yml` do upstream é mantido **intocado**. Ele não dispara nos nossos pushes (o filtro
de push é `branches: master`), mas dispara em *pull requests* — e isso é desejado: os jobs arm64 de
PC são a guarda de que uma mudança motivada por mobile não alterou o core compartilhado. Se o custo
de CI se tornar um problema, a decisão de restringi-lo é explícita e vem documentada aqui, não por
omissão.

Arquivos **novos** (não contam como superfície de contato): `FORK.md`, `docs/**`,
`tools/fork/**`, `.github/workflows/fork-android-arm64.yml`.

O `README.md` da raiz é mantido **exatamente** como o do upstream. Nossa identidade fica em
`FORK.md`.

---

## 3. CI — build Android arm64

Workflow: `.github/workflows/fork-android-arm64.yml`

- Dispara em push nos nossos branches (`claude/**`, `fork/**`, `main`), em PRs e sob demanda.
- Compila `:app:assembleGithubDebug` — o *build type* `debug` do projeto compila o núcleo C++ a
  partir do fonte (`CMAKE_BUILD_TYPE=Debug` com `-O3`), **sem** LTO, o que dá um ciclo de CI
  bem mais curto que o release sem esconder erros de compilação nossos.
- Sabor `github` (e não `play`): é o que habilita o gerenciador de driver customizado e os
  caminhos de armazenamento que precisamos testar.
- Publica o APK como artefato `ps2fork-android-arm64-debug`.
- Um job secundário (`fork-hygiene`, não bloqueante) roda `tools/fork/fork-diff.sh` e imprime a
  superfície de contato com o upstream a cada push.

### Build local (referência)

```sh
cd platforms/android
python3 app/src/main/cpp/3rdparty/shaderc/utils/git-sync-deps   # deps do shaderc não são submódulo
rustup target add aarch64-linux-android                          # librashader
./gradlew :app:assembleGithubDebug
# saída: app/build/outputs/apk/github/debug/*.apk
```

Requisitos: JDK 17, Android SDK, NDK `28.2.13676358`, CMake `3.31.6`.
O NDK e o CMake são **pinados** de propósito: um A/B de driver ou de frame generation não
significa nada se o toolchain mudar entre as duas medições.

---

## 4. Verificação manual pós-CI (precisa de dispositivo)

O CI garante que compila. O que ele **não** garante, e precisa ser feito à mão uma vez por fase:

1. Instalar o APK em um dispositivo Snapdragon/Adreno.
2. Boot da BIOS do PS2 (dump próprio) até o menu do console.
3. Um jogo-canário 2D e um 3D rodando por 5 minutos sem crash.
4. Conferir no log: GPU detectada, driver Vulkan em uso, versão do Vulkan.

Registrar o resultado em `docs/jogos-canario.md` (tabela de execuções).

---

## 5. Registro — primeira execução verde

| Item | Valor |
|---|---|
| Run | [32114881154](https://github.com/rickamaral94/Ps2-fork/actions/runs/32114881154) |
| Commit | `1032f021` |
| Data | 18/08/2026 08:08 UTC |
| Job `Android arm64 · debug APK` | sucesso — passo de build 29 min 35 s |
| Job `Superfície de contato` | sucesso — 3 arquivos do upstream modificados, 8 novos, 0 no núcleo |
| Artefato | `ps2fork-android-arm64-debug`, 76,8 MB |
| SHA-256 do artefato | `ae2ca503c4577c060530c2b01a09f6f47462dea271a4453a40c1c559522933ba` |

O que isso prova: a árvore deste fork compila o núcleo C++ para `arm64-v8a` e empacota um APK.
O que **não** prova: que o APK inicializa a BIOS em um dispositivo real — isso é a verificação
manual da seção 4 e depende de hardware.
