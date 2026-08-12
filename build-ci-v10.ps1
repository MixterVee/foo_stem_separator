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
Write-Host "V10 - VS2022 v143 + WTL 10.0.10320"
Write-Host "==============================================="
Write-Host "SDK root:    $SdkRoot"
Write-Host "Repo root:   $RepoRoot"
Write-Host "WTL include: $WtlInclude"
Write-Host "ATL include: $AtlInclude"

if (-not (Test-Path (Join-Path $WtlInclude "atlapp.h"))) {
    throw "WTL atlapp.h not found in: $WtlInclude"
}
if (-not (Test-Path (Join-Path $AtlInclude "atlbase.h"))) {
    throw "ATL atlbase.h not found in: $AtlInclude"
}

$sampleProject = Get-ChildItem -Path $SdkRoot -Filter "foo_sample.vcxproj" -Recurse |
    Select-Object -First 1

if (-not $sampleProject) {
    throw "foo_sample.vcxproj was not found."
}

$sampleDir = $sampleProject.Directory.FullName
$projectPath = $sampleProject.FullName

Write-Host "Using sample project:"
Write-Host $projectPath

foreach ($name in @("foo_stem_separator.cpp", "stem_engine.cpp", "stem_engine.h")) {
    $src = Join-Path $RepoRoot $name
    if (-not (Test-Path $src)) {
        throw "Missing repository source: $name"
    }
    Copy-Item $src (Join-Path $sampleDir $name) -Force
}

$backup = "$projectPath.v10backup"
Copy-Item $projectPath $backup -Force

try {
    [xml]$xml = Get-Content $projectPath
    $nsUri = $xml.Project.NamespaceURI
    $ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
    $ns.AddNamespace("m", $nsUri)

    # Replace only foo_sample's source file entries.
    @($xml.SelectNodes("//m:ClCompile", $ns)) | ForEach-Object {
        [void]$_.ParentNode.RemoveChild($_)
    }
    @($xml.SelectNodes("//m:ClInclude", $ns)) | ForEach-Object {
        [void]$_.ParentNode.RemoveChild($_)
    }

    $group = $xml.CreateElement("ItemGroup", $nsUri)

    foreach ($cpp in @("foo_stem_separator.cpp", "stem_engine.cpp")) {
        $node = $xml.CreateElement("ClCompile", $nsUri)
        $node.SetAttribute("Include", $cpp)

        $pch = $xml.CreateElement("PrecompiledHeader", $nsUri)
        $pch.InnerText = "NotUsing"
        [void]$node.AppendChild($pch)

        [void]$group.AppendChild($node)
    }

    $headerNode = $xml.CreateElement("ClInclude", $nsUri)
    $headerNode.SetAttribute("Include", "stem_engine.h")
    [void]$group.AppendChild($headerNode)
    [void]$xml.Project.AppendChild($group)

    # Component settings only.
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
        if (-not $deps) {
            $deps = $xml.CreateElement("AdditionalDependencies", $nsUri)
            [void]$link.AppendChild($deps)
            $deps.InnerText = "bcrypt.lib;shell32.lib;ole32.lib;%(AdditionalDependencies)"
        }
        elseif ($deps.InnerText -notmatch "bcrypt\.lib") {
            $deps.InnerText = "bcrypt.lib;shell32.lib;ole32.lib;" + $deps.InnerText
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

    # Force exact ATL + WTL include paths into all CL invocations.
   $forced = '/I"' + $SdkRoot + '" /I"' + $AtlInclude + '" /I"' + $WtlInclude + '"'
    if ([string]::IsNullOrWhiteSpace($env:CL)) {
        $env:CL = $forced
    } else {
        $env:CL = $forced + " " + $env:CL
    }

    Write-Host "CL:"
    Write-Host $env:CL

    # Explicitly request v143 toolset from the VS2022 runner.
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
    New-Item -ItemType Directory -Force $dist | Out-Null
    $final = Join-Path $dist "foo_stem_separator.dll"
    Copy-Item $dll.FullName $final -Force

    Write-Host "==============================================="
    Write-Host "SUCCESS - DLL CREATED"
    Write-Host $final
    Write-Host "==============================================="
}
finally {
    if (Test-Path $backup) {
        Copy-Item $backup $projectPath -Force
        Remove-Item $backup -Force
    }
}
