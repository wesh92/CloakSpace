#Requires -Version 7.0
[CmdletBinding()]
param(
    [string] $Root = (Join-Path $PSScriptRoot '../..'),
    [string] $OutputDirectory = 'build/release-assets'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath $Root).Path

function Get-OneCapture([string] $Text, [string] $Pattern, [string] $Label) {
    $found = [regex]::Matches($Text, $Pattern)
    if ($found.Count -ne 1) { throw "Expected exactly one $Label; found $($found.Count)." }
    return $found[0].Groups[1].Value.Trim()
}

$header = [IO.File]::ReadAllText((Join-Path $Root 'src/host.h'))
$definition = Get-OneCapture $header '(?m)^[ \t]*#[ \t]*define[ \t]+SP_VERSION[ \t]+([^\r\n]*)\r?$' 'SP_VERSION definition in src/host.h'
$match = [regex]::Match($definition, '^L"((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))"$')
if (-not $match.Success) { throw 'SP_VERSION must be a wide-string MAJOR.MINOR.PATCH without leading zeroes or a prerelease suffix.' }
$version = $match.Groups[1].Value
foreach ($part in $version.Split('.')) {
    if ($part.Length -gt 5 -or [int] $part -gt 65535) { throw 'Version components must fit Windows VERSIONINFO (0..65535).' }
}
$tag = "v$version"
$numericVersion = $version.Replace('.', ',') + ',0'
$rc = [IO.File]::ReadAllText((Join-Path $Root 'src/app.rc'))
foreach ($field in @('FILEVERSION', 'PRODUCTVERSION')) {
    $value = Get-OneCapture $rc "(?m)^[ \t]*$field[ \t]+([^\r\n]*)\r?`$" "src/app.rc $field"
    if (($value -replace '[ \t]', '') -cne $numericVersion) { throw "src/app.rc $field must match SP_VERSION ($numericVersion)." }
}
foreach ($field in @('FileVersion', 'ProductVersion')) {
    $value = Get-OneCapture $rc "(?m)^[ \t]*VALUE[ \t]+`"$field`"[ \t]*,[ \t]*`"([^`"\r\n]+)`"[ \t]*\r?`$" "src/app.rc $field"
    if ($value -cne $version) { throw "src/app.rc $field must match SP_VERSION ($version)." }
}
[xml] $manifest = [IO.File]::ReadAllText((Join-Path $Root 'src/app.manifest'))
if ($manifest.assembly.assemblyIdentity.version -cne "$version.0") { throw "src/app.manifest version must match SP_VERSION ($version.0)." }

$exe = Join-Path $Root 'build/release/cloakspace.exe'
$info = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
if ($info.FileVersion -cne $version -or $info.ProductVersion -cne $version -or $info.IsDebug) {
    throw "Release executable FileVersion/ProductVersion must equal $version and must not be a debug build."
}
$bytes = [IO.File]::ReadAllBytes($exe)
if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5a4d) { throw 'Release executable is not a PE file.' }
$pe = [BitConverter]::ToInt32($bytes, 0x3c)
if ($pe -lt 64 -or $pe -gt $bytes.Length - 94 -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or
    [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664 -or [BitConverter]::ToUInt16($bytes, $pe + 24) -ne 0x20b -or
    [BitConverter]::ToUInt16($bytes, $pe + 92) -ne 2 -or [BitConverter]::ToUInt32($bytes, $pe + 16) -ne 0) {
    throw 'Release executable must be a stripped Windows x64 GUI PE (no COFF symbols).'
}

function Invoke-ReleaseExecutable([string[]] $Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new($exe)
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.WorkingDirectory = $Root
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(10000)) { $process.Kill($true); throw 'Release executable validation timed out.' }
        $output = $stdout.GetAwaiter().GetResult().Trim()
        $errorOutput = $stderr.GetAwaiter().GetResult().Trim()
        if ($process.ExitCode -ne 0) { throw "Release executable validation failed ($($process.ExitCode)): $errorOutput $output" }
        return $output
    } finally { $process.Dispose() }
}

$reported = Invoke-ReleaseExecutable -Arguments @('--version')
if ($reported -cne "cloakspace $version (C17 / Win32)") { throw "Release executable --version does not match SP_VERSION: $reported" }
foreach ($configuration in @(
    @{ Path = 'scratchpad.conf'; Profiles = @('default', 'portable') },
    @{ Path = 'examples/never-elevate.conf'; Profiles = @('default', 'other') },
    @{ Path = 'examples/imports/main.conf'; Profiles = @('default', 'work') }
)) {
    foreach ($profile in $configuration.Profiles) {
        $null = Invoke-ReleaseExecutable -Arguments @('--config', (Join-Path $Root $configuration.Path), '--profile', $profile, '--check-config')
    }
}

# An allowlist prevents caches, test binaries, personal configs and resource objects from shipping.
$files = [Collections.Generic.List[object]]::new()
$files.Add(@{ Source = $exe; Name = 'cloakspace.exe' })
foreach ($name in @('scratchpad.conf', 'README.md', 'LICENSE')) { $files.Add(@{ Source = (Join-Path $Root $name); Name = $name }) }
foreach ($directory in @('docs', 'examples')) {
    $parent = Get-Item -LiteralPath (Join-Path $Root $directory)
    if ($parent.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Do not package linked directories: $directory" }
    foreach ($item in Get-ChildItem -LiteralPath $parent.FullName -Recurse -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Do not package linked paths: $($item.FullName)" }
        $relative = [IO.Path]::GetRelativePath($Root, $item.FullName).Replace('\', '/')
        if (($relative.Split('/') | Where-Object { $_.StartsWith('.') }) -or ($item.Attributes -band [IO.FileAttributes]::Hidden)) {
            throw "Unexpected hidden release input: $relative"
        }
        if (-not $item.PSIsContainer) {
            $allowedExtensions = if ($directory -eq 'docs') { @('.md') } else { @('.conf', '.c', '.h') }
            if ($item.Extension.ToLowerInvariant() -notin $allowedExtensions) { throw "Unexpected release input under ${directory}: $relative" }
            $files.Add(@{ Source = $item.FullName; Name = $relative })
        }
    }
}
foreach ($file in $files) {
    $item = Get-Item -LiteralPath $file.Source
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Not a regular release input: $($file.Source)" }
}

if (-not [IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory = Join-Path $Root $OutputDirectory }
if ((Test-Path -LiteralPath $OutputDirectory) -and @(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count -ne 0) {
    throw "Release output directory must be empty (nothing was deleted): $OutputDirectory"
}
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$archiveName = "cloakspace-$tag-windows-x64.zip"
$archivePath = Join-Path $OutputDirectory $archiveName
$archive = [IO.Compression.ZipFile]::Open($archivePath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in $files | Sort-Object -Property Name -CaseSensitive) {
        $entry = $archive.CreateEntry($file.Name, [IO.Compression.CompressionLevel]::Optimal)
        # Stable timestamps make retries of the same source reproducible.
        $entry.LastWriteTime = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
        $inputStream = [IO.File]::OpenRead($file.Source)
        try {
            $outputStream = $entry.Open()
            try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
        } finally { $inputStream.Dispose() }
    }
} finally { $archive.Dispose() }
$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'SHA256SUMS'), "$hash  $archiveName`n", [Text.UTF8Encoding]::new($false))
if ($env:GITHUB_OUTPUT) {
    [IO.File]::AppendAllText($env:GITHUB_OUTPUT, "version=$version`ntag=$tag`narchive=$archiveName`n", [Text.UTF8Encoding]::new($false))
}
Write-Host "Prepared $archiveName; CLI, VERSIONINFO, manifest, PE and six configuration checks passed."
[pscustomobject] @{ Version = $version; Tag = $tag; Archive = $archivePath; SHA256 = $hash }
