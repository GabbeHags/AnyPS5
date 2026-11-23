param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string[]]$Items
)
$ErrorActionPreference = 'Stop'
$Items = @($Items | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().Trim('"') } | Where-Object { $_ })
$root = $PSScriptRoot
$keep = Join-Path $root "_reset-keep"
if (Test-Path $keep) {
    Write-Host "$keep already exists: a previous test did not restore. Move its contents back by hand first."
    Read-Host 'Press Enter to exit'
    exit 1
}
New-Item -ItemType Directory -Path $keep | Out-Null
$moved = @()
try {
    foreach ($item in $Items) {
        $source = Join-Path $root $item
        if (-not (Test-Path $source)) { continue }
        $target = Join-Path $keep $item
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        Move-Item -LiteralPath $source -Destination $target
        $moved += $item
        Write-Host "set aside: $item"
    }
    $logs = Join-Path $root 'logs'
    if (-not (Test-Path $logs)) { New-Item -ItemType Directory -Path $logs | Out-Null }
    $env:APS5_NET_DISCONNECTED = '1'
    $env:APS5_LOG_TIMESTAMPS = '1'
    $env:ANYPS5_LOG_FILE = Join-Path $logs ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-test-$Name.log")
    Push-Location $root
    try {
        & (Join-Path $root 'Mafia Definitive Edition.exe') | Out-Host
    } finally {
        Pop-Location
    }
} finally {
    foreach ($item in $Items) {
        $kept = Join-Path $keep $item
        if (-not (Test-Path $kept)) { continue }
        $current = Join-Path $root $item
        if (Test-Path $current) { Remove-Item -LiteralPath $current -Recurse -Force }
        New-Item -ItemType Directory -Force -Path (Split-Path $current) | Out-Null
        Move-Item -LiteralPath $kept -Destination $current
        Write-Host "restored: $item"
    }
    Remove-Item -LiteralPath $keep -Recurse -Force
}
