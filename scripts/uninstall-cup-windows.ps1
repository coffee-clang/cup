param(
    [string]$CupRoot,
    [string]$SelfPath
)

Start-Sleep -Seconds 1

if ([string]::IsNullOrWhiteSpace($CupRoot)) {
    exit 1
}

Remove-Item -LiteralPath $CupRoot -Recurse -Force -ErrorAction SilentlyContinue

if (-not [string]::IsNullOrWhiteSpace($SelfPath)) {
    Remove-Item -LiteralPath $SelfPath -Force -ErrorAction SilentlyContinue
}

exit 0