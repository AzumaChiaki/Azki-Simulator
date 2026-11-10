param([Parameter(Mandatory=$true)][string]$WorkRoot)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$identity = 'azki.windows.port.tools.v1'
$marker = Join-Path $WorkRoot '.azki-windows-port-owner.json'
if (Test-Path -LiteralPath $WorkRoot) {
    if (!(Test-Path -LiteralPath $marker) -or ((Get-Content -Raw -LiteralPath $marker | ConvertFrom-Json).schema -ne $identity)) {
        throw 'Existing work directory has no matching Azki tools ownership marker.'
    }
} else {
    New-Item -ItemType Directory -Path $WorkRoot | Out-Null
    [ordered]@{schema=$identity;user=$env:USERNAME;created=(Get-Date).ToUniversalTime().ToString('o')} |
        ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding UTF8
}
$download = Join-Path $WorkRoot 'downloads'
$tools = Join-Path $WorkRoot 'tools'
New-Item -ItemType Directory -Force -Path $download,$tools | Out-Null
$receipt = [ordered]@{schema=$identity;pid=$PID;started=(Get-Date).ToUniversalTime().ToString('o');assets=@();complete=$false}
$receiptFile = Join-Path $WorkRoot 'tools-install-receipt.json'
function Save-Receipt { $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptFile -Encoding UTF8 }
function Get-Verified([string]$Url,[string]$Path,[string]$Algorithm,[string]$Expected) {
    if (!(Test-Path -LiteralPath $Path)) {
        Write-Output ('Downloading ' + [IO.Path]::GetFileName($Path))
        $client = [Net.WebClient]::new()
        try { $client.DownloadFile($Url,$Path) } finally { $client.Dispose() }
    }
    $actual=(Get-FileHash -LiteralPath $Path -Algorithm $Algorithm).Hash.ToLowerInvariant()
    if ($actual -ne $Expected.ToLowerInvariant()) { throw ('Checksum mismatch: ' + $Path) }
    $receipt.assets += [ordered]@{url=$Url;path=$Path;algorithm=$Algorithm;hash=$actual;verified=$true}
    Save-Receipt
}
Save-Receipt

# SDK ZIP performs a user-directory installation without changing installed SDKs.
$metadataUrl='https://builds.dotnet.microsoft.com/dotnet/release-metadata/10.0/releases.json'
$metadata=Invoke-RestMethod -Uri $metadataUrl
$sdkVersion=$metadata.'latest-sdk'
$sdk=$null
foreach($release in $metadata.releases) {
    foreach($candidate in @($release.sdk)+@($release.sdks)) {
        if($candidate -and $candidate.version -eq $sdkVersion) { $sdk=$candidate;break }
    }
    if($sdk){break}
}
if(!$sdk){throw 'Latest SDK record absent in official .NET release metadata.'}
$sdkAsset=@($sdk.files | Where-Object {$_.rid -eq 'win-x64' -and $_.name -like '*.zip'})[0]
if(!$sdkAsset){throw 'Windows x64 SDK ZIP absent.'}
$sdkZip=Join-Path $download ('dotnet-sdk-'+$sdkVersion+'-win-x64.zip')
Get-Verified $sdkAsset.url $sdkZip 'SHA512' $sdkAsset.hash
$dotnet=Join-Path $tools 'dotnet'
if(!(Test-Path -LiteralPath (Join-Path $dotnet 'dotnet.exe'))) {
    New-Item -ItemType Directory -Path $dotnet | Out-Null
    Expand-Archive -LiteralPath $sdkZip -DestinationPath $dotnet
}
$receipt.dotnet=Join-Path $dotnet 'dotnet.exe'
$receipt.dotnetSDKs=@(& $receipt.dotnet --list-sdks)
Save-Receipt
Write-Output ('DOTNET_READY=' + $receipt.dotnet)

# Installer is pinned to the PSF release checksum. No PATH/association/launcher changes.
$pythonVersion='3.13.17'
$pythonUrl='https://www.python.org/ftp/python/3.13.17/python-3.13.17-amd64.exe'
$pythonHash='5da012101ba2e17113a9a1cd901e9ce1441086f2b7ecb5de06ec94163899a190'
$pythonInstaller=Join-Path $download ('python-'+$pythonVersion+'-amd64.exe')
Get-Verified $pythonUrl $pythonInstaller 'SHA256' $pythonHash
$signature=Get-AuthenticodeSignature -LiteralPath $pythonInstaller
if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Python Software Foundation') {
    throw 'Python installer does not have a valid Python Software Foundation signature.'
}
$python=Join-Path $tools 'python'
if(!(Test-Path -LiteralPath (Join-Path $python 'python.exe'))) {
    $install=Start-Process -FilePath $pythonInstaller -ArgumentList @('/quiet','InstallAllUsers=0',('TargetDir="'+$python+'"'),'Include_launcher=0','Include_test=0','Include_doc=0','Include_tcltk=1','Include_pip=1','AssociateFiles=0','Shortcuts=0','PrependPath=0','AppendPath=0') -PassThru -Wait
    if($install.ExitCode -ne 0){throw ('Python installer exit '+$install.ExitCode)}
}
$receipt.python=Join-Path $python 'python.exe'
$receipt.pythonVersion=@(& $receipt.python -c 'import sys,tkinter;print(sys.version);print(tkinter.TkVersion)')
if($LASTEXITCODE -ne 0){throw 'Python/Tk validation failed.'}
Save-Receipt
Write-Output ('PYTHON_READY=' + $receipt.python)

# MSYS2 portable archive is the same supported layout as its installer.
$release=Invoke-RestMethod -Uri 'https://api.github.com/repos/msys2/msys2-installer/releases/latest' -Headers @{'User-Agent'='Azki-Simulator-Windows-Build'}
$msysAsset=@($release.assets | Where-Object {$_.name -match '^msys2-base-x86_64-.*\.tar\.xz$'})[0]
if(!$msysAsset -or $msysAsset.digest -notmatch '^sha256:[a-f0-9]{64}$') { throw 'Official MSYS2 archive digest unavailable.' }
$archive=Join-Path $download $msysAsset.name
Get-Verified $msysAsset.browser_download_url $archive 'SHA256' $msysAsset.digest.Substring(7)
$msys=Join-Path $tools 'msys64'
if(!(Test-Path -LiteralPath (Join-Path $msys 'usr\bin\bash.exe'))) {
    & "$env:SystemRoot\System32\tar.exe" -xf $archive -C $tools
    if($LASTEXITCODE -ne 0){throw 'MSYS2 archive extraction failed.'}
}
$bash=Join-Path $msys 'usr\bin\bash.exe'
$env:CHERE_INVOKING='yes'
$env:MSYSTEM='UCRT64'
& $bash -lc 'true'
if($LASTEXITCODE -ne 0){throw 'MSYS2 initialization failed.'}
for($round=1;$round -le 2;$round++) {
    & $bash -lc 'pacman --noconfirm -Syuu' *> (Join-Path $WorkRoot ('msys-update-'+$round+'.log'))
    if($LASTEXITCODE -ne 0){throw ('MSYS2 update failed; round '+$round)}
}
$packages='mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-glib2 mingw-w64-ucrt-x86_64-pixman mingw-w64-ucrt-x86_64-zstd mingw-w64-ucrt-x86_64-dtc mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-packaging mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf make diffutils git tar'
& $bash -lc ('pacman --noconfirm -S --needed '+$packages) *> (Join-Path $WorkRoot 'msys-packages.log')
if($LASTEXITCODE -ne 0){throw 'MSYS2 build dependency installation failed.'}
$receipt.msys=$msys
$receipt.msysPackages=$packages
$receipt.complete=$true
$receipt.finished=(Get-Date).ToUniversalTime().ToString('o')
Save-Receipt
Write-Output ('MSYS_READY=' + $msys)
Write-Output 'TOOLS_INSTALL_COMPLETE'
