# create a local random ntfy channel without sending or scheduling anything.
param([switch]$CopyTopic)

# stop on filesystem failures instead of partially configuring the publisher.
$ErrorActionPreference = 'Stop'
$localDirectory = Join-Path $PSScriptRoot '.local'
$channelPath = Join-Path $localDirectory 'ntfy_topic.txt'

# reject redirected storage before writing the channel.
if (Test-Path -LiteralPath $localDirectory) {
    $localItem = Get-Item -LiteralPath $localDirectory -Force
    # keep local state inside this workspace directory.
    if (-not $localItem.PSIsContainer -or ($localItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'The publisher .local path must be an ordinary directory.'
    }
}
# create only the dedicated local state directory.
else {
    New-Item -ItemType Directory -Path $localDirectory | Out-Null
}

# reuse an existing subscription channel without replacing it.
if (Test-Path -LiteralPath $channelPath) {
    $channelItem = Get-Item -LiteralPath $channelPath -Force
    # reject redirects, directories and unexpectedly large input.
    if ($channelItem.PSIsContainer -or ($channelItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $channelItem.Length -gt 128) {
        throw 'The local channel file is invalid.'
    }
    $channel = [IO.File]::ReadAllText($channelPath).Trim()
}
# generate 128 random bits, creating the channel file without overwriting it.
else {
    $randomBytes = New-Object byte[] 16
    $generator = [Security.Cryptography.RandomNumberGenerator]::Create()
    # release the cryptographic provider after filling the random buffer.
    try {
        $generator.GetBytes($randomBytes)
    }
    # release handles even when random generation fails.
    finally {
        $generator.Dispose()
    }
    $channel = [BitConverter]::ToString($randomBytes).Replace('-', '').ToLowerInvariant()
    $channelBytes = [Text.Encoding]::ASCII.GetBytes($channel)
    $stream = [IO.File]::Open($channelPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    # write only the random channel, never a URL or credential bundle.
    try {
        $stream.Write($channelBytes, 0, $channelBytes.Length)
    }
    # close the file before the user starts the publisher.
    finally {
        $stream.Dispose()
    }
}

# validate reused files against the same policy as the Python publisher.
if ($channel -cnotmatch '\A[A-Za-z0-9_-]{32,64}\z') {
    throw 'The local channel must contain 32-64 letters, digits, underscores or hyphens.'
}

# copy only when the user explicitly requests local clipboard access.
if ($CopyTopic) {
    Set-Clipboard -Value $channel
    Write-Output 'The ntfy channel was copied to the local clipboard; keep it out of chat.'
}

# reveal the storage location, never the channel itself.
Write-Output ('Publisher ready. Local channel file: ' + $channelPath)
Write-Output 'No notification was sent and no scheduled task was created.'
