<#
    .SYNOPSIS
    Compiles the Java contract, builds the JNI shim as a shared library, and
    runs both the Java checks and the C probe against a live JVM.

    .DESCRIPTION
    The gate xgit runs (Test-XmipSelfVerifyingModule). Needs the `java` and
    `c` prerequisites: the JDK for javac, jni.h and the JVM; zig cc for the
    shim. The shim loads the JVM at run time, so nothing links against the JDK.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

function Find-JavaHome {
    if ($env:JAVA_HOME -and (Test-Path -LiteralPath $env:JAVA_HOME)) { return $env:JAVA_HOME }
    $javac = Get-Command javac -ErrorAction SilentlyContinue
    if ($javac) { return (Split-Path (Split-Path $javac.Source)) }
    foreach ($root in @('C:\Program Files\Microsoft', 'C:\Program Files\Eclipse Adoptium', '/usr/lib/jvm', '/opt/homebrew/opt')) {
        $found = Get-ChildItem -Path $root -Directory -Filter 'jdk*' -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    return $null
}

[string] $javaHome = Find-JavaHome
if (-not $javaHome) { Write-Host 'FAILED. No JDK; prerequisite.toml declares java.'; exit 2 }
if (-not (Get-Command zig -ErrorAction SilentlyContinue)) { Write-Host 'FAILED. zig is not installed (prerequisite c).'; exit 2 }
[string] $include = $env:XMIP_ABI_INCLUDE
if (-not $include) { $include = Join-Path $PSScriptRoot '..' '..' '..' 'foundation' 'abi' 'include' }
if (-not (Test-Path -LiteralPath (Join-Path $include 'xmip_module.h'))) {
    Write-Host "FAILED. xmip_module.h not found under $include; set XMIP_ABI_INCLUDE."
    exit 2
}

[string] $bin = Join-Path $javaHome 'bin'
[string] $javac = Join-Path $bin 'javac'
[string] $java = Join-Path $bin 'java'
[string] $jar = Join-Path $bin 'jar'
[string] $jniPlatform = if ($IsWindows) { 'win32' } elseif ($IsMacOS) { 'darwin' } else { 'linux' }
[string] $jniInclude = Join-Path $javaHome 'include'
New-Item -ItemType Directory -Force -Path build/classes | Out-Null

Write-Host "   javac -> build/classes  (JDK at $javaHome)"
& $javac -d build/classes (Get-ChildItem java -Recurse -Filter '*.java').FullName tests/ContractTest.java
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host '   java ContractTest'
& $java -cp build/classes ContractTest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host '   jar -> build/xmip-core-contract-java.jar'
& $jar --create --file build/xmip-core-contract-java.jar -C build/classes xmip
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

[string] $library = if ($IsWindows) { 'xmip_core_contract_java.dll' }
    elseif ($IsMacOS) { 'libxmip_core_contract_java.dylib' } else { 'libxmip_core_contract_java.so' }
[string] $probe = if ($IsWindows) { 'probe.exe' } else { 'probe' }

Write-Host "   zig cc -shared -> build/$library"
& zig cc -shared -O2 -fvisibility=hidden -Wall -Wextra -Werror -I $include -I $jniInclude -I (Join-Path $jniInclude $jniPlatform) `
    shim/xmip_java_shim.c -o (Join-Path build $library)
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "   zig cc -> build/$probe"
& zig cc -O1 -Wall -Wextra -Werror -I $include -I $jniInclude -I (Join-Path $jniInclude $jniPlatform) `
    shim/xmip_java_shim.c tests/probe.c -o (Join-Path build $probe)
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$env:JAVA_HOME = $javaHome
$env:XMIP_JAVA_CLASSPATH = (Resolve-Path build/xmip-core-contract-java.jar).Path
if ($IsWindows) { $env:Path = "$(Join-Path $javaHome 'bin' 'server');$(Join-Path $javaHome 'bin');$env:Path" }
& (Join-Path $PSScriptRoot 'build' $probe)
exit $LASTEXITCODE
