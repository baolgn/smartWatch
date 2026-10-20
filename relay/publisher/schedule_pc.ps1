# manage this user's 15-minute publisher task; installation starts disabled.
param(
    [ValidateSet('Install', 'Enable', 'Disable', 'Remove', 'Status')]
    [string]$Action = 'Status'
)

# identify one ordinary task belonging to this user and workspace.
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$userSid = $identity.User.Value
$taskName = 'XIAO Watch JSON Relay - ' + $userSid
$taskPath = '\'
$runnerPath = Join-Path $PSScriptRoot 'run_pc.ps1'
$shellPath = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$taskArguments = '-NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $runnerPath + '"'
$description = 'XIAO notification relay; publisher=' + $PSScriptRoot

# resolve Windows' normalized account names before comparing task ownership.
function Test-CurrentPrincipal([string]$PrincipalId) {
    # empty principals never identify the current user.
    if ([string]::IsNullOrWhiteSpace($PrincipalId)) {
        return $false
    }
    # accept SID strings and account names only when they resolve to this SID.
    try {
        # SID-form principals need no account lookup.
        if ($PrincipalId -match '\AS-\d+-') {
            $resolvedSid = [Security.Principal.SecurityIdentifier]::new($PrincipalId)
        }
        # Windows may return a bare local name or a qualified account name.
        else {
            $account = [Security.Principal.NTAccount]::new($PrincipalId)
            $resolvedSid = $account.Translate([Security.Principal.SecurityIdentifier])
        }
        return $resolvedSid.Value -ceq $userSid
    }
    # unmapped or malformed principals must fail closed.
    catch {
        return $false
    }
}

# distinguish an absent task from access failures or scheduler problems.
function Get-PublisherTask {
    # query the exact per-user task without suppressing operational errors.
    try {
        return Get-ScheduledTask -TaskName $taskName -TaskPath $taskPath -ErrorAction Stop
    }
    # only the provider's explicit not-found result means no installation.
    catch {
        # require both the provider identifier and missing-object category.
        if ($_.FullyQualifiedErrorId -match '\ACmdletizationQuery_NotFound(?:_TaskName)?,Get-ScheduledTask\z' -and
            $_.CategoryInfo.Category -eq [Management.Automation.ErrorCategory]::ObjectNotFound) {
            return $null
        }
        throw
    }
}

# check deployment before registration or activation without reading the channel.
function Test-PublisherFiles {
    $stateDirectory = Join-Path $PSScriptRoot '.local'
    $channelFile = Join-Path $stateDirectory 'ntfy_topic.txt'
    $pythonFile = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
    # refuse missing files and redirected local state.
    foreach ($path in @($stateDirectory, $channelFile, $runnerPath, $pythonFile)) {
        # report only a setup instruction, never channel contents.
        if (-not (Test-Path -LiteralPath $path)) {
            throw 'Publisher setup is incomplete; run prepare_pc.ps1 and check PlatformIO Python.'
        }
        $item = Get-Item -LiteralPath $path -Force
        # reject symlink/junction substitutions for managed files and storage.
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
            ($path -ne $stateDirectory -and $item.PSIsContainer) -or
            ($path -eq $stateDirectory -and -not $item.PSIsContainer)) {
            throw 'Publisher setup paths must be ordinary local files and directories.'
        }
    }
    # require a plausible bounded file before enabling background publication.
    if ((Get-Item -LiteralPath $channelFile).Length -lt 32 -or (Get-Item -LiteralPath $channelFile).Length -gt 128) {
        throw 'The local channel file is invalid; run prepare_pc.ps1.'
    }
}

# locate only the exact per-user task; never enumerate unrelated task bodies.
$task = Get-PublisherTask

# refuse to change a name collision or a task belonging to another checkout.
if ($null -ne $task) {
    $actions = @($task.Actions)
    # match the principal, workspace marker and exact one-shot launcher.
    if ($task.Description -cne $description -or
        -not (Test-CurrentPrincipal $task.Principal.UserId) -or
        $task.Principal.LogonType -ne 'Interactive' -or $task.Principal.RunLevel -ne 'Limited' -or
        $actions.Count -ne 1 -or $actions[0].Execute -ine $shellPath -or
        $actions[0].Arguments -cne $taskArguments -or $actions[0].WorkingDirectory -ine $PSScriptRoot) {
        throw 'A different task uses this name; no task was changed.'
    }
}

# register a disabled task without starting its action or exposing credentials.
if ($Action -eq 'Install') {
    Test-PublisherFiles
    # repeated installation preserves the existing enabled/disabled state.
    if ($null -eq $task) {
        $taskAction = New-ScheduledTaskAction -Execute $shellPath -Argument $taskArguments -WorkingDirectory $PSScriptRoot
        # run every 15 minutes and at login using the current user's session.
        $periodic = New-ScheduledTaskTrigger -Once -At ((Get-Date).AddMinutes(15)) -RepetitionInterval (New-TimeSpan -Minutes 15)
        $logon = New-ScheduledTaskTrigger -AtLogOn -User $userSid
        $principal = New-ScheduledTaskPrincipal -UserId $userSid -LogonType Interactive -RunLevel Limited
        # recover missed runs without overlap; stop attempts after two minutes.
        $settings = New-ScheduledTaskSettingsSet -Disable -StartWhenAvailable -MultipleInstances IgnoreNew `
            -ExecutionTimeLimit (New-TimeSpan -Minutes 2) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
        $definition = New-ScheduledTask -Action $taskAction -Trigger @($periodic, $logon) `
            -Principal $principal -Settings $settings -Description $description
        $task = Register-ScheduledTask -TaskName $taskName -TaskPath $taskPath -InputObject $definition
        Write-Output 'Installed disabled. No publisher was started; use -Action Enable when ready.'
    }
    # avoid silently changing an already configured schedule.
    else {
        Write-Output 'The publisher task is already installed; its state was preserved.'
    }
}
# enable scheduled publication only through this explicit action.
elseif ($Action -eq 'Enable') {
    # require installation before activation.
    if ($null -eq $task) {
        throw 'Run schedule_pc.ps1 -Action Install first.'
    }
    Test-PublisherFiles
    $task = Enable-ScheduledTask -TaskName $taskName -TaskPath $taskPath
    Write-Output 'Scheduled publication enabled; a due or missed run can start immediately.'
}
# stop future and currently running publications when disabling or removing.
elseif ($Action -eq 'Disable' -or $Action -eq 'Remove') {
    # an absent task requires no action or configuration deletion.
    if ($null -ne $task) {
        $wasRunning = $task.State -eq 'Running'
        $task = Disable-ScheduledTask -TaskName $taskName -TaskPath $taskPath
        # stopping an inactive task can fail on some scheduler versions.
        if ($wasRunning -or $task.State -eq 'Running') {
            Stop-ScheduledTask -TaskName $taskName -TaskPath $taskPath
        }
        # remove only this task registration, preserving the channel and logs.
        if ($Action -eq 'Remove') {
            Unregister-ScheduledTask -TaskName $taskName -TaskPath $taskPath -Confirm:$false
            $task = $null
            Write-Output 'Publisher task removed; local channel and logs were preserved.'
        }
        # confirm that scheduled attempts have stopped.
        else {
            Write-Output 'Publisher task disabled and any running instance stopped.'
        }
    }
}

# report scheduler state without treating a completed POST as watch delivery.
if ($null -eq $task) {
    Write-Output 'Publisher task is not installed.'
}
# display the bounded operational details needed for a local check.
else {
    $task = Get-PublisherTask
    $info = Get-ScheduledTaskInfo -TaskName $taskName -TaskPath $taskPath
    # expose timing and exit status without reading channel or message data.
    [PSCustomObject]@{
        TaskName = $taskName
        State = $task.State
        Enabled = $task.Settings.Enabled
        LastRun = $info.LastRunTime
        NextRun = $info.NextRunTime
        LastResult = $info.LastTaskResult
        Log = (Join-Path $PSScriptRoot '.local\latest.log')
    }
}
