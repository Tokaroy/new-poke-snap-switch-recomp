# Runs a boot route on a software Vulkan driver (Mesa lavapipe) with the validation layer, to catch code that only works on lenient GPUs.
# docs/driver-portability.md. lavapipe is selected for this process only (VK_ICD_FILENAMES / VK_DRIVER_FILES); nothing is installed or registered.
#   powershell -File host\run_lavapipe.ps1 -MesaDir <folder with lvp_icd.x86_64.json> [-Route lvp_menu] [-BuildDir build\snap] [-Scale 0.25] [-Timeout 590]
# -MesaDir '' runs the same settings on the normal GPU (the comparison run). -Aa default keeps the game's default anti-aliasing and render size.
param([string]$MesaDir = $env:SNAP_MESA_DIR, [string]$Route = 'lvp_menu', [string]$BuildDir = 'build\snap', [double]$Scale = 0.25, [int]$Timeout = 590,
      [int]$ShotAt = 10, [int]$ShotEvery = 5, [string]$Aa = 'off', [string]$Validation = '1')
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root
if ((Test-Path 'build\play.lock') -and $env:SNAP_RUN_WHILE_PLAYING -ne '1') { Write-Host 'play.lock held: not starting a run'; exit 3 }
$env:NVN_VK_VALIDATION = $Validation
$env:VK_LAYER_PATH = Join-Path $root 'tools\vulkan_sdk\Bin'
if ($Aa -ne 'default') { $env:SNAP_AA = $Aa; $env:SNAP_RENDER_SCALE = "$Scale" }
$env:SNAP_WINDOW_W = '640'; $env:SNAP_WINDOW_H = '360'
if ($MesaDir) {
    $icd = Join-Path $MesaDir 'lvp_icd.x86_64.json'
    if (-not (Test-Path $icd)) { Write-Host "no $icd"; exit 2 }
    $env:VK_ICD_FILENAMES = $icd; $env:VK_DRIVER_FILES = $icd
}
powershell -NoProfile -File (Join-Path $PSScriptRoot 'run_boot.ps1') -Route $Route -BuildDir $BuildDir -Timeout $Timeout -ShotAt $ShotAt -ShotEvery $ShotEvery -ShotMax 20
exit $LASTEXITCODE
