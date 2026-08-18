<#
.SYNOPSIS
    Captura de log de sessao para analise do fork, no Windows.

.DESCRIPTION
    Roda na SUA maquina, com o Odin ligado por USB. Produz dois arquivos: o log cru e um resumo
    filtrado, pronto para colar.

    Por que um script e nao "roda adb logcat":

    O Console do PCSX2 escreve em stdout, e o native-lib redireciona stdout/stderr para o logcat
    sob a tag `STDOUT` (ver redirect_stdout_to_logcat). Ou seja, "ForkDriverIdentity:" e o TEXTO
    da mensagem, nao a tag - um `adb logcat -s ForkDriverIdentity` volta VAZIO e da a impressao
    de que o recurso nao existe. E nao ha emulog.txt no Android: o app nunca chama
    SetFileOutputLevel, entao o logcat e o unico canal.

    Captura por TEMPO em vez de esperar Ctrl-C: no PowerShell o Ctrl-C interrompe o pipeline e o
    bloco de limpeza nem sempre roda, o que deixaria o resumo sem ser gerado justo no fim.

.PARAMETER Seconds
    Duracao da captura. Padrao 120.

.PARAMETER Output
    Prefixo dos arquivos. Padrao: armsx2-log-<data>.

.EXAMPLE
    .\tools\fork\capture-logs.ps1
    .\tools\fork\capture-logs.ps1 -Seconds 180
#>
param(
    [int]$Seconds = 120,
    [string]$Output = ""
)

$ErrorActionPreference = 'Stop'

if (-not (Get-Command adb -ErrorAction SilentlyContinue)) {
    Write-Error "adb nao encontrado no PATH. Instale o Android Platform Tools."
    exit 1
}

# Um aparelho, e so um: com dois conectados o adb escolhe sozinho e a captura sai do errado.
$devices = @(adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice$' })
if ($devices.Count -eq 0) {
    Write-Host "Nenhum aparelho autorizado. Ligue a depuracao USB e aceite o pedido na tela do aparelho." -ForegroundColor Red
    adb devices
    exit 1
}
if ($devices.Count -gt 1) {
    Write-Host "Mais de um aparelho conectado; desconecte os outros." -ForegroundColor Red
    adb devices
    exit 1
}

if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = "armsx2-log-{0}" -f (Get-Date -Format 'yyyyMMdd-HHmmss')
}
$raw    = "$Output.raw.log"
$digest = "$Output.digest.log"

function Prop([string]$name) { (adb shell getprop $name) -replace "`r", "" }

# Cabecalho de identidade. Sem isto uma medicao nao e comparavel com outra: driver compilado pelo
# proprio usuario nao tem rotulo reproduzivel, e "alpha 1" nao distingue dois APKs.
$header = @()
$header += "# captura: {0}" -f (Get-Date -Format 'o')
$header += "# aparelho : {0} {1}" -f (Prop 'ro.product.manufacturer'), (Prop 'ro.product.model')
$header += "# android  : {0} (SDK {1})" -f (Prop 'ro.build.version.release'), (Prop 'ro.build.version.sdk')
$header += "# soc      : {0} / {1}" -f (Prop 'ro.soc.model'), (Prop 'ro.board.platform')

$packages = @(adb shell pm list packages | ForEach-Object { ($_ -replace "`r", "") -replace '^package:', '' } |
              Where-Object { $_ -match 'armsx2|pcsx2' })
foreach ($pkg in $packages) {
    $ver = (adb shell dumpsys package $pkg | Select-String -Pattern 'versionName=' | Select-Object -First 1).ToString().Trim()
    $header += "# apk      : {0} {1}" -f $pkg, $ver
}

$header | Set-Content -Path $digest -Encoding utf8
$header | Set-Content -Path $raw    -Encoding utf8
$header | Write-Host

# Buffer grande e limpo: o padrao e pequeno demais para uma sessao de jogo, e o comeco - que e
# onde o driver e escolhido - seria o primeiro a ser descartado.
adb logcat -G 16M *> $null
adb logcat -c

Write-Host ""
Write-Host "Capturando por $Seconds s. ABRA O JOGO AGORA." -ForegroundColor Green
Write-Host "Sugestao: ~1 min com Frame Generation DESLIGADA, depois ligue e repita o mesmo trecho."
Write-Host ""

$proc = Start-Process -FilePath 'adb' -ArgumentList 'logcat', '-v', 'time' `
    -RedirectStandardOutput "$raw.tmp" -NoNewWindow -PassThru

try {
    for ($i = $Seconds; $i -gt 0; $i--) {
        Write-Host -NoNewline ("`r  restam {0,4} s " -f $i)
        Start-Sleep -Seconds 1
    }
} finally {
    Write-Host ""
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    Start-Sleep -Milliseconds 300
    Get-Content "$raw.tmp" -ErrorAction SilentlyContinue | Add-Content -Path $raw -Encoding utf8
    Remove-Item "$raw.tmp" -ErrorAction SilentlyContinue

    # Filtro no TEXTO da mensagem, nao na tag. Veja o comentario no topo.
    $pattern = '@@FORK@@|ForkDriverIdentity|ForkGpuCapabilities|ForkRuntime:|ForkConfig:|@@ANDROID_LSFG@@|adrenotools|libvulkan|[Tt]urnip|VK_ERROR|FATAL EXCEPTION|beginning of crash'
    $hits = @(Select-String -Path $raw -Pattern $pattern -ErrorAction SilentlyContinue | ForEach-Object { $_.Line })
    if ($hits.Count -gt 0) { $hits | Add-Content -Path $digest -Encoding utf8 }

    Write-Host ""
    Write-Host "  log cru : $raw"
    Write-Host "  resumo  : $digest   ($($hits.Count) linhas de interesse)"
    Write-Host ""
    if ($hits.Count -eq 0) {
        Write-Host "Nenhuma linha de interesse. Mande o log CRU - que o diagnostico nao apareca" -ForegroundColor Yellow
        Write-Host "ja e, por si so, o achado." -ForegroundColor Yellow
    } else {
        Write-Host "Mande o RESUMO."
    }
}
