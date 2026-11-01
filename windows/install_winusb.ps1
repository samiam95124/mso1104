# Binds the Rigol scope to the WinUSB driver so that mso1104 can open it.
#
# Windows will not install an unsigned driver package, even one that only
# names the WinUSB driver it already ships with, so this does what Zadig
# does: makes a throwaway self-signed certificate, signs a catalog for
# rigol_winusb.inf with it, trusts the certificate on this machine just long
# enough to install the package, and then removes it from every store. What
# stays behind is the installed package alone, in the driver store as
# oemNN.inf; "pnputil /delete-driver oemNN.inf /uninstall /force" takes it
# out again.
#
# Run from any PowerShell; it asks for elevation itself (a UAC prompt). The
# catalog is made with makecat from the Windows SDK or, without the SDK,
# New-FileCatalog. Plug the scope in first.
#
#   powershell -ExecutionPolicy Bypass -File windows\install_winusb.ps1
#
param([switch]$Pause)   # set on the elevated copy, so its window waits to be read

$ErrorActionPreference = "Stop"
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Host "Asking for administrator rights; the result shows in the elevated window"
    $p = Start-Process -FilePath "powershell.exe" -Verb RunAs -PassThru -Wait `
        -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$($MyInvocation.MyCommand.Path)`" -Pause"
    exit $p.ExitCode
}

$cert = $null
$status = 1
try {
    $pkg = Join-Path $env:TEMP "rigol_winusb_pkg"
    Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $pkg | Out-Null
    Copy-Item (Join-Path $dir "rigol_winusb.inf") $pkg
    $inf = Join-Path $pkg "rigol_winusb.inf"
    $cat = Join-Path $pkg "rigol_winusb.cat"

    Write-Host "Making a temporary signing certificate"
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=MSO1104 WinUSB install (temporary)" `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddDays(2) -KeyExportPolicy NonExportable

    Write-Host "Making the catalog"
    $makecat = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\makecat.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -Last 1
    if ($makecat) {
        $cdf = Join-Path $pkg "rigol_winusb.cdf"
        @(
            "[CatalogHeader]",
            "Name=rigol_winusb.cat",
            "ResultDir=.",
            "PublicVersion=0x1",
            "EncodingType=0x00010001",
            "CATATTR1=0x10010001:OSAttr:2:10.0",
            "[CatalogFiles]",
            "<HASH>rigol_winusb.inf=rigol_winusb.inf"
        ) | Set-Content -Path $cdf -Encoding ASCII
        Push-Location $pkg
        & $makecat.FullName $cdf | Out-Null
        Pop-Location
        if (-not (Test-Path $cat)) { throw "makecat did not produce the catalog" }
    } else {
        Write-Host "  (no Windows SDK makecat: using New-FileCatalog)"
        New-FileCatalog -Path $inf -CatalogFilePath $cat -CatalogVersion 2 | Out-Null
    }
    Set-AuthenticodeSignature -FilePath $cat -Certificate $cert -HashAlgorithm SHA256 | Out-Null

    Write-Host "Trusting the certificate for the install"
    foreach ($store in "Root", "TrustedPublisher") {
        $s = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, "LocalMachine")
        $s.Open("ReadWrite"); $s.Add($cert); $s.Close()
    }

    # an earlier copy of this package in the store would shadow the new one
    & pnputil.exe /enum-drivers | Select-String "Published Name" -Context 0,1 | ForEach-Object {
        if ($_.Context.PostContext -match "rigol_winusb.inf") {
            $oem = ($_.Line -split ":\s+")[1]
            Write-Host "Removing the earlier package $oem"
            & pnputil.exe /delete-driver $oem /uninstall /force | Out-Null
        }
    }

    Write-Host "Installing"
    & pnputil.exe /add-driver $inf /install
    if ($LASTEXITCODE -ne 0) { throw "pnputil failed with code $LASTEXITCODE" }
    $status = 0
} catch {
    Write-Host "Failed: $_"
} finally {
    if ($cert) {
        Write-Host "Removing the certificate again"
        foreach ($store in "Root", "TrustedPublisher") {
            $s = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, "LocalMachine")
            $s.Open("ReadWrite"); $s.Remove($cert); $s.Close()
        }
        Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -ErrorAction SilentlyContinue
    }
    Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
}

$d = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "USB\VID_1AB1&PID_04CE*" }
if (-not $d) {
    Write-Host "No scope is plugged in; the package is installed and will bind when one is"
} else {
    $d | Format-List FriendlyName, Status, Service, Problem
    if ($d.Service -eq "WINUSB") { Write-Host "The scope is bound to WinUSB" } else { $status = 1 }
}
if ($Pause) { Read-Host "Press Enter" | Out-Null }
exit $status
