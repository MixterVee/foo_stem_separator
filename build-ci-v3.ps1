param(
    [Parameter(Mandatory=$true)]
    [string]$SdkRoot,

    [Parameter(Mandatory=$true)]
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"

Write-Host "==============================================="
Write-Host "V3 - BUILDING IN SDK TREE"
Write-Host "==============================================="
Write-Host "SDK root:  $SdkRoot"
Write-Host "Repo root: $RepoRoot"

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

# Sanity checks proving the SDK tree exists around the sample project.
$foobarHeader = Get-ChildItem -Path $SdkRoot -Filter "foobar2000.h" -Recurse |
    Select-Object -First 1
if (-not $foobarHeader) {
    throw "foobar2000.h could not be found anywhere in the SDK tree."
}
Write-Host "Found foobar2000 header:"
Write-Host $foobarHeader.FullName

# Copy component sources into the SDK sample directory.
foreach ($name in @("foo_stem_separator.cpp", "stem_engine.cpp", "stem_engine.h")) {
    $source = Join-Path $RepoRoot $name
    if (-not (Test-Path $source)) {
        throw "Missing repository source file: $name"
    }

    $dest = Join-Path $sampleDir $name
    Copy-Item $source $dest -Force
    Write-Host "Copied $name -> $dest"
}

# Back up the sample project and patch only its file list/settings.
$backupProject = "$projectPath.v3backup"
Copy-Item $projectPath $backupProject -Force

try {
    [xml]$xml = Get-Content $projectPath
    $nsUri = $xml.Project.NamespaceURI
    $ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
    $ns.AddNamespace("m", $nsUri)

    # Remove foo_sample's own compile/header entries.
    @($xml.SelectNodes("//m:ClCompile", $ns)) | ForEach-Object {
        [void]$_.ParentNode.RemoveChild($_)
    }
    @($xml.SelectNodes("//m:ClInclude", $ns)) | ForEach-Object {
        [void]$_.ParentNode.RemoveChild($_)
    }

    # Add our files.
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

    # C++17 for std::filesystem.
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

    # Windows libraries used by stem_engine.cpp.
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

    # Force component output name.
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
        Write-Host "Configurations found:"
        Select-String -Path $projectPath -Pattern "ProjectConfiguration Include" |
            ForEach-Object { Write-Host $_.Line.Trim() }
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

    msbuild $projectPath `
        /m `
        /t:Build `
        /p:Configuration="$configuration" `
        /p:Platform="$platform" `
        /p:OutDir="$dist\" `
        /p:TargetName="foo_stem_separator" `
        /p:LanguageStandard=stdcpp17 `
        /v:minimal

    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }

    $dll = Get-ChildItem -Path $dist -Filter "foo_stem_separator.dll" -Recurse |
        Select-Object -First 1

    if (-not $dll) {
        Write-Host "Contents of dist:"
        Get-ChildItem -Path $dist -Recurse | ForEach-Object {
            Write-Host $_.FullName
        }
        throw "Build completed but foo_stem_separator.dll was not found."
    }

    Write-Host "==============================================="
    Write-Host "SUCCESS - DLL CREATED"
    Write-Host $dll.FullName
    Write-Host "==============================================="
}
finally {
    if (Test-Path $backupProject) {
        Copy-Item $backupProject $projectPath -Force
        Remove-Item $backupProject -Force
    }
}
