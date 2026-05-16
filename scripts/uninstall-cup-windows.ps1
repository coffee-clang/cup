param(
    [string]$CupRoot,
    [string]$SelfPath
)

Start-Sleep -Seconds 1

if ([string]::IsNullOrWhiteSpace($CupRoot)) {
    Write-Error "Missing cup root."
    exit 1
}

Remove-Item -LiteralPath $CupRoot -Recurse -Force -ErrorAction SilentlyContinue

if (-not [string]::IsNullOrWhiteSpace($SelfPath)) {
    Remove-Item -LiteralPath $SelfPath -Force -ErrorAction SilentlyContinue
}

Write-Output "cup has been uninstalled."
Write-Output "Note: PATH entries were not removed."