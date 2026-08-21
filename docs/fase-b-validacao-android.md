# Fase B — validação Android em hardware real

**Filosofia:** compatibility first. Performance measured. Features optional.

**Baseline:** `cdf90f34d8ad8a190fad1bb706117edbdbecfb55`, ponta publicada da Fase A.

Esta fase não muda o comportamento do renderer. Ela cria o circuito que precisa provar que a
compilação assíncrona é segura antes que alguém sequer discuta mudar seu default.

## 1. Evidência do problema

`ForkBenchmark` já mede FPS real e apresentado separadamente, frametime p95/p99, 1% low,
engasgos e compilação de shaders. Porém `docs/fase6-benchmark.md` registra que o protocolo ainda é
executado por uma pessoa. Isso deixa quatro fontes de viés fora do alcance do código:

- warm-up e janela medida podem começar em momentos diferentes;
- menos de cinco repetições ou ordem sempre A→B favorecem o segundo braço;
- temperatura, clocks, driver efetivamente carregado e configuração podem mudar sem invalidar o
  resultado;
- screenshots soltas não formam um gate visual reproduzível.

O diagnóstico Vulkan também não exporta dois campos que a Fase B exige: `driverID` e
`pipelineCacheUUID`. Ambos já existem em `GSDeviceVK`; faltava carregá-los até o relatório.

## 2. Desenho

### 2.1 Controle

O host executa `tools/fork/android-validation.py` e conversa com um único aparelho por `adb`.
O APK expõe um `BroadcastReceiver` Android-only protegido por `android.permission.DUMP`: somente o
shell do `adb`/sistema pode usá-lo. O receiver não abre socket, não aceita arquivos e não fica no
caminho por frame.

Comandos permitidos são uma lista fechada:

- consultar ambiente, estado do driver e resultados do benchmark;
- aplicar opções `[Fork]` declaradas pelo cenário;
- carregar um savestate numerado;
- pausar, retomar e parar a VM.

O jogo-canário ou GS dump é iniciado pelo `Intent.ACTION_VIEW` já suportado pelo frontend. BIOS,
jogos, savestates, dumps e imagens de referência permanecem fora do repositório.

### 2.2 Cenário

Cada cenário é JSON versionado pelo testador e contém URI local, slot opcional, warm-up, duração,
descanso, cinco ou mais repetições por braço, resolução esperada, limite térmico e exatamente dois
braços. As diferenças de configuração entre A e B são explícitas; para a Fase A, o normal é:

```json
{
  "schema_version": 1,
  "scenario_id": "sotc-pipeline-compiler",
  "package": "com.armsx2",
  "launch_uri": "file:///storage/emulated/0/PS2/SCUS-97472.iso",
  "save_slot": 0,
  "warmup_seconds": 120,
  "measure_seconds": 60,
  "cooldown_seconds": 300,
  "repetitions": 5,
  "max_temperature_delta_c": 3.0,
  "arms": [
    {"id": "sync", "settings": {"PipelineCompiler.Mode": "off"}},
    {"id": "async", "settings": {
      "PipelineCompiler.Mode": "experimental",
      "PipelineCompiler.Workers": 1
    }}
  ]
}
```

O runner alterna pares AB/BA. Warm-up nunca entra no `benchmark.begin`; a medição começa depois do
warm-up e termina antes da captura ambiental final.

### 2.3 Evidência produzida

Cada execução grava dados brutos, sem apagar valores inconvenientes:

- fabricante/modelo, SoC, GPU, Android, kernel e page size;
- `driverID`, `driverName`, `driverInfo`, versão Mesa/Qualcomm, Vulkan, SHA-256 do pacote e UUID do
  pipeline cache;
- temperatura e clocks disponíveis antes/depois; ausência de permissão é registrada como
  `unavailable`, nunca convertida em zero;
- resolução, versão do APK e SHA-256 da configuração resolvida;
- FPS real e apresentado em campos separados, p95, p99, 1% low, stutters, erros de present e
  compilação de shaders;
- screenshot PNG, hash perceptual e distância para a referência, quando configurada;
- logcat bruto, incluindo `@@FORK_PIPELINE_ASYNC@@`.

São gerados `manifest.json`, `runs.json`, `summary.csv` e `report.md` em um diretório novo por
sessão. Arquivos existentes nunca são sobrescritos.

### 2.4 Gates

`publishable=false` é obrigatório quando ocorrer qualquer um destes casos:

- menos de cinco execuções válidas por braço;
- driver solicitado diferente do efetivamente carregado;
- `driverID`, UUID do cache, resolução ou configuração inesperados;
- compilação de shader durante a janela medida;
- diferença térmica entre braços acima do limite ou throttling evidente pelos clocks;
- erro de present, crash, timeout ou hash visual além da tolerância;
- duração da janela fora da tolerância.

O relatório pode mostrar números brutos de uma sessão inválida, mas não imprime vencedor. FPS
apresentado por frame generation nunca participa do veredito de desempenho da emulação.

## 3. Matriz de hardware

| Alvo | Estado | Uso nesta fase |
|---|---|---|
| AYN Odin 2 Portal, Snapdragon 8 Gen 2 / Adreno 740 | disponível ao projeto | Qualcomm do sistema × Turnip, A/B da Fase A |
| Anbernic RG557, Dimensity 8300 / Mali-G615 | disponível ao projeto | gate Mali e lifecycle, sem Turnip |
| Android intermediário independente | pendente | obrigatório antes de conclusão geral |

Ter o runner não equivale a executar a matriz. Nenhuma conclusão de desempenho será publicada até
que os bundles dos aparelhos reais atendam aos gates acima.

## 4. Limites deliberados

- O runner não distribui conteúdo protegido e não procura jogos automaticamente.
- Entrada automatizada complexa ainda não é prometida. Jogos devem usar savestate e trecho
  autônomo; GS dumps são o caminho determinístico preferido para regressão gráfica.
- pHash detecta diferença, não prova correção. Uma diferença exige inspeção da imagem e reprodução.
- Nenhum default muda nesta fase.
