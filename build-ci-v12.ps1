param(
    [Parameter(Mandatory=$true)][string]$SdkRoot,
    [Parameter(Mandatory=$true)][string]$RepoRoot,
    [Parameter(Mandatory=$true)][string]$WtlInclude,
    [Parameter(Mandatory=$true)][string]$AtlInclude
)

$ErrorActionPreference = "Stop"

$sampleProject = Get-ChildItem -Path $SdkRoot -Filter "foo_sample.vcxproj" -Recurse |
    Select-Object -First 1
if (-not $sampleProject) { throw "foo_sample.vcxproj was not found." }

$impl = Join-Path $RepoRoot "foo_stem_separator_impl.cpp"
if (-not (Test-Path $impl)) { throw "Missing repository source: foo_stem_separator_impl.cpp" }
Copy-Item $impl (Join-Path $sampleProject.Directory.FullName "foo_stem_separator_impl.cpp") -Force

& (Join-Path $RepoRoot "build-ci-v12-impl.ps1") `
    -SdkRoot $SdkRoot `
    -RepoRoot $RepoRoot `
    -WtlInclude $WtlInclude `
    -AtlInclude $AtlInclude
