param(
    [string] $Tools = (Join-Path $PSScriptRoot '../build/windows-deps'),
    [string] $HostClang = 'clang++'
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Tools = [IO.Path]::GetFullPath($Tools)
New-Item -ItemType Directory -Path $Tools -Force | Out-Null

function Get-ToolArchive([string] $Name, [string] $Archive, [string] $Url, [string] $Hash) {
    $directory = Join-Path $Tools $Name
    if (Test-Path -LiteralPath $directory -PathType Container) { return $directory }
    $download = Join-Path $Tools $Archive
    if (-not (Test-Path -LiteralPath $download -PathType Leaf)) {
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $download
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $download).Hash -ne $Hash) {
        throw "Unexpected SHA-256 for $download"
    }
    tar -xf $download -C $Tools
    return (Resolve-Path -LiteralPath $directory).Path
}

$llvmName = 'clang+llvm-18.1.8-x86_64-pc-windows-msvc'
$mingwName = 'llvm-mingw-20261117-ucrt-x86_64'
$llvm = Get-ToolArchive $llvmName "$llvmName.tar.xz" "https://github.com/llvm/llvm-project/releases/download/llvmorg-18.1.8/$llvmName.tar.xz" `
    '22C5907DB053026CC2A8FF96D21C0F642A90D24D66C23C6D28EE7B1D572B82E8'
$mingw = Get-ToolArchive $mingwName "$mingwName.zip" "https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/$mingwName.zip" `
    '4D002E330A0F703625AA5CCBC37E96F8F824413E163BFC7336774AABD2C3A412'

if (-not $env:VCToolsRedistDir) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $visualStudio) { throw 'Could not locate Visual Studio C++ Build Tools' }
    & (Join-Path $visualStudio 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
}
if (-not $env:LYRA_TEST_OPENSSL) {
    $gitRoot = Split-Path (Split-Path (Get-Command git).Source)
    $openssl = Join-Path $gitRoot 'usr/bin/openssl.exe'
    if (Test-Path -LiteralPath $openssl -PathType Leaf) { $env:LYRA_TEST_OPENSSL = $openssl }
    else { $env:LYRA_TEST_OPENSSL = (Get-Command openssl -ErrorAction Stop).Source }
}

$vcpkg = Join-Path $Tools 'vcpkg'
$baseline = (Get-Content -Raw -LiteralPath (Join-Path $root 'vcpkg.json') | ConvertFrom-Json).'builtin-baseline'
if (-not (Test-Path -LiteralPath (Join-Path $vcpkg '.git'))) {
    git clone --filter=blob:none https://github.com/microsoft/vcpkg.git $vcpkg
    git -C $vcpkg checkout --detach $baseline
}
if (-not (Test-Path -LiteralPath (Join-Path $vcpkg 'vcpkg.exe'))) {
    & (Join-Path $vcpkg 'bootstrap-vcpkg.bat') -disableMetrics
}

Push-Location $root
try {
    $prefix = Join-Path $Tools 'installed'
    $env:VCPKG_DISABLE_METRICS = '1'
    $originalPath = $env:Path
    try {
        $env:Path = "$mingw/bin;$originalPath"
        & (Join-Path $vcpkg 'vcpkg.exe') install --triplet x64-mingw-static --host-triplet x64-windows "--x-install-root=$prefix"
    } finally { $env:Path = $originalPath }
    $hostCxx = (Get-Command $HostClang -ErrorAction Stop).Source
    $hostC = Join-Path (Split-Path $hostCxx) 'clang.exe'
    $env:PYTHONUTF8 = '1'
    foreach ($preset in 'windows-dev', 'windows-release') {
        cmake --preset $preset "-DCMAKE_CXX_COMPILER=$hostCxx" "-DCMAKE_C_COMPILER=$hostC" "-DLLVM_DIR=$llvm/lib/cmake/llvm" `
            "-DLYRA_WINDOWS_SYSROOT=$mingw" "-DLYRA_WINDOWS_TARGET_PORTS=$prefix/x64-mingw-static"
        cmake --build --preset $preset --parallel 4
        ctest --preset $preset --no-tests=error
    }
} finally { Pop-Location }
