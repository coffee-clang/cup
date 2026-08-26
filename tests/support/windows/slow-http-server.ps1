# Serves one response whose final partial transfer window is below the
# Windows installer low-speed threshold.

param(
    [Parameter(Mandatory = $true)]
    [string]$ReadyPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$listener = $null
$client = $null
$stream = $null

try {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
    [IO.File]::WriteAllText($ReadyPath, [string]$port, [Text.Encoding]::ASCII)

    $client = $listener.AcceptTcpClient()
    $stream = $client.GetStream()
    $requestTail = ''
    $requestByte = New-Object byte[] 1

    for ($requestSize = 0; $requestSize -lt 8192; $requestSize++) {
        if ($stream.Read($requestByte, 0, 1) -ne 1) {
            throw 'HTTP fixture request ended before its headers'
        }
        $requestTail += [char]$requestByte[0]
        if ($requestTail.Length -gt 4) {
            $requestTail = $requestTail.Substring($requestTail.Length - 4)
        }
        if ($requestTail -eq "`r`n`r`n") {
            break
        }
    }
    if ($requestTail -ne "`r`n`r`n") {
        throw 'HTTP fixture request headers exceeded the limit'
    }

    $header = [Text.Encoding]::ASCII.GetBytes(
        "HTTP/1.1 200 OK`r`nContent-Length: 2760`r`nConnection: close`r`n`r`n"
    )
    $stream.Write($header, 0, $header.Length)

    $block = New-Object byte[] 512
    for ($index = 0; $index -lt $block.Length; $index++) {
        $block[$index] = [byte][char]'a'
    }
    for ($index = 0; $index -lt 5; $index++) {
        $stream.Write($block, 0, $block.Length)
        $stream.Flush()
        if ($index -ne 4) {
            Start-Sleep -Milliseconds 300
        }
    }

    $tail = New-Object byte[] 100
    for ($index = 0; $index -lt $tail.Length; $index++) {
        $tail[$index] = [byte][char]'b'
    }
    for ($index = 0; $index -lt 2; $index++) {
        Start-Sleep -Milliseconds 600
        $stream.Write($tail, 0, $tail.Length)
        $stream.Flush()
    }
    Start-Sleep -Milliseconds 300
} finally {
    if ($null -ne $stream) {
        $stream.Dispose()
    }
    if ($null -ne $client) {
        $client.Dispose()
    }
    if ($null -ne $listener) {
        $listener.Stop()
    }
}
