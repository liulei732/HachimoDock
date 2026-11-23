# The Rust library test harness is linked separately from the desktop app.
# Give it the same Common Controls activation context before running UI-linked code.
[CmdletBinding()]
param([string[]] $Suites = @(''))
$ErrorActionPreference = 'Stop'
$manifestPath = Join-Path $PSScriptRoot '../src-tauri/Cargo.toml'
$messages = & cargo test --locked --manifest-path $manifestPath --lib --no-run --message-format=json-render-diagnostics
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$executables = @($messages | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object {
    $_.reason -eq 'compiler-artifact' -and $_.target.name -eq 'pet_manager_tauri_lib' -and $_.profile.test -and $_.executable
} | ForEach-Object { $_.executable })
if ($executables.Count -ne 1) { throw 'Expected one Rust library test executable' }
$testExe = $executables[0]
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$mt = Get-ChildItem -Path "$sdk\*\x64\mt.exe" | Sort-Object {
    [version]$_.Directory.Parent.Name
} -Descending | Select-Object -First 1
if (!$mt) { throw 'Windows SDK manifest tool unavailable' }
$manifest = Join-Path $env:RUNNER_TEMP 'desktop-test-harness.manifest'
@'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <dependency><dependentAssembly>
    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*" />
  </dependentAssembly></dependency>
</assembly>
'@ | Set-Content -Encoding utf8NoBOM -LiteralPath $manifest
& $mt.FullName -nologo -manifest $manifest "-outputresource:$testExe;#1"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location (Split-Path -Parent $manifestPath)
try {
    foreach ($suite in $Suites) {
        if ($suite) {
            & $testExe $suite
        } else {
            python (Join-Path $PSScriptRoot 'run-native-test-cases.py') $testExe
        }
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
} finally { Pop-Location }
