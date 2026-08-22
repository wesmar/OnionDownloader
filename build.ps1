<#
.SYNOPSIS
    Deterministic build of Onion Downloader (od_x64.exe / od_arm64.exe).

.DESCRIPTION
    Locates the newest installed Visual Studio and builds Release with MSVC v145
    (/std:c++latest, static /MT runtime). By default it builds BOTH native
    targets and leaves them side by side in bin\ - od_x64.exe and od_arm64.exe.
    Pass -Platform to build a single architecture.

    Each binary links only against Windows system libraries - no third-party DLL
    or C++ redistributable. The matching native Tor engine is packed as an
    LZX-21 cabinet into the per-architecture icon (resources\app.ico for x64,
    resources\app-arm64.ico for ARM64) only when its content (or the base icon)
    changes; unchanged payloads are reused. Because the payload already lives in
    the checked-in icon, a plain Windows + MSVC clone builds either architecture
    without WSL - the loose tor.exe is only needed to regenerate the cabinet via
    build-tor.sh / build-tor-arm64.sh.

.PARAMETER Configuration
    Release (default) or Debug.

.PARAMETER Platform
    All (default) builds x64 and ARM64. Pass x64 or ARM64 to build just one.
    Selects the target architecture, the embedded native Tor payload, and the
    output name (od_x64.exe / od_arm64.exe).

.PARAMETER Clean
    Remove intermediate directories before building.

.PARAMETER RepackTor
    Force regeneration of the embedded LZX-21 cabinet even when source hashes
    match the existing payload stamp.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('All', 'x64', 'ARM64')]
    [string]$Platform = 'All',

    [switch]$Clean,

    [switch]$RepackTor
)

$ErrorActionPreference = 'Stop'

$projectName  = 'od'
$root         = $PSScriptRoot
$binDir       = Join-Path $root 'bin'
$projectFile  = Join-Path $root "$projectName.vcxproj"
$resourcesDir = Join-Path $root 'resources'
$baseIcon     = Join-Path $resourcesDir 'app_orig.ico'

# Deterministic file stamp, matching the sibling projects in C:\Projekty.
$deterministicDate = Get-Date '2030-01-01 00:00:00'

function Write-Step([string]$Text) {
    Write-Host "--- $Text" -ForegroundColor Cyan
}

function Remove-BuildDirectory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    for ($attempt = 1; $attempt -le 5; ++$attempt) {
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
            return
        }
        catch {
            if ($attempt -eq 5) { throw }
            Start-Sleep -Milliseconds 250
        }
    }
}

# Per-architecture names and paths. Each target is suffixed with its
# architecture (od_x64.exe / od_arm64.exe) so both live side by side in bin\.
# Each architecture embeds its OWN native Tor payload, packed into its own icon
# (app.ico / app-arm64.ico) that resources\od.rc selects through the OD_ARM64
# preprocessor define.
function Get-PlatformContext([string]$Plat) {
    if ($Plat -eq 'ARM64') {
        [pscustomobject]@{
            Platform     = 'ARM64'
            OutputName   = "${projectName}_arm64"
            TorSource    = Join-Path $resourcesDir 'tor-arm64.exe'
            PayloadIcon  = Join-Path $resourcesDir 'app-arm64.ico'
            PayloadStamp = Join-Path $resourcesDir 'app-arm64.ico.payload.sha256'
            RecipeScript = 'build-tor-arm64.sh'
        }
    }
    else {
        [pscustomobject]@{
            Platform     = 'x64'
            OutputName   = "${projectName}_x64"
            TorSource    = Join-Path $resourcesDir 'tor.exe'
            PayloadIcon  = Join-Path $resourcesDir 'app.ico'
            PayloadStamp = Join-Path $resourcesDir 'app.ico.payload.sha256'
            RecipeScript = 'build-tor.sh'
        }
    }
}

function Test-EmbeddedCab([string]$IconPath, [string]$BaseIconPath) {
    if (-not (Test-Path -LiteralPath $IconPath) -or
        -not (Test-Path -LiteralPath $BaseIconPath)) {
        return $false
    }

    $iconLength = (Get-Item -LiteralPath $IconPath).Length
    $baseLength = (Get-Item -LiteralPath $BaseIconPath).Length
    if ($iconLength -lt ($baseLength + 16)) {
        return $false
    }

    $stream = [System.IO.File]::OpenRead($IconPath)
    try {
        [void]$stream.Seek($baseLength, [System.IO.SeekOrigin]::Begin)
        $header = [byte[]]::new(12)
        if ($stream.Read($header, 0, $header.Length) -ne $header.Length) {
            return $false
        }
        $signature = [System.Text.Encoding]::ASCII.GetString($header, 0, 4)
        $reserved = [System.BitConverter]::ToUInt32($header, 4)
        $cabSize = [System.BitConverter]::ToUInt32($header, 8)
        return $signature -eq 'MSCF' -and $reserved -eq 0 -and
               $cabSize -eq ($iconLength - $baseLength)
    }
    finally {
        $stream.Dispose()
    }
}

function Update-EmbeddedTor($Ctx, [bool]$Force) {
    $torSource    = $Ctx.TorSource
    $payloadIcon  = $Ctx.PayloadIcon
    $payloadStamp = $Ctx.PayloadStamp
    $recipeScript = $Ctx.RecipeScript
    $torName      = Split-Path $torSource  -Leaf
    $iconName     = Split-Path $payloadIcon -Leaf

    if (-not (Test-Path -LiteralPath $baseIcon)) {
        throw "Base application icon not found: $baseIcon"
    }

    # The loose tor.exe / tor-arm64.exe is intentionally not tracked in the
    # repository: the LZX-21 payload already lives inside the checked-in icon
    # (app.ico for x64, app-arm64.ico for ARM64). An ordinary clone therefore
    # builds directly from that icon on plain Windows + MSVC, and only the WSL
    # recipe reintroduces the loose engine when it is rebuilt from source.
    if (-not (Test-Path -LiteralPath $torSource)) {
        if ($Force) {
            throw "RepackTor was requested but $torName is absent. Rebuild the engine with $recipeScript first."
        }
        if (Test-EmbeddedCab $payloadIcon $baseIcon) {
            Write-Host "    $torName absent; reusing embedded payload in $iconName" -ForegroundColor Gray
            return
        }
        throw "No Tor payload available: $torName is missing and $iconName has no valid embedded cabinet. Run $recipeScript to produce the engine."
    }

    $torItem = Get-Item -LiteralPath $torSource
    $maxTorSize = 8MB
    if ($torItem.Length -gt $maxTorSize) {
        throw "$torName is $($torItem.Length) bytes; refusing to bundle more than $maxTorSize bytes. Use the pruned client build."
    }

    $torHash = (Get-FileHash -LiteralPath $torSource -Algorithm SHA256).Hash
    $iconHash = (Get-FileHash -LiteralPath $baseIcon -Algorithm SHA256).Hash
    $stamp = "format=od-cab-v1`ncompression=LZX-21`ntor=$torHash`nicon=$iconHash`n"
    $storedStamp = if (Test-Path -LiteralPath $payloadStamp) {
        [System.IO.File]::ReadAllText($payloadStamp)
    } else {
        ''
    }

    if (-not $Force -and $storedStamp -ceq $stamp -and
        (Test-EmbeddedCab $payloadIcon $baseIcon)) {
        Write-Host "    Embedded Tor unchanged; reusing $iconName" -ForegroundColor Gray
        return
    }

    $token = [Guid]::NewGuid().ToString('N')
    $cabTemp = Join-Path ([System.IO.Path]::GetTempPath()) "od-$token.cab"
    $iconTemp = Join-Path ([System.IO.Path]::GetTempPath()) "od-$token.ico"
    $stampTemp = Join-Path ([System.IO.Path]::GetTempPath()) "od-$token.sha256"
    try {
        & makecab.exe /D CompressionType=LZX /D CompressionMemory=21 `
            /D Cabinet=ON /D Compress=ON $torSource $cabTemp | Out-Null
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $cabTemp)) {
            throw 'makecab.exe failed to create the embedded Tor payload.'
        }

        $output = [System.IO.File]::Create($iconTemp)
        try {
            $input = [System.IO.File]::OpenRead($baseIcon)
            try { $input.CopyTo($output) } finally { $input.Dispose() }
            $input = [System.IO.File]::OpenRead($cabTemp)
            try { $input.CopyTo($output) } finally { $input.Dispose() }
        }
        finally {
            $output.Dispose()
        }

        if (-not (Test-EmbeddedCab $iconTemp $baseIcon)) {
            throw "Generated $iconName failed embedded CAB validation."
        }

        [System.IO.File]::WriteAllText(
            $stampTemp, $stamp, [System.Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $iconTemp -Destination $payloadIcon -Force
        Move-Item -LiteralPath $stampTemp -Destination $payloadStamp -Force
        $cabLength = (Get-Item -LiteralPath $cabTemp).Length
        Write-Host ("    Packed $torName -> $iconName ({0:N0} bytes, LZX-21)" -f $cabLength) -ForegroundColor Green
    }
    finally {
        Remove-Item -LiteralPath $cabTemp, $iconTemp, $stampTemp -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-BuildPlatform($Ctx) {
    $plat       = $Ctx.Platform
    $outputName = $Ctx.OutputName

    Write-Step "[$plat] Preparing embedded Tor payload"
    Update-EmbeddedTor $Ctx $RepackTor.IsPresent

    Write-Step "[$plat] Preparing output"
    Get-Process $outputName -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 200
    Remove-Item (Join-Path $binDir "$outputName.exe") -Force -ErrorAction SilentlyContinue

    Write-Step "[$plat] Building $Configuration|$plat"
    $msBuildArgs = @(
        $projectFile
        '/t:Rebuild'
        "/p:Configuration=$Configuration"
        "/p:Platform=$plat"
        '/p:DebugSymbols=false'
        '/p:DebugType=none'
        '/m'
        '/nologo'
        '/v:m'
    )
    & $msbuild $msBuildArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "!!! BUILD FAILED ($plat) !!!" -ForegroundColor Red
        exit $LASTEXITCODE
    }

    Write-Step "[$plat] Finalizing binary"
    $builtExe = Join-Path $root "$plat\$Configuration\$outputName.exe"
    if (-not (Test-Path $builtExe)) {
        throw "Build reported success but $builtExe is missing."
    }
    $targetExe = Join-Path $binDir "$outputName.exe"
    Move-Item -Path $builtExe -Destination $targetExe -Force

    foreach ($intermediate in @($plat, 'obj')) {
        Remove-BuildDirectory (Join-Path $root $intermediate)
    }

    $item = Get-Item $targetExe
    $item.CreationTime   = $deterministicDate
    $item.LastWriteTime  = $deterministicDate
    $item.LastAccessTime = $deterministicDate

    $hash = (Get-FileHash $targetExe -Algorithm SHA256).Hash
    Write-Host ("    {0,-16} {1,10:N0} bytes  ({2})" -f "$outputName.exe", $item.Length, $plat) -ForegroundColor Green
    Write-Host "    SHA-256          $hash" -ForegroundColor DarkGray
}

# ---------------------------------------------------------------- toolchain --
Write-Step 'Locating Visual Studio'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 or newer with the C++ workload."
}

$vsPath = & $vswhere -latest -prerelease -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsPath) {
    throw 'No Visual Studio installation with MSBuild was found.'
}

$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) {
    throw "MSBuild.exe not found under $vsPath."
}

$vsVersion = & $vswhere -latest -prerelease -property catalog_productDisplayVersion
Write-Host "    Visual Studio $vsVersion" -ForegroundColor Gray
Write-Host "    $msbuild" -ForegroundColor DarkGray

# ------------------------------------------------------------------ prepare --
Write-Step 'Preparing output directory'

Get-Process 'tor' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 200

if (-not (Test-Path $binDir)) {
    New-Item -ItemType Directory -Path $binDir -Force | Out-Null
}

# A stale loose Tor would override the embedded payload at runtime; keep the
# canonical input only in resources\.
Remove-Item (Join-Path $binDir 'tor.exe') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $binDir '*.pdb')   -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $binDir '*.ilk')   -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $binDir '*.log')   -Force -ErrorAction SilentlyContinue

if ($Clean) {
    foreach ($stale in @('x64', 'ARM64', 'obj', '.vs')) {
        Remove-BuildDirectory (Join-Path $root $stale)
    }
}

# -------------------------------------------------------------------- build --
$platforms = if ($Platform -eq 'All') { @('x64', 'ARM64') } else { @($Platform) }
Write-Host "    Targets: $($platforms -join ', ')" -ForegroundColor Gray

foreach ($p in $platforms) {
    Invoke-BuildPlatform (Get-PlatformContext $p)
}

# ------------------------------------------------------------------- report --
Write-Step 'Result'
foreach ($p in $platforms) {
    $name = "${projectName}_$($p.ToLower()).exe"
    $path = Join-Path $binDir $name
    if (Test-Path $path) {
        Write-Host ("    {0,-16} {1,10:N0} bytes" -f $name, (Get-Item $path).Length) -ForegroundColor Green
    }
}
Write-Host "    Timestamp        $deterministicDate (deterministic, /Brepro + /BREPRO)" -ForegroundColor DarkGray
Write-Host "Done: $binDir" -ForegroundColor Green
