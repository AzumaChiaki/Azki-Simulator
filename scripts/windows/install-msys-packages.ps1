param([Parameter(Mandatory=$true)][string]$WorkRoot)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$marker=Join-Path $WorkRoot '.azki-windows-port-owner.json'
if(!(Test-Path $marker) -or ((Get-Content -Raw $marker|ConvertFrom-Json).schema -ne 'azki.windows.port.tools.v1')){throw 'Unowned work directory'}
$bash=Join-Path $WorkRoot 'tools\msys64\usr\bin\bash.exe'
$env:CHERE_INVOKING='yes';$env:MSYSTEM='UCRT64'
$posixRoot='/'+$WorkRoot.Substring(0,1).ToLowerInvariant()+$WorkRoot.Substring(2).Replace('\','/')
foreach($phase in @('msys-init','msys-packages')){
 $out=Join-Path $WorkRoot ($phase+'-v5.stdout')
 $err=Join-Path $WorkRoot ($phase+'-v5.stderr')
 $process=Start-Process -FilePath $bash -ArgumentList @('-l',($posixRoot+'/'+$phase+'.sh')) -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
 $process.WaitForExit()
 @{phase=$phase;pid=$process.Id;exitCode=$process.ExitCode;closed=$true}|ConvertTo-Json|Set-Content (Join-Path $WorkRoot ($phase+'-v5.exit.json')) -Encoding UTF8
 if($process.ExitCode -ne 0){throw ($phase+' failed; inspect its private stdout/stderr')}
 Write-Output ($phase+' complete')
}
@{msys=(Join-Path $WorkRoot 'tools\msys64');complete=$true}|ConvertTo-Json|Set-Content (Join-Path $WorkRoot 'msys-ready-v5.json') -Encoding UTF8
Write-Output 'MSYS_BUILD_TOOLS_READY'
