# manage one hidden local browser relay without installing a service.
param(
    [ValidateSet('Start', 'Stop', 'Status')]
    [string]$Action = 'Status',
    [string]$PythonPath
)

$ErrorActionPreference = 'Stop'

# reject redirected state, executables and log files before accessing them.
function Assert-OrdinaryPath([string]$Path, [bool]$Directory = $false) {
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $item.PSIsContainer -ne $Directory) {
        throw 'Relay paths must be ordinary files and directories.'
    }
    return $item.FullName
}

# use the existing standalone PlatformIO interpreter or an explicit executable.
function Resolve-RelayPython([string]$Requested) {
    $candidates = @($Requested)
    if ([string]::IsNullOrWhiteSpace($Requested)) {
        $candidates = @((Join-Path $env:USERPROFILE '.platformio\python3\python.exe'),
                        (Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'))
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $resolved = Assert-OrdinaryPath $candidate
            if ([IO.Path]::GetFileName($resolved) -notin @('python.exe', 'pythonw.exe')) {
                throw 'PythonPath must point to python.exe or pythonw.exe.'
            }
            return $resolved
        }
    }
    throw 'PlatformIO Python was not found; supply -PythonPath with its full executable path.'
}

# read one process identity without displaying its command line.
function Get-RelayProcess([int]$ProcessId) {
    return Get-CimInstance Win32_Process -Filter ('ProcessId = ' + $ProcessId)
}

# represent creation time independently of the local timezone.
function Get-ProcessStamp($Process) {
    return ([datetime]$Process.CreationDate).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
}

# compare the complete known invocation, including its executable argument.
function Test-RelayCommand($Process, [string]$Python, [string]$Arguments) {
    if ($null -eq $Process -or [string]::IsNullOrWhiteSpace($Process.CommandLine)) { return $false }
    $command = $Process.CommandLine.TrimEnd()
    return $command -ceq ('"' + $Python + '" ' + $Arguments) -or $command -ceq ($Python + ' ' + $Arguments)
}

# refuse PID reuse or a record for another checkout before stopping anything.
function Test-RecordedProcess($Process, $Record, [string]$Server) {
    return $null -ne $Process -and $Record.server_path -ieq $Server -and
           [int]$Record.process_id -eq [int]$Process.ProcessId -and
           $Record.created_utc -ceq (Get-ProcessStamp $Process) -and
           $Record.executable_path -ieq $Process.ExecutablePath -and
           $Record.command_line -ceq $Process.CommandLine -and
           (Test-RelayCommand $Process $Record.launcher_path ('-B -u "' + $Server + '" --host 127.0.0.1 --port 8765 --weather'))
}

# save only a process whose full invocation belongs to this deployment.
function Save-RelayRecord($Process, [string]$Python, [string]$Arguments, [string]$Server, [string]$Path) {
    if (-not (Test-RelayCommand $Process $Python $Arguments)) {
        throw 'The started process identity could not be verified.'
    }
    $record = [ordered]@{ version = 1; process_id = [int]$Process.ProcessId
        created_utc = Get-ProcessStamp $Process; executable_path = $Process.ExecutablePath
        command_line = $Process.CommandLine; server_path = $Server; launcher_path = $Python }
    [IO.File]::WriteAllText($Path, ($record | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
}
# distinguish the configured listener from any other use of the same port.
function Get-RelayListeners {
    return @(Get-NetTCPConnection -State Listen | Where-Object { $_.LocalPort -eq 8765 })
}

# serialize manager actions and retain only nonsecret process metadata.
function Invoke-RelayManager([string]$SelectedAction, [string]$RequestedPython, [string]$Root = $PSScriptRoot) {
    $rootPath = Assert-OrdinaryPath $Root $true
    $serverPath = Assert-OrdinaryPath (Join-Path $rootPath 'server.py')
    if ($serverPath.Contains('"') -or $serverPath.Contains("`r") -or $serverPath.Contains("`n")) {
        throw 'The server path contains unsupported command-line characters.'
    }
    $stateDirectory = Join-Path $rootPath '.local'
    if (-not (Test-Path -LiteralPath $stateDirectory)) {
        New-Item -ItemType Directory -Path $stateDirectory | Out-Null
    }
    $stateDirectory = Assert-OrdinaryPath $stateDirectory $true
    $recordPath = Join-Path $stateDirectory 'server_process.json'
    $lockPath = Join-Path $stateDirectory 'manager.lock'
    $stdoutPath = Join-Path $stateDirectory 'server_stdout.log'
    $stderrPath = Join-Path $stateDirectory 'server_stderr.log'
    foreach ($path in @($recordPath, $lockPath, $stdoutPath, $stderrPath)) {
        if (Test-Path -LiteralPath $path) {
            $null = Assert-OrdinaryPath $path
        }
    }
    $managerLock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate,
                                 [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $record, $managed = $null, $null
        if (Test-Path -LiteralPath $recordPath) {
            if ((Get-Item -LiteralPath $recordPath).Length -gt 8192) {
                throw 'The process record is invalid; no process was changed.'
            }
            $record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
            $createdUtc = [datetime]::MinValue
            # validate metadata before deciding whether a process record is stale.
            if ($record.version -isnot [int] -or $record.version -ne 1 -or
                $record.process_id -isnot [int] -or $record.process_id -le 0 -or
                $record.server_path -isnot [string] -or $record.server_path -ine $serverPath -or
                $record.command_line -isnot [string] -or [string]::IsNullOrWhiteSpace($record.command_line) -or
                $record.executable_path -isnot [string] -or [string]::IsNullOrWhiteSpace($record.executable_path) -or
                $record.launcher_path -isnot [string] -or [string]::IsNullOrWhiteSpace($record.launcher_path) -or
                $record.created_utc -isnot [string] -or
                -not [datetime]::TryParseExact($record.created_utc, "yyyy-MM-ddTHH:mm:ss.fff'Z'",
                    [Globalization.CultureInfo]::InvariantCulture,
                    [Globalization.DateTimeStyles]::AssumeUniversal, [ref]$createdUtc)) {
                throw 'The process record is invalid; no process was changed.'
            }
            $managed = Get-RelayProcess $record.process_id
            # a different creation time means the recorded process has ended.
            if ($null -ne $managed -and $record.created_utc -cne (Get-ProcessStamp $managed)) {
                $managed = $null
            # a matching creation time still requires the complete saved identity.
            } elseif ($null -ne $managed -and -not (Test-RecordedProcess $managed $record $serverPath)) {
                throw 'The recorded PID belongs to a different process; no process was changed.'
            }
        }

        # report missing or running instances without reading relay credentials.
        if ($SelectedAction -eq 'Status') {
            if ($null -eq $managed) {
                Write-Output 'The managed browser relay is stopped.'
            } else {
                $listening = @(Get-RelayListeners | Where-Object { $_.OwningProcess -eq $managed.ProcessId }).Count -gt 0
                Write-Output ('Browser relay PID {0}; loopback listener ready: {1}.' -f $managed.ProcessId, $listening)
            }
            return
        }

        # bind the process handle and recheck creation time immediately before stopping.
        if ($SelectedAction -eq 'Stop') {
            if ($null -ne $managed) {
                $handle = Get-Process -Id $managed.ProcessId
                $null = $handle.Handle
                if ($handle.StartTime.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ') -cne $record.created_utc) {
                    throw 'The process changed before stopping; no process was stopped.'
                }
                Stop-Process -InputObject $handle -ErrorAction Stop
                $handle.WaitForExit(5000) | Out-Null
            }
            if (Test-Path -LiteralPath $recordPath) {
                Remove-Item -LiteralPath $recordPath
            }
            Write-Output 'The managed browser relay is stopped.'
            return
        }

        # avoid duplicate managers, unmanaged instances and occupied listeners.
        if ($null -ne $managed) {
            Write-Output ('Browser relay is already running with PID {0}.' -f $managed.ProcessId)
            return
        }
        $otherInstances = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe' OR Name = 'pythonw.exe'" |
            Where-Object { $_.CommandLine -and $_.CommandLine.IndexOf($serverPath, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
        if ($otherInstances.Count -gt 0 -or @(Get-RelayListeners).Count -gt 0) {
            throw 'Another relay instance or listener uses this deployment; no process was started.'
        }
        $python = Resolve-RelayPython $RequestedPython
        $arguments = '-B -u "' + $serverPath + '" --host 127.0.0.1 --port 8765 --weather'
        $started = Start-Process -FilePath $python -ArgumentList $arguments -WorkingDirectory $rootPath `
            -WindowStyle Hidden -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
        # record the launcher immediately so a delayed listener remains manageable.
        $launchIdentity = Get-RelayProcess $started.Id
        Save-RelayRecord $launchIdentity $python $arguments $serverPath $recordPath
        $deadline = [datetime]::UtcNow.AddSeconds(10)
        $service = $null

        # identify the listener, including a virtual-environment launcher's child.
        do {
            $started.Refresh()
            if ($started.HasExited) { break }
            foreach ($listener in @(Get-RelayListeners)) {
                $candidate = Get-RelayProcess $listener.OwningProcess
                if (([int]$candidate.ProcessId -eq $started.Id -or [int]$candidate.ParentProcessId -eq $started.Id) -and
                    (Test-RelayCommand $candidate $python $arguments)) {
                    $service = $candidate
                    break
                }
            }
            if ($null -ne $service) { break }
            Start-Sleep -Milliseconds 100
        } while ([datetime]::UtcNow -lt $deadline)
        if ($null -eq $service) {
            # retain a verified child record when a virtual-environment launcher waits.
            $children = @(Get-CimInstance Win32_Process -Filter ('ParentProcessId = ' + $started.Id) |
                Where-Object { Test-RelayCommand $_ $python $arguments })
            if ($children.Count -eq 1) {
                Save-RelayRecord $children[0] $python $arguments $serverPath $recordPath
            }
            throw 'The relay did not open its listener; use Status or Stop and inspect the local server logs.'
        }
        Save-RelayRecord $service $python $arguments $serverPath $recordPath

        Write-Output ('Browser relay started with PID {0} at http://127.0.0.1:8765.' -f $service.ProcessId)
    } finally {
        $managerLock.Dispose()
    }
}

Invoke-RelayManager $Action $PythonPath
