param(
    [Parameter(Mandatory=$true)] [string]$SdkRoot,
    [Parameter(Mandatory=$true)] [string]$RepoRoot,
    [Parameter(Mandatory=$true)] [string]$WtlInclude,
    [Parameter(Mandatory=$true)] [string]$AtlInclude
)

$ErrorActionPreference = "Stop"

Push-Location $RepoRoot
try {
    python .\patch-cache-settings.py
    if ($LASTEXITCODE -ne 0) {
        throw "Cache settings source patch failed."
    }

    python .\patch-cache-settings-compilefix.py
    if ($LASTEXITCODE -ne 0) {
        throw "Cache settings Windows compile fix failed."
    }

    & .\build-ci-v12.ps1 `
        -SdkRoot $SdkRoot `
        -RepoRoot $RepoRoot `
        -WtlInclude $WtlInclude `
        -AtlInclude $AtlInclude

    if ($LASTEXITCODE -ne 0) {
        throw "Stem Separator build failed."
    }
}
finally {
    Pop-Location
}
