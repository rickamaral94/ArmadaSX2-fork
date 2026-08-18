# Ps2-fork — fork Android ARM64 de ARMSX2

Fork de [ARMSX2](https://github.com/ARMSX2/ARMSX2) (GPL-3.0), que por sua vez é fork do
[PCSX2](https://github.com/PCSX2/pcsx2) (GPL-3.0). Este arquivo descreve **apenas o que é nosso**;
o `README.md` da raiz continua sendo o do upstream, intocado de propósito.

**Filosofia:** *Compatibility first. Performance measured. Features optional.*

## Objetivo

```
PS2 → GS → Vulkan → driver selecionável (Turnip) → pós-processamento
   → Frame Generation opcional → Android Surface
```

Sem comprometer a precisão da emulação e sem tocar em EE, IOP ou VU.

## Regras do fork (não negociáveis)

1. **O núcleo de emulação não se toca sem evidência — mas nada é intocável.** O objetivo do
   projeto é a maior compatibilidade e o melhor desempenho possíveis; se os dados de um teste em
   console real mostrarem que isso exige mexer em `pcsx2/arm64`, EE/IOP/VU ou em qualquer coisa
   herdada do upstream, mexemos.

   O que a regra exige é a ORDEM: **medir primeiro, mudar depois.** Uma mudança no núcleo só entra
   com (a) evidência concreta — log, GS dump, medição A/B, teste que reproduz; (b) um teste de
   regressão que falhe antes da correção; (c) registro na tabela de superfície de contato. Enquanto
   a evidência não existir, bug de JIT é reportado ao ARMSX2, não contornado aqui.

   O motivo não é reverência ao upstream: é que uma mudança de núcleo sem evidência custa merges
   mais caros para sempre e não se sabe se ajudou. Com evidência, o custo é justificado e mensurável.
2. **Diff mínimo.** Nossas mudanças vivem em arquivos novos. Alterações em arquivos existentes
   são limitadas aos ganchos listados em `docs/fase0-setup.md` e precisam de justificativa no
   commit.
3. **Nada entra ligado por padrão** sem A/B medido e sem forma de desligar.
4. **Nenhum código fechado, nenhum binário de terceiros redistribuído.** Drivers Vulkan são
   importados pelo usuário em runtime, nunca embutidos no APK.
5. **FPS real e FPS apresentado nunca são o mesmo número** em nenhum log, overlay ou relatório.

## Módulos planejados

| Módulo | Onde | Estado |
|---|---|---|
| `android_driver_manager` | `platforms/android/.../com/armsx2/driver/` | Fase 4-5 |
| `adrenotools_backend` | `pcsx2/GS/Renderers/Vulkan/VKLoader.cpp` (existente) | Fase 4 |
| `gpu_capabilities` | `pcsx2/Fork/ForkGpuCapabilities.*` + `com.armsx2.fork` | **feito** (Fase 3) |
| `fork_config` | `pcsx2/Fork/` + `com.armsx2.fork` | **feito** (infraestrutura) |
| `presentation_metrics` | `pcsx2/GS/Renderers/Common/GSPresentationMetrics.*` | **feito** (Fase 2) |
| `frame_generation` | `pcsx2/GS/Renderers/Vulkan/FrameGen/` | Fase 7-8 |
| `frame_interpolation` | `pcsx2/GS/Renderers/Vulkan/FrameGen/backends/` | Fase 8 |

## Documentação

- [Fase 1 — avaliação técnica e escolha da base](docs/fase1-avaliacao-tecnica-base.md)
- [Fase 0 — setup do fork, CI e política de sincronização](docs/fase0-setup.md)
- [Fase 0.5 — gate de correctness do recompilador ARM64](docs/fase0.5-correctness-arm64.md)
- [Superfície de configuração do fork](docs/superficie-configuracao-fork.md)
- [Fase 2 — métricas da camada de apresentação](docs/fase2-presentation-metrics.md)
- [Fase 3 — capacidades da GPU e gating do Turnip](docs/fase3-gpu-capabilities.md)
- [Fase 4 — endurecer o gerenciador de driver Vulkan](docs/fase4-driver-manager.md)
- [Lista de jogos-canário e protocolo de teste](docs/jogos-canario.md)

## Licença

GPL-3.0, herdada de PCSX2/ARMSX2. Todo o código deste fork é publicado integralmente.
Componentes de terceiros mantêm suas licenças originais (ex.: `libadrenotools`, BSD-2-Clause,
vendored em `platforms/android/app/src/main/cpp/3rdparty/adrenotools`).
