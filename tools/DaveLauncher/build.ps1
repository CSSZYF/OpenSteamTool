param([string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\build-dave-launcher'), [switch]$Test)
$ErrorActionPreference = 'Stop'
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw '.NET Framework 4.x compiler not found' }
$sources = @('Program.cs', 'WindowsLaunchHost.cs', 'SteamControl.cs', 'DaveReadiness.cs', 'LaunchSequence.cs', 'SteamPlayRequest.cs', 'SteamPlayWatcher.cs') | ForEach-Object { Join-Path $PSScriptRoot $_ }
& $compiler /nologo /langversion:5 /target:winexe /platform:x64 /optimize+ /utf8output /main:DaveLauncher.Program "/out:$output\DaveLauncher.exe" /r:System.Net.Http.dll /r:System.Web.Extensions.dll /r:System.Windows.Forms.dll $sources
if ($LASTEXITCODE -ne 0) { throw 'Launcher compilation failed' }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'DaveLauncher.json') -Destination $output
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'SteamPlayGate.js') -Destination $output
if ($Test) {
    foreach ($suite in @('LaunchSequence', 'DaveReadiness', 'SteamPlayRequest')) {
        $testSources = @("$suite.cs", "${suite}Tests.cs") | ForEach-Object { Join-Path $PSScriptRoot $_ }
        if ($suite -eq 'LaunchSequence') { $testSources += Join-Path $PSScriptRoot 'SteamControl.cs' }
        & $compiler /nologo /langversion:5 /target:exe "/main:DaveLauncher.${suite}Tests" "/out:$output\${suite}Tests.exe" /r:System.Net.Http.dll /r:System.Web.Extensions.dll $testSources
        if ($LASTEXITCODE -ne 0) { throw "$suite test compilation failed" }
        & (Join-Path $output "${suite}Tests.exe")
        if ($LASTEXITCODE -ne 0) { throw "$suite tests failed" }
    }
    & node (Join-Path $PSScriptRoot 'SteamPlayGateTests.js')
    if ($LASTEXITCODE -ne 0) { throw 'Steam Play prelaunch gate tests failed' }
}
Write-Output "Built $output\DaveLauncher.exe"
