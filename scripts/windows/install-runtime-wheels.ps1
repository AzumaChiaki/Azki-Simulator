param([Parameter(Mandatory=$true)][string]$WorkRoot)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$python=Join-Path $WorkRoot 'tools\python\python.exe'
$pillow=Join-Path $WorkRoot 'downloads\pillow-12.3.0-cp313-cp313-win_amd64.whl'
$littlefs=Join-Path $WorkRoot 'downloads\littlefs_python-0.19.0-cp313-cp313-win_amd64.whl'
if((Get-FileHash $pillow -Algorithm SHA256).Hash.ToLowerInvariant() -ne '1cca606cd25738df4ed873d5ad46bbdb3d83b5cbca291f6b4ff13a4df6b0bbe8'){throw 'Pillow hash mismatch'}
if((Get-FileHash $littlefs -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'ec8d3de4f05d9420d0e1d013d9134cc68b2b6c15656115a7add2f1f9f5a655be'){throw 'LittleFS hash mismatch'}
& $python -m pip install --no-index --no-deps $pillow $littlefs
if($LASTEXITCODE -ne 0){throw 'Offline package installation failed'}
& $python -c 'import PIL,littlefs,tkinter;print(PIL.__version__);print(tkinter.TkVersion)'
if($LASTEXITCODE -ne 0){throw 'Installed package import failed'}
