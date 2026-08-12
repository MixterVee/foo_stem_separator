param(
    [Parameter(Mandatory=$true)]
    [string]$SdkRoot,

    [Parameter(Mandatory=$true)]
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"

Write-Host "SDK root:  $SdkRoot"
Write-Host "Repo root: $RepoRoot"

$sampleProject = Get-ChildItem -Path $SdkRoot -Filter "foo_sample.vcxproj" -Recurse |
    Select-Object -First 1

if (-not $sampleProject) {
    Write-Host "Could not find foo_sample.vcxproj. Available .vcxproj files:"
    Get-ChildItem -Path $SdkRoot -Filter "*.vcxproj" -Recurse |
        ForEach-Object { Write-Host $_.FullName }
    throw "foo_sample.vcxproj was not found in the SDK."
}

$sampleDir = $sampleProject.Directory.FullName
Write-Host "Using SDK sample project: $($sampleProject.FullName)"

$workRoot = Join-Path $RepoRoot "_ci_component"
if (Test-Path $workRoot) {
    Remove-Item $workRoot -Recurse -Force
}
Copy-Item $sampleDir $workRoot -Recurse

$project = Get-ChildItem -Path $workRoot -Filter "foo_sample.vcxproj" |
    Select-Object -First 1

if (-not $project) {
    throw "Copied foo_sample.vcxproj could not be found."
}

$requiredSources = @(
    "foo_stem_separator.cpp",
    "stem_engine.cpp",
    "stem_engine.h"
)

foreach ($name in $requiredSources) {
    $src = Join-Path $RepoRoot $name
    if (-not (Test-Path $src)) {
        throw "Required repository file is missing: $name"
    }
    Copy-Item $src (Join-Path $workRoot $name) -Force
}

[xml]$xml = Get-Content $project.FullName

$nsUri = $xml.Project.NamespaceURI
$ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
$ns.AddNamespace("m", $nsUri)

# Remove SDK sample source/header entries.
$compileNodes = @($xml.SelectNodes("//m:ClCompile", $ns))
foreach ($node in $compileNodes) {
    [void]$node.ParentNode.RemoveChild($node)
}

$includeNodes = @($xml.SelectNodes("//m:ClInclude", $ns))
foreach ($node in $includeNodes) {
    [void]$node.ParentNode.RemoveChild($node)
}

# Add our sources.
$itemGroup = $xml.CreateElement("ItemGroup", $nsUri)

foreach ($cpp in @("foo_stem_separator.cpp", "stem_engine.cpp")) {
    $n = $xml.CreateElement("ClCompile", $nsUri)
    $n.SetAttribute("Include", $cpp)

    $pch = $xml.CreateElement("PrecompiledHeader", $nsUri)
    $pch.InnerText = "NotUsing"
    [void]$n.AppendChild($pch)

    [void]$itemGroup.AppendChild($n)
}

$h = $xml.CreateElement("ClInclude", $nsUri)
$h.SetAttribute("Include", "stem_engine.h")
[void]$itemGroup.AppendChild($h)
[void]$xml.Project.AppendChild($itemGroup)

# Set DLL name.
$propertyGroups = @($xml.SelectNodes("//m:PropertyGroup", $ns))
foreach ($pg in $propertyGroups) {
    if ($pg.GetAttribute("Condition")) {
        $target = $pg.SelectSingleNode("m:TargetName", $ns)
        if (-not $target) {
            $target = $xml.CreateElement("TargetName", $nsUri)
            [void]$pg.AppendChild($target)
        }
        $target.InnerText = "foo_stem_separator"
    }
}

# IMPORTANT: std::filesystem requires C++17 or newer.
$itemDefs = @($xml.SelectNodes("//m:ItemDefinitionGroup", $ns))
foreach ($idg in $itemDefs) {
    $cl = $idg.SelectSingleNode("m:ClCompile", $ns)
    if (-not $cl) {
        $cl = $xml.CreateElement("ClCompile", $nsUri)
        [void]$idg.AppendChild($cl)
    }

    $lang = $cl.SelectSingleNode("m:LanguageStandard", $ns)
    if (-not $lang) {
        $lang = $xml.CreateElement("LanguageStandard", $nsUri)
        [void]$cl.AppendChild($lang)
    }
    $lang.InnerText = "stdcpp17"
}

# Add required Windows libraries.
$linkNodes = @($xml.SelectNodes("//m:ItemDefinitionGroup/m:Link", $ns))
foreach ($link in $linkNodes) {
    $deps = $link.SelectSingleNode("m:AdditionalDependencies", $ns)
    if (-not $deps) {
        $deps = $xml.CreateElement("AdditionalDependencies", $nsUri)
        [void]$link.AppendChild($deps)
        $deps.InnerText = "bcrypt.lib;shell32.lib;ole32.lib;%(AdditionalDependencies)"
    } else {
        $existing = $deps.InnerText
        $deps.InnerText = "bcrypt.lib;shell32.lib;ole32.lib;" + $existing
    }
}

$xml.Save($project.FullName)

Write-Host "Patched project:"
Write-Host $project.FullName

$projectText = Get-Content $project.FullName -Raw
if ($projectText -match "Release\|x64") {
    $configuration = "Release"
    $platform = "x64"
} elseif ($projectText -match "Release fb2k\|x64") {
    $configuration = "Release fb2k"
    $platform = "x64"
} else {
    Write-Host "Project configurations:"
    Select-String -Path $project.FullName -Pattern "ProjectConfiguration Include" |
        ForEach-Object { Write-Host $_.Line.Trim() }
    throw "Could not locate an x64 Release configuration in foo_sample.vcxproj."
}

Write-Host "Building Configuration=$configuration Platform=$platform"
Write-Host "C++ language standard: C++17"

msbuild $project.FullName `
    /m `
    /t:Build `
    /p:Configuration="$configuration" `
    /p:Platform="$platform" `
    /p:OutDir="$RepoRoot\dist\" `
    /p:TargetName="foo_stem_separator" `
    /p:LanguageStandard=stdcpp17 `
    /v:minimal

if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}

$dll = Get-ChildItem -Path (Join-Path $RepoRoot "dist") -Filter "foo_stem_separator.dll" -Recurse |
    Select-Object -First 1

if (-not $dll) {
    Write-Host "Files in dist:"
    Get-ChildItem -Path (Join-Path $RepoRoot "dist") -Recurse |
        ForEach-Object { Write-Host $_.FullName }
    throw "Build finished but foo_stem_separator.dll was not found."
}

Write-Host ""
Write-Host "SUCCESS:"
Write-Host $dll.FullName
