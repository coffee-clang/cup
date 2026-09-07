# Owns best-effort process-tree termination for native Windows test tooling.

function Stop-TestProcessTree {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,

        [ValidateRange(1, 60000)]
        [int]$WaitMilliseconds = 10000
    )

    if ($Process.HasExited) {
        return
    }

    $treeStopped = $false
    try {
        & taskkill.exe /PID $Process.Id /T /F 2>&1 | Out-Null
        $treeStopped = ($LASTEXITCODE -eq 0)
    } catch {
        $treeStopped = $false
    }

    if (-not $treeStopped -and -not $Process.HasExited) {
        try {
            $Process.Kill()
        } catch {
            # Cleanup is best effort.
        }
    }
    if (-not $Process.WaitForExit($WaitMilliseconds) -and -not $Process.HasExited) {
        try {
            $Process.Kill()
        } catch {
            # Cleanup is best effort.
        }
        [void]$Process.WaitForExit($WaitMilliseconds)
    }
}
