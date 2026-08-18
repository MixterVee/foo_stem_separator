param(
    [Parameter(Mandatory=$true)]
    [string]$SdkRoot,

    [Parameter(Mandatory=$true)]
    [string]$RepoRoot,

    [Parameter(Mandatory=$true)]
    [string]$WtlInclude,

    [Parameter(Mandatory=$true)]
    [string]$AtlInclude
)

$ErrorActionPreference = "Stop"

Write-Host "==============================================="
Write-Host "V12 - NATIVE ONNX DSP PROTOTYPE"
Write-Host "==============================================="

$handoffPatch = Join-Path $RepoRoot "patch-cache-compression-handoff.py"
if (Test-Path $handoffPatch) {
    python $handoffPatch
    if ($LASTEXITCODE -ne 0) {
        throw "Cache compression handoff timing patch failed."
    }
}

$sampleProject = Get-ChildItem -Path $SdkRoot -Filter "foo_sample.vcxproj" -Recurse |
    Select-Object -First 1

if (-not $sampleProject) {
    throw "foo_sample.vcxproj was not found."
}

$sampleDir = $sampleProject.Directory.FullName
$projectPath = $sampleProject.FullName

$cppFiles = @(
    "foo_stem_separator.cpp",
    "stem_mode.cpp",
    "onnx_stem_engine.cpp",
    "persistent_stem_cache.cpp",
    "stem_dsp.cpp",
    "stem_waveform_provider.cpp",
    "stem_benchmark.cpp"
)

$hFiles = @(
    "stem_mode.h",
    "onnx_stem_engine.h",
    "persistent_stem_cache.h",
    "stem_waveform_provider.h",
    "stem_transport_service.h",
    "stem_processing_status_service.h"
)

foreach ($name in ($cppFiles + $hFiles)) {
    $src = Join-Path $RepoRoot $name
    if (-not (Test-Path $src)) {
        throw "Missing repository source: $name"
    }
    Copy-Item $src (Join-Path $sampleDir $name) -Force
}

$backup = "$projectPath.v12backup"
Copy-Item $projectPath $backup -Force

try {
    [xml]$xml = Get-Content $projectPath
    $nsUri = $xml.Project.NamespaceURI
    $ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
    $ns.AddNamespace("m", $nsUri)

    @($xml.SelectNodes("//m:ClCompile", $ns)) | ForEach-Object {
        [void]$_.ParentNode.RemoveChild($_)
    }
    @($xml.SelectNodes("//m:ClInclude", $ns)) | ForEach-Object {
        [void]$_.ParentNode.RemoveChild($_)
    }

    $group = $xml.CreateElement("ItemGroup", $nsUri)

    foreach ($cpp in $cppFiles) {
        $node = $xml.CreateElement("ClCompile", $nsUri)
        $node.SetAttribute("Include", $cpp)

        $pch = $xml.CreateElement("PrecompiledHeader", $nsUri)
        $pch.InnerText = "NotUsing"
        [void]$node.AppendChild($pch)

        [void]$group.AppendChild($node)
    }

    foreach ($h in $hFiles) {
        $node = $xml.CreateElement("ClInclude", $nsUri)
        $node.SetAttribute("Include", $h)
        [void]$group.AppendChild($node)
    }

    [void]$xml.Project.AppendChild($group)

    foreach ($idg in @($xml.SelectNodes("//m:ItemDefinitionGroup", $ns))) {
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

    foreach ($link in @($xml.SelectNodes("//m:ItemDefinitionGroup/m:Link", $ns))) {
        $deps = $link.SelectSingleNode("m:AdditionalDependencies", $ns)
        $needed = "user32.lib;shell32.lib;ole32.lib"

        if (-not $deps) {
            $deps = $xml.CreateElement("AdditionalDependencies", $nsUri)
            [void]$link.AppendChild($deps)
            $deps.InnerText = "$needed;%(AdditionalDependencies)"
        } else {
            $existing = $deps.InnerText
            foreach ($lib in @("user32.lib","shell32.lib","ole32.lib")) {
                if ($existing -notmatch [regex]::Escape($lib)) {
                    $existing = "$lib;$existing"
                }
            }
            $deps.InnerText = $existing
        }
    }

    foreach ($pg in @($xml.SelectNodes("//m:PropertyGroup", $ns))) {
        if ($pg.GetAttribute("Condition")) {
            $target = $pg.SelectSingleNode("m:TargetName", $ns)
            if (-not $target) {
                $target = $xml.CreateElement("TargetName", $nsUri)
                [void]$pg.AppendChild($target)
            }
            $target.InnerText = "foo_stem_separator"
        }
    }

    $xml.Save($projectPath)

    $forced = '/I"' + $SdkRoot + '" /I"' + $AtlInclude + '" /I"' + $WtlInclude + '"'
    if ([string]::IsNullOrWhiteSpace($env:CL)) {
        $env:CL = $forced
    } else {
        $env:CL = $forced + " " + $env:CL
    }

    msbuild $projectPath `
        /m `
        /t:Build `
        /p:Configuration=Release `
        /p:Platform=x64 `
        /p:PlatformToolset=v143 `
        /p:LanguageStandard=stdcpp17 `
        /v:minimal

    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }

    $dll = Get-ChildItem -Path $SdkRoot -Filter "foo_stem_separator.dll" -Recurse |
        Select-Object -First 1

    if (-not $dll) {
        throw "Build succeeded but foo_stem_separator.dll was not found."
    }

    $dist = Join-Path $RepoRoot "dist"
    if (Test-Path $dist) {
        Remove-Item $dist -Recurse -Force
    }
    New-Item -ItemType Directory -Force $dist | Out-Null

    Copy-Item $dll.FullName (Join-Path $dist "foo_stem_separator.dll") -Force

    Write-Host "Component DLL created."
}
finally {
    if (Test-Path $backup) {
        Copy-Item $backup $projectPath -Force
        Remove-Item $backup -Force
    }
}
