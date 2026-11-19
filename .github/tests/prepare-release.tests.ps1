#Requires -Version 7.2
[CmdletBinding()]
param([string] $Root = (Join-Path $PSScriptRoot '../..'))

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath $Root).Path
$prepare = Join-Path $Root '.github/scripts/prepare-release.ps1'
$fixture = Join-Path $Root "build/prepare-release-tests-$([Guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory -Path (Join-Path $fixture 'src'), (Join-Path $fixture 'build/release') -Force
$checks = 0
$savedOutput = $env:GITHUB_OUTPUT
try {
    foreach ($name in @('host.h', 'app.rc', 'app.manifest')) { Copy-Item -LiteralPath (Join-Path $Root "src/$name") -Destination (Join-Path $fixture 'src') }
    Copy-Item -LiteralPath (Join-Path $Root 'build/release/cloakspace.exe') -Destination (Join-Path $fixture 'build/release')
    foreach ($name in @('README.md', 'LICENSE', 'scratchpad.conf', 'docs', 'examples')) { Copy-Item -LiteralPath (Join-Path $Root $name) -Destination $fixture -Recurse }
    $headerPath = Join-Path $fixture 'src/host.h'
    $rcPath = Join-Path $fixture 'src/app.rc'
    $manifestPath = Join-Path $fixture 'src/app.manifest'
    $header = [IO.File]::ReadAllText($headerPath)
    $rc = [IO.File]::ReadAllText($rcPath)
    $manifest = [IO.File]::ReadAllText($manifestPath)
    $definition = [regex]::Match($header, '(?m)^#define SP_VERSION[^\r\n]*').Value
    $env:GITHUB_OUTPUT = Join-Path $fixture 'outputs.txt'
    $first = & $prepare -Root $fixture -OutputDirectory 'out-one'
    $checks++
    if ([IO.Path]::GetFileName($first.Archive) -cne "cloakspace-$($first.Tag)-windows-x64.zip") { throw 'Archive name must use the cloakspace identity, validated tag and Windows x64 suffix.' }
    $checks++
    $expectedOutputs = "version=$($first.Version)`ntag=$($first.Tag)`narchive=$([IO.Path]::GetFileName($first.Archive))`n"
    if ([IO.File]::ReadAllText($env:GITHUB_OUTPUT) -cne $expectedOutputs) { throw 'GITHUB_OUTPUT did not contain the exact validated version/tag/archive.' }
    $checks++
    $env:GITHUB_OUTPUT = $null
    $second = & $prepare -Root $fixture -OutputDirectory 'out-two'
    if ($first.SHA256 -cne $second.SHA256) { throw 'The same release inputs produced different archive hashes.' }
    $checks++
    $zip = [IO.Compression.ZipFile]::OpenRead($first.Archive)
    try {
        $expectedNames = @('cloakspace.exe', 'scratchpad.conf', 'README.md', 'LICENSE')
        foreach ($directory in @('docs', 'examples')) {
            foreach ($file in Get-ChildItem -LiteralPath (Join-Path $fixture $directory) -Recurse -File) { $expectedNames += [IO.Path]::GetRelativePath($fixture, $file.FullName).Replace('\', '/') }
        }
        if ((@($zip.Entries.FullName | Sort-Object) -join "`n") -cne (@($expectedNames | Sort-Object) -join "`n")) { throw 'ZIP file inventory differs from the explicit portable payload.' }
        foreach ($entry in $zip.Entries) {
            if ($entry.LastWriteTime.Year -ne 1980) { throw 'ZIP timestamps are not normalized.' }
            $source = if ($entry.FullName -eq 'cloakspace.exe') { Join-Path $fixture 'build/release/cloakspace.exe' } else { Join-Path $fixture $entry.FullName }
            $stream = $entry.Open()
            try {
                $actualHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream))
                if ($actualHash -cne (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash) { throw "ZIP entry bytes differ: $($entry.FullName)" }
            } finally { $stream.Dispose() }
        }
    } finally { $zip.Dispose() }
    $checks++

    function Assert-Rejected([string] $Name, [scriptblock] $Action, [string] $Pattern) {
        $rejected = $false
        try { $null = & $Action } catch {
            if ($_.Exception.Message -notmatch $Pattern) { throw "${Name}: unexpected failure: $($_.Exception.Message)" }
            $rejected = $true
        }
        if (-not $rejected) { throw "${Name}: expected rejection, but packaging succeeded." }
        Write-Host "PASS: $Name"
        $script:checks++
    }
    foreach ($case in @(
        @{ Name = 'missing SP_VERSION'; Definition = '#define SOMETHING_ELSE L"1.1.0"'; Pattern = 'exactly one SP_VERSION' },
        @{ Name = 'duplicate SP_VERSION'; Definition = "$definition`n$definition"; Pattern = 'exactly one SP_VERSION' },
        @{ Name = 'malformed version'; Definition = '#define SP_VERSION L"1.1"'; Pattern = 'MAJOR.MINOR.PATCH' },
        @{ Name = 'leading zero'; Definition = '#define SP_VERSION L"01.1.0"'; Pattern = 'MAJOR.MINOR.PATCH' },
        @{ Name = 'prerelease suffix'; Definition = '#define SP_VERSION L"1.1.0-rc1"'; Pattern = 'MAJOR.MINOR.PATCH' },
        @{ Name = 'Windows version overflow'; Definition = '#define SP_VERSION L"65536.0.0"'; Pattern = '0..65535' }
    )) {
        try {
            [IO.File]::WriteAllText($headerPath, $header.Replace($definition, $case.Definition))
            Assert-Rejected $case.Name { & $prepare -Root $fixture -OutputDirectory 'must-not-exist' } $case.Pattern
        } finally { [IO.File]::WriteAllText($headerPath, $header) }
    }
    try {
        [IO.File]::WriteAllText($rcPath, [regex]::Replace($rc, '(?m)^FILEVERSION[^\r\n]*', 'FILEVERSION 99,0,0,0'))
        Assert-Rejected 'numeric VERSIONINFO mismatch' { & $prepare -Root $fixture -OutputDirectory 'must-not-exist' } 'FILEVERSION must match'
        [IO.File]::WriteAllText($rcPath, [regex]::Replace($rc, '(?m)^([ \t]*VALUE "ProductVersion", )"[^"\r\n]*"', '$1"99.0.0"'))
        Assert-Rejected 'string VERSIONINFO mismatch' { & $prepare -Root $fixture -OutputDirectory 'must-not-exist' } 'ProductVersion must match'
    } finally { [IO.File]::WriteAllText($rcPath, $rc) }
    try {
        [xml] $changed = $manifest
        $changed.assembly.assemblyIdentity.version = (@('0', '0', '0', '0') -join '.')
        $changed.Save($manifestPath)
        Assert-Rejected 'manifest mismatch' { & $prepare -Root $fixture -OutputDirectory 'must-not-exist' } 'manifest version must match'
    } finally { [IO.File]::WriteAllText($manifestPath, $manifest) }
    try {
        $version = $first.Version
        [IO.File]::WriteAllText($headerPath, $header.Replace($version, '99.0.0'))
        [IO.File]::WriteAllText($rcPath, $rc.Replace($version, '99.0.0').Replace(($version.Replace('.', ',') + ',0'), '99,0,0,0'))
        [IO.File]::WriteAllText($manifestPath, $manifest.Replace("$version.0", (@('99', '0', '0', '0') -join '.')))
        Assert-Rejected 'stale compiled executable version' { & $prepare -Root $fixture -OutputDirectory 'must-not-exist' } 'executable FileVersion/ProductVersion'
    } finally {
        [IO.File]::WriteAllText($headerPath, $header)
        [IO.File]::WriteAllText($rcPath, $rc)
        [IO.File]::WriteAllText($manifestPath, $manifest)
    }
    $configPath = Join-Path $fixture 'scratchpad.conf'
    $config = [IO.File]::ReadAllText($configPath)
    try {
        [IO.File]::WriteAllText($configPath, "invalid-release-config-directive`n$config")
        Assert-Rejected 'invalid shipped configuration' { & $prepare -Root $fixture -OutputDirectory 'must-not-exist' } 'executable validation failed'
    } finally { [IO.File]::WriteAllText($configPath, $config) }
    foreach ($case in @(
        @{ Path = 'docs/accidental.pdb'; Pattern = 'Unexpected release input' },
        @{ Path = 'examples/.unreviewed.conf'; Pattern = 'Unexpected hidden release input' }
    )) {
        $unexpected = Join-Path $fixture $case.Path
        try {
            [IO.File]::WriteAllText($unexpected, 'not a release input')
            Assert-Rejected "unintended file $($case.Path)" { & $prepare -Root $fixture -OutputDirectory 'must-not-exist' } $case.Pattern
        } finally { Remove-Item -LiteralPath $unexpected -Force }
    }
    Assert-Rejected 'nonempty output is preserved' { & $prepare -Root $fixture -OutputDirectory 'out-one' } 'must be empty'
    if ((Get-FileHash -LiteralPath $first.Archive -Algorithm SHA256).Hash.ToLowerInvariant() -cne $first.SHA256) { throw 'Nonempty-output rejection modified an existing archive.' }
    $checks++
    if (Test-Path -LiteralPath (Join-Path $fixture 'must-not-exist')) { throw 'A rejected input created release output.' }
    $checks++
    Write-Host "PASS: $checks release preparation checks (real Windows executable, no GitHub writes)."
} finally {
    $env:GITHUB_OUTPUT = $savedOutput
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
