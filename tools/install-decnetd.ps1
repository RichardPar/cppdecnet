<#
install-decnetd.ps1 -- make this Windows machine a DECnet node that starts
when you log in: asks who the node is and how it reaches the network,
writes %LOCALAPPDATA%\cppdecnet\decnetd.conf, installs decnetd, and runs it
from a Task Scheduler task.  The Windows counterpart of install-decnetd.sh.

    tools\install-decnetd.cmd                 ask, then install and start
    tools\install-decnetd.cmd -DryRun DIR     ask, then write the files to
                                              DIR and show what would be done
    tools\install-decnetd.cmd -Uninstall      stop and remove the task

Run it as yourself, not as administrator: decnetd runs as you, from a task
that starts when you log in.  Only a firewall rule for a listening circuit
needs administrator rights, and it asks for those itself.  Running it again
replaces the configuration (the old one is kept as a .bak).
#>

[CmdletBinding ()]
param (
    [switch] $Uninstall,
    [string] $DryRun = "",
    [string] $Prefix = (Join-Path $env:LOCALAPPDATA "Programs\cppdecnet"),
    [switch] $Help
)

$ErrorActionPreference = "Stop"

$repo     = Split-Path -Parent $PSScriptRoot
$confdir  = Join-Path $env:LOCALAPPDATA "cppdecnet"
$conf     = Join-Path $confdir "decnetd.conf"
$logfile  = Join-Path $confdir "decnetd.log"
$taskname = "cppdecnet decnetd"
# Where decnetd puts its API socket when the api line names none, and where
# the PathNoWorks tools look: decnetapi.sock in the temporary directory.
$socket   = Join-Path ([IO.Path]::GetTempPath()) "decnetapi.sock"
$builddir = Join-Path $repo "build\msvc"
$binaries = "decnetd.exe", "dnfal.exe", "dnping.exe"

if ($Help) {
    Get-Content $PSCommandPath -TotalCount 16 | Select-Object -Skip 1
    exit 0
}

function Say ([string] $text = "") { Write-Host $text }
function Section ([string] $text) { Write-Host ""; Write-Host $text -ForegroundColor White }
function Die ([string] $text) {
    Write-Host "install-decnetd.ps1: $text" -ForegroundColor Red
    exit 1
}

# Run-Step DESCRIPTION SCRIPTBLOCK -- do it, or with -DryRun just say it.
function Run-Step ([string] $what, [scriptblock] $do) {
    if ($DryRun) { Say "  + $what" } else { & $do }
}

# Put-File PATH TEXT -- UTF-8 without a BOM and with LF line ends, which is
# how decnetd likes its configuration (with -DryRun, into the DryRun folder).
function Put-File ([string] $path, [string] $text) {
    if ($DryRun) {
        $path2 = Join-Path $DryRun (Split-Path -Leaf $path)
        Say "  + write $path (in $DryRun)"
        $path = $path2
    } else {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    }
    [IO.File]::WriteAllText($path, ($text -replace "`r`n", "`n"),
                             (New-Object Text.UTF8Encoding $false))
}

# ---------------------------------------------------------------- questions

# Ask PROMPT DEFAULT [CHECK] -- read a value; CHECK is a scriptblock that
# says whether it will do, printing why not.
function Ask ([string] $prompt, [string] $default = "", [scriptblock] $check = $null) {
    while ($true) {
        if ($default) { $text = "$prompt [$default]" } else { $text = $prompt }
        $reply = Read-Host $text
        if ($null -eq $reply) { Die "no answer to `"$prompt`"" }
        $reply = $reply.Trim()
        if (-not $reply) { $reply = $default }
        if (-not $check -or (& $check $reply)) { return $reply }
    }
}

function Yes-No ([string] $prompt, [string] $default) {
    if ($default -eq "y") { $hint = "Y/n" } else { $hint = "y/N" }
    $reply = Read-Host "$prompt [$hint]"
    if ($null -eq $reply) { Die "no answer to `"$prompt`"" }
    if (-not $reply.Trim()) { $reply = $default }
    return $reply.Trim() -match '^[Yy]'
}

$validName = {
    param ($v)
    if ($v -match '^[A-Za-z0-9]{1,6}$' -and $v -match '[A-Za-z]') { return $true }
    Say "  A node name is one to six letters and digits, with at least one letter."
    return $false
}

$validAddress = {
    param ($v)
    if ($v -match '^(\d+)\.(\d+)$') {
        $a = [int] $Matches[1]; $n = [int] $Matches[2]
        if ($a -ge 1 -and $a -le 63 -and $n -ge 1 -and $n -le 1023) { return $true }
    }
    Say "  An address is area.node: area 1 to 63, node 1 to 1023, like 29.151."
    return $false
}

$validAddressOrNone = { param ($v) (-not $v) -or $v -eq "-" -or (& $validAddress $v) }
$validNameOrNone    = { param ($v) (-not $v) -or $v -eq "-" -or (& $validName $v) }

$validPort = {
    param ($v)
    if ($v -match '^\d+$' -and [int] $v -ge 1 -and [int] $v -le 65535) { return $true }
    Say "  A port is a number from 1 to 65535."
    return $false
}

$validHost = {
    param ($v)
    if ($v -match '^[A-Za-z0-9.:_-]+$') { return $true }
    Say "  Give the peer's IP address or host name."
    return $false
}

$validHostOrAny = { param ($v) (-not $v) -or $v -eq "any" -or (& $validHost $v) }

$validMinutes = {
    param ($v)
    if ($v -match '^\d+$' -and [int] $v -ge 1) { return $true }
    Say "  A number of minutes, at least 1."
    return $false
}

# Tasks other than ours that start decnetd, and decnetd processes not
# started from our copy: either would put the node on the network twice.
function Other-Tasks {
    Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object {
        $_.TaskName -ne $taskname -and
        ($_.Actions | Where-Object { "$($_.Execute) $($_.Arguments)" -match 'decnetd' })
    }
}

function Other-Processes {
    $ours = Join-Path $Prefix "decnetd.exe"
    Get-Process decnetd -ErrorAction SilentlyContinue | Where-Object { $_.Path -ne $ours }
}

# ------------------------------------------------------------------ uninstall

if ($Uninstall) {
    Section "Removing the decnetd task"
    if (Get-ScheduledTask -TaskName $taskname -ErrorAction SilentlyContinue) {
        Run-Step "stop and remove the task `"$taskname`"" {
            Stop-ScheduledTask -TaskName $taskname -ErrorAction SilentlyContinue
            Unregister-ScheduledTask -TaskName $taskname -Confirm:$false
        }
    }
    $ours = Join-Path $Prefix "decnetd.exe"
    Run-Step "stop decnetd ($ours)" {
        Get-Process decnetd -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $ours } | Stop-Process -Force
        Start-Sleep -Seconds 1
    }
    Run-Step "remove $Prefix" {
        Remove-Item -Recurse -Force $Prefix -ErrorAction SilentlyContinue
    }
    if (Get-NetFirewallRule -DisplayName "cppdecnet decnetd" -ErrorAction SilentlyContinue) {
        Say "  There's a firewall rule `"cppdecnet decnetd`"; remove it, as administrator, with:"
        Say "    Remove-NetFirewallRule -DisplayName `"cppdecnet decnetd`""
    }
    Say "Done.  The configuration and log are still in $confdir; remove them"
    Say "yourself if you're sure."
    exit 0
}

# ----------------------------------------------------------------- the node

if ($DryRun) { New-Item -ItemType Directory -Force -Path $DryRun | Out-Null }

Section "This node"
Say "  An endnode is right for a desktop: one circuit, to a router that looks"
Say "  after the rest of the network.  A router joins circuits together and"
Say "  carries other nodes' traffic."
Say "    1) endnode"
Say "    2) level 1 router (routes within its area)"
Say "    3) level 2 router (an area router)"
$pickType = {
    param ($v)
    if ($v -in "1", "2", "3", "endnode", "l1router", "l2router") { return $true }
    Say "  1, 2 or 3."
    return $false
}
switch (Ask "Node type" "1" $pickType) {
    { $_ -in "1", "endnode" }  { $ntype = "endnode" }
    { $_ -in "2", "l1router" } { $ntype = "l1router" }
    default                    { $ntype = "l2router" }
}

$name = (Ask "Node name" "" $validName).ToUpper()
Say "  On HECnet, your area's coordinator gives you an address; elsewhere, pick"
Say "  one nobody on your network is using."
$address = Ask "Node address (area.node)" "" $validAddress

# ------------------------------------------------------------------ circuits

$circuits   = New-Object System.Collections.Generic.List[string]
$peerNodes  = New-Object System.Collections.Generic.List[string]
$listenPorts = New-Object System.Collections.Generic.List[int]
$nMul = 0

function Add-Circuit {
    Section "Circuit $($script:circuits.Count + 1)"
    Say "    1) Multinet, connecting to a peer that listens (the usual for an endnode)"
    Say "    2) Multinet, listening for a peer that connects to us"
    Say "  (DECnet straight on an Ethernet needs a packet driver, which decnetd"
    Say "  doesn't use on Windows: Multinet only, here.)"
    $pickKind = {
        param ($v)
        if ($v -in "1", "2") { return $true }
        Say "  1 or 2."
        return $false
    }
    $kind = Ask "How does it reach the network" "1" $pickKind
    if ($kind -eq "1") {
        $peer = Ask "The peer's IP address or host name" "" $validHost
        $port = Ask "The port it listens on" "7100" $validPort
        $dev = "Multinet ${peer}:${port}:connect"
    } else {
        Say "  Only the peer's address may connect.  Leave it empty (or say `"any`")"
        Say "  to take a connection from anywhere."
        $peer = Ask "The peer's IP address or host name" "" $validHostOrAny
        if ($peer -eq "any") { $peer = "" }
        $port = Ask "The port to listen on" "7100" $validPort
        $dev = "Multinet ${peer}:${port}:listen"
        $script:listenPorts.Add([int] $port)
    }
    $script:circuits.Add("circuit mul-$($script:nMul) $dev --t3 15")
    $script:nMul++
    Say "  Naming the node at the other end lets you use its name.  Leave these"
    Say "  empty if you don't know them."
    $peerAddr = Ask "Its DECnet address" "" $validAddressOrNone
    $peerName = Ask "Its DECnet name" "" $validNameOrNone
    if ($peerAddr -and $peerAddr -ne "-" -and $peerName -and $peerName -ne "-") {
        $script:peerNodes.Add("node $peerAddr $($peerName.ToUpper())")
    }
}

Add-Circuit
if ($ntype -ne "endnode") {
    while (Yes-No "Add another circuit?" "n") { Add-Circuit }
}

# ------------------------------------------------------------ disconnecting

$onDemand = ""
Section "Staying connected"
if ($ntype -eq "endnode") {
    Say "  A desktop node can stay off the network until a PathNoWorks program"
    Say "  needs it: the circuit comes up when the first one connects, and goes"
    Say "  down once none has been connected for a while."
    if (Yes-No "Disconnect when nothing has used it for a while?" "n") {
        $minutes = Ask "After how many minutes" "120" $validMinutes
        $onDemand = " --on-demand --idle $([int] $minutes * 60)"
    }
} else {
    Say "  A router stays connected: other nodes rely on it to carry their traffic."
}

# ---------------------------------------------------------- the rest of it

Section "Names and monitoring"
$hecnet = Yes-No "Keep HECnet's list of node names, fetched weekly from MIM?" "n"
Say "  Nodes nobody has named can still be asked: the neighbours for the names"
Say "  they know, and any node you connect to for its own."
$learn = Yes-No "Learn node names from the network?" "y"

$http = ""
if (Yes-No "Serve the monitoring web pages?" "n") {
    $http = Ask "Port" "8102" $validPort
}

Say "  decnetd runs as you, $env:USERNAME, from a task that starts when you log in,"
Say "  and its API socket is yours: the PathNoWorks tools you run can use it"
Say "  straight away."

# -------------------------------------------------------------- the files

# Paths in the configuration: quoted, since a Windows user name, and so its
# profile's path, may have a space in it; forward slashes, which decnetd
# takes on Windows too.
function Conf-Path ([string] $p) { '"' + ($p -replace '\\', '/') + '"' }

$timestamp = Get-Date -Format "yyyy-MM-dd HH:mm"
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("# decnetd.conf -- written by install-decnetd.ps1, $timestamp.")
$lines.Add("# Edit freely, then restart decnetd: Stop-ScheduledTask and")
$lines.Add("# Start-ScheduledTask `"$taskname`" (or log out and in again).")
$lines.Add("")
$lines.Add("routing $address --type $ntype")
$lines.Add("")
$lines.Add("node $address $name")
foreach ($l in $peerNodes) { $lines.Add($l) }
if ($hecnet) { $lines.Add("node @hecnet --cache $(Conf-Path (Join-Path $confdir 'hecnet.dat'))") }
if ($learn)  { $lines.Add("node @neighbours") }
$lines.Add("")
foreach ($l in $circuits) { $lines.Add($l) }
$lines.Add("")
$lines.Add("# The socket the PathNoWorks tools look for, decnetapi.sock in %TEMP%.")
$lines.Add("# Whoever can open it acts as this node.")
$lines.Add("api$onDemand")
if ($http) {
    $lines.Add("")
    $lines.Add("# Monitoring pages: on all interfaces, with no login.")
    $lines.Add("http --http-port $http")
}
$config = ($lines -join "`n") + "`n"

Section "What will be installed"
foreach ($l in $lines) { Say "    $l" }
Say ""
Say "  as $conf, with decnetd in $Prefix, run by the"
Say "  task `"$taskname`" when you log in, as $env:USERNAME."
if (-not (Yes-No "Go ahead?" "y")) { Say "Nothing done."; exit 0 }

# ------------------------------------------------------------- installing

Section "Installing"

# Build, if there's nothing built: Visual Studio's C++ build tools, with
# their CMake and Ninja.
$built = Join-Path $builddir "decnetd.exe"
if (-not (Test-Path $built)) {
    Say "  decnetd isn't built yet; building it."
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        Die "no Visual Studio here to build with.  Install the Build Tools for Visual Studio 2022 (with `"Desktop development with C++`"), or build cppdecnet first."
    }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { Die "Visual Studio has no C++ build tools.  Add `"Desktop development with C++`" to it." }
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
    $cmd = "call `"$vcvars`" >nul && cmake -S `"$repo`" -B `"$builddir`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCPPDECNET_TESTS=OFF && cmake --build `"$builddir`" --target decnetd dnfal dnping"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { Die "the build failed" }
}
foreach ($b in $binaries) {
    if (-not (Test-Path (Join-Path $builddir $b))) { Die "$b didn't build" }
}

# One node per address: another decnetd would be the same node twice.
$otherTasks = @(Other-Tasks)
$otherProcs = @(Other-Processes)
$start = $true
if ($otherTasks.Count -or $otherProcs.Count) {
    Say ""
    Write-Host "  decnetd is already set up or running here, not from this installer:" -ForegroundColor Red
    foreach ($t in $otherTasks) { Say "    task `"$($t.TaskName)`"" }
    foreach ($p in $otherProcs) { Say "    process $($p.Id): $($p.Path)" }
    Say "  Running both would put the node on the network twice."
    if (Yes-No "Stop it, and disable those tasks?" "y") {
        foreach ($t in $otherTasks) {
            Run-Step "disable the task `"$($t.TaskName)`"" {
                Stop-ScheduledTask -TaskName $t.TaskName -ErrorAction SilentlyContinue
                Disable-ScheduledTask -TaskName $t.TaskName | Out-Null
            }
        }
        foreach ($p in $otherProcs) {
            Run-Step "stop process $($p.Id)" { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
        }
    } else {
        $start = $false
    }
}

# Our own copy, if it's running, holds its files open.
Run-Step "stop the task `"$taskname`", if it's running" {
    if (Get-ScheduledTask -TaskName $taskname -ErrorAction SilentlyContinue) {
        Stop-ScheduledTask -TaskName $taskname -ErrorAction SilentlyContinue
    }
    Get-Process decnetd -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq (Join-Path $Prefix "decnetd.exe") } | Stop-Process -Force
    Start-Sleep -Seconds 1
}

Run-Step "copy $($binaries -join ', ') to $Prefix" {
    New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
    foreach ($b in $binaries) { Copy-Item -Force (Join-Path $builddir $b) $Prefix }
}

if (-not $DryRun -and (Test-Path $conf)) {
    $backup = "$conf.$(Get-Date -Format yyyyMMdd-HHmmss).bak"
    Copy-Item $conf $backup
    Say "  The old configuration is kept as $backup."
}
Put-File $conf $config

# The task: at logon, as this user, with no window (conhost --headless runs
# a console program without one), no time limit, and a few retries should
# it fail to start.
$decnetd = Join-Path $Prefix "decnetd.exe"
$taskArgs = "--headless `"$decnetd`" --log-level info --log-file `"$logfile`" `"$conf`""
Run-Step "register the task `"$taskname`": conhost.exe $taskArgs, at logon" {
    $user = "$env:USERDOMAIN\$env:USERNAME"
    $action = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\conhost.exe" `
                                      -Argument $taskArgs -WorkingDirectory $confdir
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) `
                    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
                    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                    -MultipleInstances IgnoreNew
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName $taskname -Force -Action $action -Trigger $trigger `
        -Settings $settings -Principal $principal `
        -Description "DECnet node $name ($address), cppdecnet decnetd" | Out-Null
}

# A listening circuit wants a firewall rule to let its peer in.  That needs
# administrator rights, so ask, and ask Windows for them.
if ($listenPorts.Count) {
    Say ""
    Say "  Windows Firewall blocks the peer from connecting to a listening circuit"
    Say "  (port $($listenPorts -join ', ')) unless it's allowed."
    if (Yes-No "Allow it?  (Windows will ask for administrator rights.)" "y") {
        $ports = $listenPorts -join ","
        $rule = "Remove-NetFirewallRule -DisplayName 'cppdecnet decnetd' -ErrorAction SilentlyContinue; " +
                "New-NetFirewallRule -DisplayName 'cppdecnet decnetd' -Direction Inbound -Protocol TCP " +
                "-LocalPort $ports -Program '$decnetd' -Action Allow -Profile Private,Domain | Out-Null"
        Run-Step "as administrator: $rule" {
            try {
                Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden `
                    -ArgumentList "-NoProfile", "-Command", $rule
            } catch {
                Write-Host "  Not allowed.  To add it later, as administrator:" -ForegroundColor Red
                Say "    $rule"
            }
        }
    }
}

if ($start) {
    Run-Step "start the task `"$taskname`"" { Start-ScheduledTask -TaskName $taskname }
} else {
    Say "  Set up for your next logon, not started.  Start it once the other one"
    Say "  has stopped: Start-ScheduledTask `"$taskname`""
}

if ($DryRun) { Section "Dry run: nothing changed.  The files are in $DryRun."; exit 0 }

# ---------------------------------------------------------------- checking

if ($start) {
    Section "Checking"
    for ($i = 0; $i -lt 20 -and -not (Test-Path $socket); $i++) { Start-Sleep -Milliseconds 500 }
    $running = Get-Process decnetd -ErrorAction SilentlyContinue |
               Where-Object { $_.Path -eq $decnetd }
    if ($running -and (Test-Path $socket)) {
        Write-Host "  OK " -ForegroundColor Green -NoNewline
        Say "decnetd is running as $name ($address), and its API is at $socket"
    } else {
        Write-Host "  decnetd didn't start properly.  The log says:" -ForegroundColor Red
        if (Test-Path $logfile) { Get-Content $logfile -Tail 20 | ForEach-Object { Say "    $_" } }
        exit 1
    }
}

Section "Done"
Say "  Configuration:  $conf"
Say "  Task:           Start-ScheduledTask / Stop-ScheduledTask `"$taskname`""
Say "  Log:            Get-Content -Wait `"$logfile`""
Say "  Remove it with: $PSScriptRoot\install-decnetd.cmd -Uninstall"
