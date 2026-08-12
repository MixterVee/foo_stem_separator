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
Write-Host "V9 - FORCE /I THROUGH CL"
Write-Host "==============================================="
Write-Host "SDK root:  $SdkRoot"
Write-Host "Repo root: $RepoRoot"
Write-Host "WTL include: $WtlInclude"
Write-Host "ATL include: $AtlInclude"

if (-not (Test-Path $WtlInclude)) {
    throw "WTL include directory does not exist: $WtlInclude"
}
if (-not (Test-Path $AtlInclude)) {
    throw "ATL include directory does not exist: $AtlInclude"
}

$sampleProject = Get-ChildItem -Path $SdkRoot -Filter "foo_sample.vcxproj" -Recurse |
    Select-Object -First 1

if (-not $sampleProject) {
    throw "foo_sample.vcxproj was not found in the downloaded SDK."
}

$sampleDir = $sampleProject.Directory.FullName
$projectPath = $sampleProject.FullName

Write-Host "Using SDK sample project IN PLACE:"
Write-Host $projectPath
Write-Host "Sample directory:"
Write-Host $sampleDir

$foobarHeader = Get-ChildItem -Path $SdkRoot -Filter "foobar2000.h" -Recurse |
    Select-Object -First 1
if (-not $foobarHeader) {
    throw "foobar2000.h could not be found anywhere in the SDK tree."
}
Write-Host "Found foobar2000 header:"
Write-Host $foobarHeader.FullName

foreach ($name in @("foo_stem_separator.cpp", "stem_engine.cpp", "stem_engine.h")) {
    $source = Join-Path $RepoRoot $name
    if (-not (Test-Path $source)) {
        throw "Missing repository source file: $name"
    }

    $dest = Join-Path $sampleDir $name
    Copy-Item $source $dest -Force
    Write-Host "Copied $name -> $dest"
}

$backupProject = "$projectPath.v9backup"
Copy-Item $projectPath $backupProject -Force

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

    $text = Get-Content $projectPath -Raw
    if ($text -match "Release\|x64") {
        $configuration = "Release"
        $platform = "x64"
    }
    elseif ($text -match "Release fb2k\|x64") {
        $configuration = "Release fb2k"
        $platform = "x64"
    }
    else {
        throw "No x64 Release configuration was found."
    }

    $dist = Join-Path $RepoRoot "dist"
    if (Test-Path $dist) {
        Remove-Item $dist -Recurse -Force
    }
    New-Item -ItemType Directory -Force $dist | Out-Null

    Write-Host "Building Configuration=$configuration Platform=$platform"
    Write-Host "IMPORTANT: Project being built is:"
    Write-Host $projectPath

    # Force ATL + WTL into EVERY cl.exe invocation.
    $forcedIncludes = '/I"' + $AtlInclude + '" /I"' + $WtlInclude + '"'
    if ([string]::IsNullOrWhiteSpace($env:CL)) {
        $env:CL = $forcedIncludes
    } else {
        $env:CL = $forcedIncludes + " " + $env:CL
    }

    Write-Host "CL environment variable:"
    Write-Host $env:CL

    # Secondary fallback.
    if ([string]::IsNullOrWhiteSpace($env:INCLUDE)) {
        $env:INCLUDE = "$AtlInclude;$WtlInclude"
    } else {
        $env:INCLUDE = "$AtlInclude;$WtlInclude;" + $env:INCLUDE
    }

    msbuild $projectPath `
        /m `
        /t:Build `
        /p:Configuration="$configuration" `
        /p:Platform="$platform" `
        /p:LanguageStandard=stdcpp17 `
        /v:minimal

    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }

    $builtDll = Get-ChildItem -Path $sampleDir -Filter "foo_stem_separator.dll" -Recurse |
        Select-Object -First 1

    if (-not $builtDll) {
        $builtDll = Get-ChildItem -Path $SdkRoot -Filter "foo_stem_separator.dll" -Recurse |
            Select-Object -First 1
    }

    if (-not $builtDll) {
        throw "Build completed but foo_stem_separator.dll was not found."
    }

    $finalDll = Join-Path $dist "foo_stem_separator.dll"
    Copy-Item $builtDll.FullName $finalDll -Force

    Write-Host "==============================================="
    Write-Host "SUCCESS - DLL CREATED"
    Write-Host $finalDll
    Write-Host "==============================================="
}
finally {
    if (Test-Path $backupProject) {
        Copy-Item $backupProject $projectPath -Force
        Remove-Item $backupProject -Force
    }
}
