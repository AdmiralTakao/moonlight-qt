# GFN fork: compiles the Direct3D 11 post-processing shaders with fxc.exe
# from the Windows SDK. Run from any directory; exits non-zero on failure.

$ErrorActionPreference = 'Stop'

$fxc = Get-Command fxc.exe -ErrorAction SilentlyContinue
if ($fxc) {
    $fxcPath = $fxc.Source
}
else {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $candidate = Get-ChildItem -Path $kits -Recurse -Filter fxc.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\fxc\.exe$' } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if (-not $candidate) {
        Write-Error 'fxc.exe not found. Install the Windows SDK.'
    }
    $fxcPath = $candidate.FullName
}

Write-Host "Using $fxcPath"

Push-Location $PSScriptRoot
try {
    $shaders = @(
        'd3d11_yuv420_cfl_pixel',
        'd3d11_fsr_easu_pixel',
        'd3d11_fsr_rcas_pixel'
    )

    foreach ($shader in $shaders) {
        & $fxcPath /nologo /T ps_5_0 /O3 /Fo "$shader.fxc" "$shader.hlsl"
        if ($LASTEXITCODE -ne 0) {
            throw "fxc failed for $shader.hlsl"
        }
    }
}
finally {
    Pop-Location
}
