# Owns the configuration domain shared by native Windows test tooling.

function Assert-TestConfiguration {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Configuration
    )

    if ([string]::IsNullOrWhiteSpace($Configuration) -or
        $Configuration -notin @(
            "development", "debug", "coverage", "sanitizers", "release")) {
        throw "unsupported CUP_TEST_CONFIGURATION: $Configuration"
    }
}

function Get-TestConfiguration {
    $configuration = if ([string]::IsNullOrWhiteSpace($env:CUP_TEST_CONFIGURATION)) {
        "development"
    } else {
        $env:CUP_TEST_CONFIGURATION
    }
    Assert-TestConfiguration -Configuration $configuration
    return $configuration
}
