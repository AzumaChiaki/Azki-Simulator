param([Parameter(Mandatory=$true)][string]$WorkRoot)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$marker=Join-Path $WorkRoot '.azki-windows-port-owner.json'
if(!(Test-Path $marker) -or ((Get-Content -Raw $marker|ConvertFrom-Json).schema -ne 'azki.windows.port.tools.v1')){throw 'Unowned work directory'}
$downloads=Join-Path $WorkRoot 'downloads'
$tools=Join-Path $WorkRoot 'tools'
$pythonInstaller=Join-Path $downloads 'python-3.13.17-amd64.exe'
if((Get-FileHash $pythonInstaller -Algorithm SHA256).Hash.ToLowerInvariant() -ne '5da012101ba2e17113a9a1cd901e9ce1441086f2b7ecb5de06ec94163899a190'){throw 'Python checksum mismatch'}
$signature=Get-AuthenticodeSignature $pythonInstaller
if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Python Software Foundation'){throw 'Invalid PSF installer signature'}
$python=Join-Path $tools 'python'
if(!(Test-Path (Join-Path $python 'python.exe'))){
 $p=Start-Process -FilePath $pythonInstaller -ArgumentList @('/quiet','InstallAllUsers=0',('TargetDir="'+$python+'"'),'Include_launcher=0','Include_test=0','Include_doc=0','Include_tcltk=1','Include_pip=1','AssociateFiles=0','Shortcuts=0','PrependPath=0','AppendPath=0') -PassThru -Wait
 if($p.ExitCode -ne 0){throw ('Python installer exit '+$p.ExitCode)}
}
$pythonExe=Join-Path $python 'python.exe'
& $pythonExe -c 'import sys,tkinter;print(sys.version);print(tkinter.TkVersion)'
if($LASTEXITCODE -ne 0){throw 'Python/Tk failed'}
@{python=$pythonExe;version='3.13.17';tcltk=$true;installerVerified=$true}|ConvertTo-Json|Set-Content (Join-Path $WorkRoot 'python-ready-v3.json') -Encoding UTF8
Write-Output ('PYTHON_READY='+$pythonExe)

$archive=Join-Path $downloads 'msys2-base-x86_64-latest.tar.xz'
if((Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'a7dbb48eef460edd6282e17dd511aad5630bdd1e0a5824bfeb33b8842c7e3646'){throw 'MSYS2 checksum mismatch'}
$msys=Join-Path $tools 'msys64'
$bash=Join-Path $msys 'usr\bin\bash.exe'
if(!(Test-Path $bash)){
 & "$env:SystemRoot\System32\tar.exe" -xf $archive -C $tools
 if($LASTEXITCODE -ne 0){throw 'MSYS2 extraction failed'}
}
$env:CHERE_INVOKING='yes';$env:MSYSTEM='UCRT64'
& $bash -lc 'true'
if($LASTEXITCODE -ne 0){throw 'MSYS2 first shell failed'}
Write-Output ('MSYS_BASE_READY='+$msys)
for($round=1;$round -le 2;$round++){
 & $bash -lc 'pacman --noconfirm -Syuu' *> (Join-Path $WorkRoot ('msys-update-v3-'+$round+'.log'))
 if($LASTEXITCODE -ne 0){throw ('MSYS2 update failed; round '+$round)}
}
$packages='mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-glib2 mingw-w64-ucrt-x86_64-pixman mingw-w64-ucrt-x86_64-zstd mingw-w64-ucrt-x86_64-dtc mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-packaging mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf make diffutils git tar'
& $bash -lc ('pacman --noconfirm -S --needed '+$packages) *> (Join-Path $WorkRoot 'msys-packages-v3.log')
if($LASTEXITCODE -ne 0){throw 'MSYS2 build dependencies failed'}
@{msys=$msys;packages=$packages;complete=$true}|ConvertTo-Json|Set-Content (Join-Path $WorkRoot 'msys-ready-v3.json') -Encoding UTF8
Write-Output 'MSYS_BUILD_TOOLS_READY'
