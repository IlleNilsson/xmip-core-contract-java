<#
    .SYNOPSIS
    Compiles the Java contract, builds the JNI shim as a shared library, and
    runs both the Java checks and the C probe against a live JVM.

    .DESCRIPTION
    The gate xgit runs (Test-XmipSelfVerifyingModule). Needs the `java` and
    `c` prerequisites: the JDK for javac, jni.h and the JVM; zig cc for the
    shim. The shim loads the JVM at run time, so nothing links against the
    JDK. The probe and the shim's build are the capability's, shared by every
    language technology (ADR-0044): probe/verify.ps1 beside this repository's
    mount in the estate, or where XMIP_CONTRACT_PROBE points.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

function Find-JavaHome {
    [CmdletBinding()]
    [OutputType([string])]
    param()

    if ($env:JAVA_HOME -and (Test-Path -LiteralPath $env:JAVA_HOME)) {
        return $env:JAVA_HOME
    }
    $javac = Get-Command javac -ErrorAction SilentlyContinue
    if ($javac) {
        return (Split-Path (Split-Path $javac.Source))
    }
    [string[]] $roots = @(
        'C:\Program Files\Microsoft',
        'C:\Program Files\Eclipse Adoptium',
        '/usr/lib/jvm',
        '/opt/homebrew/opt'
    )
    foreach ($root in $roots) {
        $found = Get-ChildItem -Path $root -Directory -Filter 'jdk*' -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            Select-Object -First 1
        if ($found) {
            return $found.FullName
        }
    }
    return $null
}

[string] $javaHome = Find-JavaHome
if ([string]::IsNullOrWhiteSpace($javaHome)) {
    Write-Host 'FAILED. No JDK; prerequisite.toml declares java.'
    exit 2
}
[string] $probe = $env:XMIP_CONTRACT_PROBE
if ([string]::IsNullOrWhiteSpace($probe)) {
    $probe = Join-Path $PSScriptRoot '..' 'probe'
}
[string] $verify = Join-Path $probe 'verify.ps1'
if (-not (Test-Path -LiteralPath $verify)) {
    Write-Host "FAILED. The capability's probe is not at $probe; set XMIP_CONTRACT_PROBE."
    exit 2
}

[string] $bin = Join-Path $javaHome 'bin'
[string] $javac = Join-Path $bin 'javac'
[string] $java = Join-Path $bin 'java'
[string] $jar = Join-Path $bin 'jar'
[string] $jniPlatform = 'linux'
if ($IsWindows) {
    $jniPlatform = 'win32'
}
elseif ($IsMacOS) {
    $jniPlatform = 'darwin'
}
[string] $jniInclude = Join-Path $javaHome 'include'
New-Item -ItemType Directory -Force -Path build/classes | Out-Null

Write-Host "   javac -> build/classes  (JDK at $javaHome)"
[string[]] $sources = (Get-ChildItem java -Recurse -Filter '*.java').FullName
& $javac -d build/classes $sources tests/ContractTest.java
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host '   java ContractTest'
& $java -cp build/classes ContractTest
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host '   jar -> build/xmip-core-contract-java.jar'
& $jar --create --file build/xmip-core-contract-java.jar -C build/classes xmip
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

# The probe loads the JVM at run time through the shim, from these.
$env:JAVA_HOME = $javaHome
$env:XMIP_JAVA_CLASSPATH = (Resolve-Path build/xmip-core-contract-java.jar).Path
if ($IsWindows) {
    $env:Path = "$(Join-Path $javaHome 'bin' 'server');$(Join-Path $javaHome 'bin');$env:Path"
}

[hashtable] $build = @{
    Directory = $PSScriptRoot
    Compiler  = 'cc'
    Source    = @('shim/xmip_java_shim.c')
    Standard  = 'java'
    Include   = @($jniInclude, (Join-Path $jniInclude $jniPlatform))
}
& $verify @build
exit $LASTEXITCODE
