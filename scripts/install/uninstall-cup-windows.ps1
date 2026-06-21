param(
    [Parameter(Mandatory = $true)]
    [string]$CupRoot,

    [Parameter(Mandatory = $true)]
    [string]$SelfPath,

    [Parameter(Mandatory = $true)]
    [int]$ParentPid
)

$ErrorActionPreference = "Stop"

try {
    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        throw "USERPROFILE is not set"
    }

    $expectedRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $env:USERPROFILE ".cup")
    ).TrimEnd('\')
    $requestedRoot = [System.IO.Path]::GetFullPath($CupRoot).TrimEnd('\')

    if (-not $requestedRoot.Equals(
        $expectedRoot,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw "refusing to remove a non-canonical cup root"
    }

    try {
        Wait-Process -Id $ParentPid
    } catch [Microsoft.PowerShell.Commands.ProcessCommandException] {
        # The parent already exited before the helper started waiting.
    }

    if (Test-Path -LiteralPath $requestedRoot) {
        Remove-Item -LiteralPath $requestedRoot -Recurse -Force
    }

    if (Test-Path -LiteralPath $SelfPath -PathType Leaf) {
        Remove-Item -LiteralPath $SelfPath -Force
    }

    exit 0
} catch {
    Write-Error $_
    exit 1
}
