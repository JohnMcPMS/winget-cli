<#
.SYNOPSIS
Generates two test certificate chains (root -> intermediate -> leaf)
and writes them into a `cert-chains` subdirectory next to this script.

USAGE
.\New-TestCertificateChains.ps1

The script creates for each chain:
- root.cer, root.pfx
- intermediate.cer, intermediate.pfx
- leaf.cer, leaf.pfx

Default PFX password is `P@ssw0rd` (change if you want).
#>

param(
    [string]$OutDir,
    [string]$PfxPassword = 'P@ssw0rd'
)

$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Definition }
if (-not $OutDir) { $OutDir = Join-Path $ScriptDir 'cert-chains' }

function New-TestChain {
    param(
        [int]$Index,
        [string]$Folder,
        [System.Security.SecureString]$Pwd
    )

    New-Item -Path $Folder -ItemType Directory -Force | Out-Null

    $rootSubject = "CN=Test Root CA $Index"
    $root = New-SelfSignedCertificate -Subject $rootSubject -KeyExportPolicy Exportable -KeyUsage CertSign,CRLSign -Type Custom -KeyLength 2048 -NotAfter (Get-Date).AddYears(20) -CertStoreLocation "Cert:\CurrentUser\My"
    if (-not $root) {
        Write-Error "Failed to create root certificate for chain $Index"
        return @{Root=$null; Intermediate=$null; Leaf=$null; Folder=$Folder}
    }
    $rootDer = Join-Path $Folder 'root.cer'
    Export-Certificate -Cert $root -FilePath $rootDer | Out-Null
    Export-PfxCertificate -Cert $root -FilePath (Join-Path $Folder 'root.pfx') -Password $Pwd | Out-Null

    $interSubject = "CN=Test Intermediate CA $Index"
    $inter = New-SelfSignedCertificate -Subject $interSubject -KeyExportPolicy Exportable -KeyUsage CertSign,CRLSign -Type Custom -KeyLength 2048 -NotAfter (Get-Date).AddYears(10) -CertStoreLocation "Cert:\CurrentUser\My" -Signer $root
    if (-not $inter) {
        Write-Error "Failed to create intermediate certificate for chain $Index"
        return @{Root=$root; Intermediate=$null; Leaf=$null; Folder=$Folder}
    }
    $interDer = Join-Path $Folder 'intermediate.cer'
    Export-Certificate -Cert $inter -FilePath $interDer | Out-Null
    Export-PfxCertificate -Cert $inter -FilePath (Join-Path $Folder 'intermediate.pfx') -Password $Pwd | Out-Null

    $leafDns = "testleaf$Index.local"
    $leafSubject = "CN=$leafDns"
    $leaf = New-SelfSignedCertificate -Subject $leafSubject -DnsName $leafDns -KeyExportPolicy Exportable -KeyUsage DigitalSignature,KeyEncipherment -Type Custom -KeyLength 2048 -NotAfter (Get-Date).AddYears(5) -CertStoreLocation "Cert:\CurrentUser\My" -Signer $inter
    if (-not $leaf) {
        Write-Error "Failed to create leaf certificate for chain $Index"
        return @{Root=$root; Intermediate=$inter; Leaf=$null; Folder=$Folder}
    }
    $leafDer = Join-Path $Folder 'leaf.cer'
    Export-Certificate -Cert $leaf -FilePath $leafDer | Out-Null
    Export-PfxCertificate -Cert $leaf -FilePath (Join-Path $Folder 'leaf.pfx') -Password $Pwd | Out-Null

    return @{Root=$root; Intermediate=$inter; Leaf=$leaf; Folder=$Folder}
}

# Ensure output directory exists
New-Item -Path $OutDir -ItemType Directory -Force | Out-Null

$securePwd = ConvertTo-SecureString -String $PfxPassword -AsPlainText -Force

$results = @()
for ($i = 1; $i -le 2; $i++) {
    $chainFolder = Join-Path $OutDir "chain$i"
    $res = New-TestChain -Index $i -Folder $chainFolder -Pwd $securePwd
    $results += $res
}

Write-Host "Generated 2 certificate chains under: $OutDir"
foreach ($r in $results) { Write-Host " - $($r.Folder)" }

Write-Host "Notes:"
Write-Host " - PFX files use the password provided (default: P@ssw0rd)."

# Automatic cleanup: remove the created certs from the CurrentUser\My store
foreach ($r in $results) {
    $thumbs = @()
    if ($r.Root -and $r.Root.Thumbprint) { $thumbs += $r.Root.Thumbprint }
    if ($r.Intermediate -and $r.Intermediate.Thumbprint) { $thumbs += $r.Intermediate.Thumbprint }
    if ($r.Leaf -and $r.Leaf.Thumbprint) { $thumbs += $r.Leaf.Thumbprint }

    foreach ($tp in $thumbs) {
        try {
            $path = "Cert:\CurrentUser\My\$tp"
            if (Test-Path $path) {
                Remove-Item -Path $path -Force -ErrorAction Stop
                Write-Host "Removed cert from store: $tp"
            }
            else {
                Write-Host "Cert not found in store (already removed?): $tp"
            }
        }
        catch {
            Write-Warning "Failed to remove cert $tp : $($_.Exception.Message)"
        }
    }
}

# End of script
