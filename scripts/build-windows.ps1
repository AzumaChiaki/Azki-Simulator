param(
 [Parameter(Mandatory=$true)][string]$ToolsRoot,
 [Parameter(Mandatory=$true)][string]$WorkRoot,
 [Parameter(Mandatory=$true)][string]$SourceArchive,
 [ValidateRange(1,32)][int]$Jobs=6
)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$bash=Join-Path $ToolsRoot 'msys64\usr\bin\bash.exe'
$python=Join-Path $ToolsRoot 'python\python.exe'
$dotnet=Join-Path $ToolsRoot 'dotnet\dotnet.exe'
$hostProject=Join-Path $repo 'engine\windows-bluetooth\AzkiBluetoothHost\AzkiBluetoothHost.csproj'
foreach($path in @($bash,$python,$dotnet,$hostProject,$SourceArchive)){
 if(!(Test-Path -LiteralPath $path -PathType Leaf)){throw ('Build input missing: '+$path)}
}
$marker=Join-Path $WorkRoot '.azki-windows-build-wrapper.json'
if(Test-Path -LiteralPath $WorkRoot){
 if(!(Test-Path -LiteralPath $marker) -or ((Get-Content -Raw -LiteralPath $marker|ConvertFrom-Json).source -ne $repo)){
  throw 'Existing build work directory belongs to another task'
 }
}else{
 New-Item -ItemType Directory -Path $WorkRoot|Out-Null
 @{schema='azki.windows.build.wrapper.v1';source=$repo}|ConvertTo-Json|Set-Content -LiteralPath $marker -Encoding UTF8
}
$cliHome=Join-Path $WorkRoot 'dotnet-cli'
New-Item -ItemType Directory -Force -Path $cliHome|Out-Null
$env:DOTNET_CLI_HOME=$cliHome
$env:DOTNET_ROOT=Join-Path $ToolsRoot 'dotnet'
$env:DOTNET_ROOT_X64=$env:DOTNET_ROOT
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE='false'
$env:DOTNET_CLI_TELEMETRY_OPTOUT='1'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1'
$env:DOTNET_NOLOGO='1'
$sdkOutput=& $dotnet --list-sdks
if($LASTEXITCODE -ne 0 -or !($sdkOutput -match '^10\.')){throw 'The private ToolsRoot/dotnet needs a .NET 10 SDK before building Bluetooth'}
$publish=Join-Path $repo 'engine\windows-bluetooth\publish'
$publishMarker=Join-Path $publish '.azki-bluetooth-publish.json'
if(Test-Path -LiteralPath $publish){
 if(Test-Path -LiteralPath $publishMarker){
  $publishOwner=Get-Content -Raw -LiteralPath $publishMarker|ConvertFrom-Json
  if($publishOwner.schema -ne 'azki.windows.bluetooth.publish.v1' -or $publishOwner.source -ne $repo){
   throw 'Existing Bluetooth publish directory belongs to another build; preserve it and choose a fresh checkout'
  }
 }elseif(@(Get-ChildItem -Force -LiteralPath $publish).Count -ne 0){
  throw 'Existing Bluetooth publish directory has no project ownership marker'
 }
}else{New-Item -ItemType Directory -Path $publish|Out-Null}
@{schema='azki.windows.bluetooth.publish.v1';source=$repo;frameworkDependent=$true;requiredRuntime='Microsoft.NETCore.App10'}|
 ConvertTo-Json|Set-Content -LiteralPath $publishMarker -Encoding UTF8
& $dotnet publish $hostProject -c Release -o $publish --self-contained false
if($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath (Join-Path $publish 'AzkiBluetoothHost.exe') -PathType Leaf)){
 throw 'Windows Bluetooth publish failed; the QEMU build was not started'
}
$env:MSYSTEM='UCRT64';$env:CHERE_INVOKING='yes'
$env:AZKI_WIN_BUILD_DRIVER=Join-Path $PSScriptRoot 'windows\build-engine.py'
$env:AZKI_WIN_SOURCE_ARCHIVE=[IO.Path]::GetFullPath($SourceArchive)
$env:AZKI_WIN_BUILD_CACHE=Join-Path $WorkRoot 'cache'
$env:AZKI_WIN_RUNTIME_PYTHON=$python
$env:AZKI_WIN_BUILD_JOBS=[string]$Jobs
$script=(Join-Path $PSScriptRoot 'windows\build-env.sh').Replace('\','/')
$out=Join-Path $WorkRoot 'windows-build.stdout'
$err=Join-Path $WorkRoot 'windows-build.stderr'
$process=Start-Process -FilePath $bash -ArgumentList @('-l',('"'+$script+'"')) -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
@{pid=$process.Id;source=$repo;cache=$env:AZKI_WIN_BUILD_CACHE;output=(Join-Path $repo 'dist\windows')}|ConvertTo-Json|Set-Content (Join-Path $WorkRoot 'owned-build-child.json') -Encoding UTF8
$processHandle=$process.Handle
$process.WaitForExit()
$process.Refresh()
if($null -eq $process.ExitCode){throw 'Build process did not expose a reliable exit status'}
@{pid=$process.Id;exitCode=$process.ExitCode;closed=$true}|ConvertTo-Json|Set-Content (Join-Path $WorkRoot 'owned-build-exit.json') -Encoding UTF8
if($process.ExitCode -ne 0){throw ('Windows build failed; inspect '+$out+' and '+$err)}
Write-Output ('WINDOWS_ENGINE_READY='+ (Join-Path $repo 'dist\windows'))
