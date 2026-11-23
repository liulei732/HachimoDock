#requires -Version 7.2
<#
.SYNOPSIS
Exercise the release installer on a fresh GitHub-hosted Windows x64 runner.
.EXAMPLE
pwsh -NoProfile -File ./pc/scripts/smoke-release-windows.ps1 -Version 0.1.254 -Installer ./candidate.exe -ExpectedSha256 $auditedHash
.NOTES
Download the draft asset in a preceding step. Pin ExpectedSha256 from the independent
audit receipt. Give this step no secrets, restored profiles, or persisted checkout
credentials. Only upload the report, never the temporary app profile or raw logs.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')][string] $Version,
    [Parameter(Mandatory)][string] $Installer,
    [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{64}$')][string] $ExpectedSha256,
    [ValidatePattern('^[a-fA-F0-9]{64}$')][string] $ExpectedExeSha256,
    [string] $ReportPath = (Join-Path $env:RUNNER_TEMP 'windows-smoke-result.json')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Join-Path $env:RUNNER_TEMP ('hachimi-' + [guid]::NewGuid().ToString('N'))
$install = Join-Path $root 'app'
$userDir = Join-Path $root 'user'
$tempDir = Join-Path $root 'tmp'
$registry = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\com.petmanager.desktop'
$exe = Join-Path $install 'Hachimodock.exe'
$uninstaller = Join-Path $install 'Uninstall.exe'
$report = [ordered]@{ version=$version; passed=$false; stage='runner'; failures=@(); checks=[ordered]@{} }
$installedHere = $false

# Observe native startup without forwarding GUI output.
function Initialize-SmokeProbe {
    Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
public sealed class SmokeReady {
    public volatile bool Accepted, StaleReady, ReadyTimeout, LoadStarted, LoadFinished;
    public volatile string PanicFile, ErrorType, WebViewHResult;
    public volatile int PanicLine;
    public readonly ManualResetEventSlim StderrClosed = new ManualResetEventSlim();
    static readonly Regex Location = new Regex(@"(?:^|[/\\ ])([A-Za-z0-9_-]{1,64}\.rs):([0-9]{1,7}):[0-9]{1,5}:?$");
    static readonly Regex HResult = new Regex(@"\bHRESULT\(0x([0-9A-Fa-f]{8})\)");
    void ObserveFailure(string line) {
        if (line.Length > 8192) return;
        if (line.StartsWith("thread '",StringComparison.Ordinal) && line.Contains(" panicked at ")) {
            ErrorType="RustPanic";
            var location=Location.Match(line);
            if (location.Success) {
                PanicFile=location.Groups[1].Value;
                PanicLine=int.Parse(location.Groups[2].Value);
            }
        }
        if (ErrorType==null) return;
        if (line.Contains("WebView2Error(") || line.Contains("WebView2 error:") || line.Contains("CreateWebview(")) {
            ErrorType=line.Contains("WebView2") ? "WebView2Error" : "CreateWebview";
            var hr=HResult.Match(line);
            if (hr.Success) WebViewHResult="0x"+hr.Groups[1].Value.ToUpperInvariant();
        } else if (line.Contains("CreateWindow")) ErrorType="CreateWindow";
        else if (line.Contains("SetupError(") || line.Contains("Setup(")) ErrorType="Setup";
        else if (line.Contains("already managed")) ErrorType="DuplicateManagedState";
        else if (line.Contains("PluginInitialization(")) ErrorType="PluginInitialization";
        else if (line.Contains("InvalidIcon(")) ErrorType="InvalidIcon";
        else if (line.Contains("AssetNotFound(")) ErrorType="AssetNotFound";
        else if (line.Contains("Io(Os {")) ErrorType="Io";
    }
    public void Attach(Process p) {
        p.ErrorDataReceived += (s,e) => {
            string line=e.Data;
            if (line==null) { StderrClosed.Set(); return; }
            ObserveFailure(line);
            if (line=="[startup-native] category=ready") Accepted=true;
            if (line=="[startup-native] category=stale_ready") StaleReady=true;
            if (line=="[startup-native] category=ready_timeout") ReadyTimeout=true;
            if (line.StartsWith("[webview-load] window=main phase=Started ",StringComparison.Ordinal)) LoadStarted=true;
            if (line.StartsWith("[webview-load] window=main phase=Finished ",StringComparison.Ordinal)) LoadFinished=true;
        };
        p.OutputDataReceived += (s,e) => {};
    }
}
public static class SmokeWindow {
    delegate bool EnumProc(IntPtr hwnd, IntPtr unused);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc fn, IntPtr arg);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr hwnd,uint msg,IntPtr w,IntPtr l,uint flags,uint ms,out IntPtr result);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetMenuString(IntPtr menu,uint pos,StringBuilder name,int n,uint flags);
    [DllImport("user32.dll")] static extern uint GetMenuItemID(IntPtr menu,int pos);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd,StringBuilder title,int n);
    public static bool Responsive(IntPtr hwnd) {
        IntPtr result;
        return SendMessageTimeout(hwnd,0,IntPtr.Zero,IntPtr.Zero,2,500,out result)!=IntPtr.Zero;
    }
    public static IntPtr Owned(int process, string cls, string title) {
        IntPtr found=IntPtr.Zero;
        EnumWindows((h,u)=>{
            uint pid;
            GetWindowThreadProcessId(h,out pid);
            if(pid!=process) return true;
            var name=new StringBuilder(128);
            if(!String.IsNullOrEmpty(cls)) {
                GetClassName(h,name,name.Capacity);
                if(name.ToString()!=cls) return true;
            }
            if(!String.IsNullOrEmpty(title)) {
                GetWindowText(h,name,name.Capacity);
                if(name.ToString()!=title) return true;
            }
            found=h; return false;
        },IntPtr.Zero);
        return found;
    }
    [DllImport("user32.dll")] static extern bool PostThreadMessage(uint thread,uint msg,IntPtr w,IntPtr l);
    public static bool QuitLoop(IntPtr main, int expectedPid) {
        uint pid;
        uint thread=GetWindowThreadProcessId(main,out pid);
        return thread!=0 && pid==expectedPid && PostThreadMessage(thread,0x0012,IntPtr.Zero,IntPtr.Zero);
    }
    public static bool OpenTray(IntPtr tray) {
        // tray-icon 0.23.1 uses 6002 for the shell's tray notification.
        return PostMessage(tray,6002,IntPtr.Zero,(IntPtr)0x0205);
    }
    public static bool QuitMenu(IntPtr tray,IntPtr popup) {
        IntPtr menu;
        // MN_GETHMENU for the popup belonging to this candidate process.
        if(SendMessageTimeout(popup,0x01e1,IntPtr.Zero,IntPtr.Zero,2,1000,out menu)==IntPtr.Zero) return false;
        for(int i=0;i<GetMenuItemCount(menu);i++) {
            var name=new StringBuilder(128);
            GetMenuString(menu,(uint)i,name,name.Capacity,0x400);
            if(name.ToString().Replace("&","")!="\u9000\u51fa HachimiDock") continue;
            uint command=GetMenuItemID(menu,i);
            if(command==0 || command>65535) return false;
            PostMessage(tray,0x001f,IntPtr.Zero,IntPtr.Zero);
            return PostMessage(tray,0x0111,(IntPtr)command,IntPtr.Zero);
        }
        return false;
    }
}
'@
}

function Write-SmokeFailure([System.Management.Automation.ErrorRecord]$Record, [switch]$Comparison) {
    # Do not serialize the message, invocation text, stack, native output or paths.
    $detail = [ordered]@{
        stage = $report.stage
        script = 'smoke-release-windows.ps1'
        line = [int]$Record.InvocationInfo.ScriptLineNumber
        column = [int]$Record.InvocationInfo.OffsetInLine
        category = $Record.CategoryInfo.Category.ToString()
    }
    $cause = $Record.Exception.GetBaseException()
    if ($cause -is [ComponentModel.Win32Exception]) { $detail.win32Code = $cause.NativeErrorCode }
    if ($Record.Exception.Data.Contains('smokeExitCode')) { $detail.exitCode = [int]$Record.Exception.Data['smokeExitCode'] }
    if ($Comparison) {
        $report.syntheticFailure = $detail
    } else {
        if (!$report.Contains('failureStage')) { $report.failureStage = $detail.stage }
        $report.failures += $detail
    }
    if ($detail.stage -in 'gui','gui-ready','exit','gui-cleanup' -and $report.Contains('guiProbe')) { $detail.probe = $report.guiProbe }
    Write-Host ('Windows smoke failure ' + ($detail | ConvertTo-Json -Compress))
}

function Assert-SmokeExit([Diagnostics.Process]$Process) {
    if ($Process.ExitCode -ne 0) {
        $exception = [Exception]::new('Candidate returned a nonzero exit code')
        $exception.Data['smokeExitCode'] = $Process.ExitCode
        throw $exception
    }
}

function New-SmokeProcess([string]$File, [string[]]$Arguments, [switch]$RunnerProfile) {
    $p = [Diagnostics.Process]::new()
    $start = $p.StartInfo
    $start.UseShellExecute = $false
    $start.FileName = $File
    $start.WorkingDirectory = $root
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Environment.Clear()
    foreach ($name in 'SystemRoot','WINDIR','ComSpec','SystemDrive','ProgramFiles','ProgramFiles(x86)',
                      'ProgramW6432','ProgramData','OS','PROCESSOR_ARCHITECTURE','NUMBER_OF_PROCESSORS') {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($value) { $start.Environment[$name] = $value }
    }
    $start.Environment['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
    if ($RunnerProfile) {
        foreach ($name in 'USERPROFILE','APPDATA','LOCALAPPDATA','USERNAME','USERDOMAIN','COMPUTERNAME',
                          'HOMEDRIVE','HOMEPATH','TEMP','TMP') {
            $value = [Environment]::GetEnvironmentVariable($name)
            if ($value) { $start.Environment[$name] = $value }
        }
        $start.Environment['HOME'] = $env:USERPROFILE
    } else {
        $start.Environment['TEMP'] = $tempDir
        $start.Environment['TMP'] = $tempDir
        $start.Environment['HOME'] = $userDir
        $start.Environment['USERPROFILE'] = $userDir
        $start.Environment['APPDATA'] = Join-Path $userDir 'Roaming'
        $start.Environment['LOCALAPPDATA'] = Join-Path $userDir 'Local'
        $start.Environment['WEBVIEW2_USER_DATA_FOLDER'] = Join-Path $userDir 'WebView'
    }
    foreach ($arg in $Arguments) { $start.ArgumentList.Add($arg) }
    return $p
}

function Invoke-SmokeProcess([string]$File, [string[]]$Arguments, [int]$Timeout = 120, [bool]$Capture = $true) {
    $p = New-SmokeProcess $File $Arguments
    $p.StartInfo.RedirectStandardOutput = $Capture
    $p.StartInfo.RedirectStandardError = $Capture
    try {
        if (!$p.Start()) { throw 'Process did not start' }
        if ($Capture) {
            $out = $p.StandardOutput.ReadToEndAsync()
            $err = $p.StandardError.ReadToEndAsync()
        }
        if (!$p.WaitForExit(1000 * $Timeout)) {
            $p.Kill($true)
            throw 'Candidate operation timed out'
        }
        Assert-SmokeExit $p
        if (!$Capture) { return }
        if (!$out.Wait(5000) -or !$err.Wait(5000)) { throw 'Candidate output did not close' }
        return $out.Result
    } finally { $p.Dispose() }
}

function Wait-For([scriptblock]$Condition, [int]$Seconds) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 250
    } while ($timer.Elapsed.TotalSeconds -lt $Seconds)
    throw "Timed out at $($report.stage)"
}

function Get-WebView2Registration {
    $key = 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'
    $versions = [ordered]@{}
    foreach ($scope in 'LocalMachine','CurrentUser') {
        foreach ($view in 'Registry32','Registry64') {
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($scope,$view)
            try {
                $entry = $base.OpenSubKey($key)
                try {
                    $pv = if ($entry) { $entry.GetValue('pv') }
                    $versions["$scope-$view"] = if ("$pv" -match '^\d{1,5}(\.\d{1,5}){3}$') { "$pv" } else { $null }
                } finally { if ($entry) { $entry.Dispose() } }
            } finally { $base.Dispose() }
        }
    }
    return $versions
}

function Invoke-SmokeGui([ValidateSet('synthetic','runner')][string]$ProfileName) {
    $probe = [ordered]@{
        profile=$ProfileName; sessionZero=([Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0)
        frontendReady=$false; staleReady=$false; nativeTimeout=$false
        loadStarted=$false; loadFinished=$false; guiReady=$false; cleanExit=$false
        hasWindow=$false; visible=$false; responding=$false
    }
    $report.guiProbe = $probe
    $report.guiProfiles[$ProfileName] = $probe
    $report.stage = 'gui'
    $gui = New-SmokeProcess $exe @() -RunnerProfile:($ProfileName -eq 'runner')
    $ready = [SmokeReady]::new()
    $ready.Attach($gui)
    $started = $false
    try {
        if (!$gui.Start()) { throw 'GUI did not start' }
        $started = $true
        $gui.BeginErrorReadLine()
        $gui.BeginOutputReadLine()
        $report.stage = 'gui-ready'
        Wait-For {
            $gui.Refresh()
            if ($gui.HasExited) { Assert-SmokeExit $gui; throw 'GUI exited during startup' }
            $probe.frontendReady = $ready.Accepted
            $handle = [SmokeWindow]::Owned($gui.Id,'','Hachimodock')
            $probe.hasWindow = $handle -ne [IntPtr]::Zero
            $probe.visible = $probe.hasWindow -and [SmokeWindow]::IsWindowVisible($handle)
            $probe.responding = $probe.visible -and [SmokeWindow]::Responsive($handle)
            $probe.frontendReady -and $probe.responding
        } 60
        $probe.guiReady = $true

        $report.stage = 'exit'
        $mainWindow = [SmokeWindow]::Owned($gui.Id,'','Hachimodock')
        $tray = [SmokeWindow]::Owned($gui.Id,'tray_icon_app','')
        $quitMode = 'tray-menu'
        $menuInvoked = $false
        if ($tray -ne [IntPtr]::Zero -and [SmokeWindow]::OpenTray($tray)) {
            $timer = [Diagnostics.Stopwatch]::StartNew()
            do {
                $popup = [SmokeWindow]::Owned($gui.Id,'#32768','')
                if ($popup -ne [IntPtr]::Zero) {
                    if (![SmokeWindow]::QuitMenu($tray,$popup)) { throw 'Candidate tray Quit command unavailable' }
                    $menuInvoked = $true
                    break
                }
                Start-Sleep -Milliseconds 200
            } while ($timer.Elapsed.TotalSeconds -lt 8)
        }
        if (!$menuInvoked) {
            # Hosted sessions can lack an Explorer notification area. Exercise the
            # candidate GUI thread's normal message-loop termination in that case.
            $quitMode = 'win32-message-loop'
            if (![SmokeWindow]::QuitLoop($mainWindow,$gui.Id)) { throw 'Could not request GUI thread exit' }
        }
        $probe.exitRoute = $quitMode
        if (!$gui.WaitForExit(30000)) { throw 'Candidate did not quit cleanly' }
        Assert-SmokeExit $gui
        $probe.cleanExit = $true
    } finally {
        try {
            if ($started) {
                if (!$gui.HasExited) {
                    $gui.Kill($true)
                    if (!$gui.WaitForExit(10000)) { throw 'GUI cleanup timed out' }
                }
                # The process can exit before the asynchronous panic and EOF callbacks arrive.
                $probe.stderrDrained = $ready.StderrClosed.Wait(3000)
                $probe.frontendReady = $ready.Accepted
                $probe.staleReady = $ready.StaleReady
                $probe.nativeTimeout = $ready.ReadyTimeout
                $probe.loadStarted = $ready.LoadStarted
                $probe.loadFinished = $ready.LoadFinished
                $probe.panicFile = $ready.PanicFile
                $probe.panicLine = if ($ready.PanicFile) { $ready.PanicLine } else { $null }
                $probe.errorType = $ready.ErrorType
                $probe.webViewHResult = $ready.WebViewHResult
                $probe.processExitCode = $gui.ExitCode
            }
        } catch {
            $report.stage = 'gui-cleanup'
            throw
        } finally { $gui.Dispose() }
    }
}

try {
    if (-not $IsWindows -or $env:RUNNER_OS -ne 'Windows' -or $env:RUNNER_ARCH -ne 'X64' -or
        $env:RUNNER_ENVIRONMENT -ne 'github-hosted' -or -not [Environment]::Is64BitProcess) {
        throw 'Use a fresh GitHub-hosted x64 Windows job with pwsh.'
    }
    $report.stage = 'probe-init'
    Initialize-SmokeProbe
    $report.stage = 'candidate'
    $setup = (Resolve-Path -LiteralPath $Installer).ProviderPath
    $hash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $ExpectedSha256) { throw 'Candidate digest mismatch' }
    $report.installerSha256 = $hash
    if (Test-Path -LiteralPath $registry) { throw 'Existing product registration on runner' }
    foreach ($path in $root,$userDir,$tempDir,(Join-Path $userDir 'Roaming'),(Join-Path $userDir 'Local')) {
        $null = New-Item -ItemType Directory -Path $path
    }
    $report.stage = 'webview-registration-before-install'
    $report.webView2BeforeInstall = Get-WebView2Registration
    $report.stage = 'install'
    # NSIS requires the unquoted /D= override last, including paths with spaces.
    $installerProcess = New-SmokeProcess $setup @()
    $installerProcess.StartInfo.Arguments = "/S /D=$install"
    $installerProcess.StartInfo.RedirectStandardOutput = $false
    $installerProcess.StartInfo.RedirectStandardError = $false
    try {
        if (!$installerProcess.Start()) { throw 'Installer did not start' }
        if (!$installerProcess.WaitForExit(240000)) { $installerProcess.Kill($true); throw 'Installer timed out' }
        Assert-SmokeExit $installerProcess
    } finally { $installerProcess.Dispose() }
    $report.stage = 'install-registration'
    $registration = Get-ItemProperty -LiteralPath $registry
    if ($registration.InstallLocation -ne $install) {
        throw 'Installed product registration mismatch'
    }
    $installedHere = $true
    $report.stage = 'webview-registration-after-install'
    $report.webView2AfterInstall = Get-WebView2Registration
    $report.stage = 'install-version'
    if ($registration.DisplayVersion -ne $Version) { throw 'Installed registration version mismatch' }
    $v = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
    $fixed = '{0}.{1}.{2}.{3}' -f $v.ProductMajorPart,$v.ProductMinorPart,$v.ProductBuildPart,$v.ProductPrivatePart
    if ($fixed -ne "$version.0") { throw 'Installed EXE product version mismatch' }
    $report.exeSha256 = (Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()
    if ($ExpectedExeSha256 -and $report.exeSha256 -ne $ExpectedExeSha256) { throw 'Installed EXE digest mismatch' }
    $report.checks.install = $fixed

    $report.stage = 'petui-tool-info'
    $info = (Invoke-SmokeProcess $exe @('--petui','tool-info')) | ConvertFrom-Json
    if ($info.version -ne $version -or $info.target -ne 'windows-x64' -or $info.protocol -ne 2) {
        throw 'Installed PetUI CLI identity mismatch'
    }
    $report.stage = 'petui-doctor'
    $doctor = (Invoke-SmokeProcess $exe @('--petui','doctor')) | ConvertFrom-Json
    if ($doctor.ok -ne $true -or $doctor.wasmBytes -le 0) { throw 'Bundled compiler did not produce WASM' }
    $sample = Join-Path $install 'skills\petui\examples\local-notebook'
    $packed = Join-Path $root 'notebook.hachiapp'
    $report.stage = 'petui-pack'
    $null = Invoke-SmokeProcess $exe @('--petui','pack',$sample,$packed)
    $report.stage = 'petui-validate'
    $null = Invoke-SmokeProcess $exe @('--petui','validate',$packed)
    $report.stage = 'petui-install-rehearsal'
    $rehearsal = (Invoke-SmokeProcess $exe @('--petui','install-rehearsal',$packed)) | ConvertFrom-Json
    if ($rehearsal.component -ne 'local-notebook' -or $rehearsal.firstOpen -ne 'library-entry-verified') {
        throw 'PetUI temporary library rehearsal failed'
    }
    $report.checks.petui = @{target=$info.target; wasmBytes=$doctor.wasmBytes; rehearsal=$rehearsal.firstOpen}

    $report.stage = 'copy-installed-resources'
    # Use the exact shipped compiler and checker for the native regression suite.
    $testResources = Join-Path $env:RUNNER_TEMP 'petui-release-resources'
    $null = New-Item -ItemType Directory -Path $testResources
    foreach ($resource in 'authoring-runtime','skills') {
        Copy-Item -LiteralPath (Join-Path $install $resource) -Destination $testResources -Recurse
    }

    $report.guiProfiles = [ordered]@{}
    try { Invoke-SmokeGui 'synthetic' } catch {
        Write-SmokeFailure $_ -Comparison
        if ($report.stage -eq 'gui-cleanup') { throw }
    }
    Invoke-SmokeGui 'runner'
    $report.checks.gui = 'visible-responsive-window-and-native-frontend-ready'
    $report.exitRoute = $report.guiProbe.exitRoute
    $report.checks.exit = $report.guiProbe.exitRoute

} catch {
    Write-SmokeFailure $_
} finally {
    if ($installedHere) {
        try {
            $report.stage = 'uninstall'
            $null = Invoke-SmokeProcess $uninstaller @('/S') 90 $false
            $report.stage = 'uninstall-removal'
            Wait-For {
                $report.installDirPresent = Test-Path -LiteralPath $install
                $report.registrationPresent = Test-Path -LiteralPath $registry
                !$report.installDirPresent -and !$report.registrationPresent
            } 45
            $report.checks.uninstall = 'files-and-registration-removed'
        } catch {
            $report.uninstallFailed = $true
            Write-SmokeFailure $_
        }
    }
    $report.stage = 'remove-test-profile'
    try {
        if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    } catch { Write-SmokeFailure $_ }
    $report.passed = $report.checks.Contains('exit') -and $report.checks.Contains('uninstall') -and !$report.Contains('failureStage')
    $report.stage = 'finished'
    try {
        $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding utf8
    } catch {
        $report.stage = 'write-report'
        Write-SmokeFailure $_
        $report.passed = $false
    }
}
if (!$report.passed) { throw 'Windows smoke failed. See the sanitized JSON report.' }
Write-Output "Windows $version smoke passed."
