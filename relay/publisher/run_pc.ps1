# run one weather publication with local state and bounded, rotated logs.
param(
    [switch]$DryRun,
    [ValidateSet('binary', 'json')]
    [string]$WireFormat = 'binary',
    [string]$PythonPath = (Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe')
)

# validate local files before starting a child process.
$ErrorActionPreference = 'Stop'
$localDirectory = Join-Path $PSScriptRoot '.local'
$channelPath = Join-Path $localDirectory 'ntfy_topic.txt'
$logPath = Join-Path $localDirectory 'latest.log'
$previousLogPath = Join-Path $localDirectory 'previous.log'
$publisherPath = Join-Path $PSScriptRoot 'publisher.py'
$utf8 = New-Object Text.UTF8Encoding($false)

# require explicit preparation; never silently create a new subscription.
if (-not (Test-Path -LiteralPath $localDirectory -PathType Container)) {
    throw 'Run prepare_pc.ps1 first.'
}

# prevent local state or log redirection outside this folder.
foreach ($path in @($localDirectory, $channelPath, $logPath, $previousLogPath)) {
    # inspect only existing paths; new logs are ordinary files.
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path -Force
        # reject reparse points and directories masquerading as state files.
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or ($path -ne $localDirectory -and $item.PSIsContainer)) {
            throw 'Publisher state paths must be ordinary local files and directories.'
        }
    }
}

# require the small channel file and an installed Python executable.
if (-not (Test-Path -LiteralPath $channelPath -PathType Leaf) -or (Get-Item -LiteralPath $channelPath).Length -gt 128) {
    throw 'The local channel file is missing or invalid; run prepare_pc.ps1.'
}
# reject a missing interpreter before preparing the child process.
if (-not (Test-Path -LiteralPath $PythonPath -PathType Leaf)) {
    throw 'Python was not found; provide its full executable path with -PythonPath.'
}

# keep the channel out of process arguments, logs and the parent environment.
$channel = [IO.File]::ReadAllText($channelPath).Trim()
# require the same channel syntax as the Python publisher.
if ($channel -cnotmatch '\A[A-Za-z0-9_-]{32,64}\z') {
    throw 'The local channel file is invalid.'
}
# configure a hidden child with redirected output and a private environment.
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $PythonPath
$start.Arguments = '-B "' + $publisherPath + '" --weather --wire-format ' + $WireFormat
$start.WorkingDirectory = $PSScriptRoot
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.EnvironmentVariables['NTFY_TOPIC'] = $channel

# only the explicit dry-run switch suppresses this launcher's one-shot send.
if (-not $DryRun) {
    $start.Arguments += ' --send'
}

# drain both output streams concurrently to avoid a pipe deadlock.
$process = New-Object Diagnostics.Process
$process.StartInfo = $start
# run once and retain a bounded success, failure or timeout result.
try {
    [void]$process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    # enforce an outer deadline in addition to Python's per-request limits.
    if (-not $process.WaitForExit(90000)) {
        $process.Kill()
        $process.WaitForExit()
        $exitCode = 124
        $result = 'error: publisher exceeded the 90-second process deadline'
    }
    # retain the child's success/failure status for Task Scheduler.
    else {
        $exitCode = $process.ExitCode
        $result = ($stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()).Trim()
    }
}
# close process handles after success, failure or timeout.
finally {
    $process.Dispose()
}

# retain at most two short logs and remove any unexpected channel disclosure.
$result = $result.Replace($channel, '[channel redacted]')
# keep only the bounded tail of unexpected child output.
if ($result.Length -gt 16000) {
    $result = $result.Substring($result.Length - 16000)
}
$record = [DateTime]::UtcNow.ToString('o') + ' exit=' + $exitCode + [Environment]::NewLine + $result + [Environment]::NewLine

# rotate the prior bounded log without deleting unrelated files.
if (Test-Path -LiteralPath $logPath -PathType Leaf) {
    $prior = [IO.File]::ReadAllText($logPath)
    # cap a modified or unexpectedly large old log before retaining it.
    if ($prior.Length -gt 16000) {
        $prior = $prior.Substring($prior.Length - 16000)
    }
    [IO.File]::WriteAllText($previousLogPath, $prior.Replace($channel, '[channel redacted]'), $utf8)
}
# persist the current result and return its exit status to Task Scheduler.
[IO.File]::WriteAllText($logPath, $record, $utf8)
Write-Output $result
exit $exitCode
