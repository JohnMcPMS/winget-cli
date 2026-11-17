# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [Parameter(Mandatory = $true)]
    [string]$OfflineRegistryDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$hostArchitecture = switch ($env:PROCESSOR_ARCHITECTURE)
{
    'AMD64' { 'x64' }
    'ARM64' { 'arm64' }
    'x86' { 'x86' }
    default { throw "Unsupported build host architecture: $env:PROCESSOR_ARCHITECTURE" }
}

$offregPath = (Resolve-Path -LiteralPath (Join-Path $OfflineRegistryDirectory "$hostArchitecture\offreg.dll")).Path
$manifestPath = Join-Path $PSScriptRoot 'Package.appxmanifest'
[xml]$manifest = Get-Content -LiteralPath $manifestPath -Raw
$namespaces = [System.Xml.XmlNamespaceManager]::new($manifest.NameTable)
$namespaces.AddNamespace('appx', $manifest.DocumentElement.NamespaceURI)
$namespaces.AddNamespace('com', 'http://schemas.microsoft.com/appx/manifest/com/windows10')
$application = $manifest.SelectSingleNode('/appx:Package/appx:Applications/appx:Application[@Id="WinGetComServer"]', $namespaces)
if (!$application)
{
    throw "WinGetComServer application was not found in $manifestPath"
}

$classRegistrations = @($application.SelectNodes('appx:Extensions/com:Extension/com:ComServer/com:ExeServer[@DisplayName="Windows Package Manager Server"]/com:Class', $namespaces))
if (!$classRegistrations.Count)
{
    throw "Windows Package Manager Server COM classes were not found in $manifestPath"
}

$appId = ([guid]($classRegistrations[0].GetAttribute('Id'))).ToString('B').ToUpperInvariant()

# Offline registry APIs never mount the hive or modify the build machine's registry.
Add-Type -TypeDefinition (@'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace WinGet.InternalCOMRegistry
{
    public static class Hive
    {
        [DllImport(@"OFFREG_DLL", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern uint ORCreateHive(out IntPtr hive);

        [DllImport(@"OFFREG_DLL", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern uint ORCreateKey(IntPtr hive, string path, string keyClass, uint options,
            IntPtr securityDescriptor, out IntPtr key, out uint disposition);

        [DllImport(@"OFFREG_DLL", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern uint ORSetValue(IntPtr key, string name, uint type, byte[] data, uint length);

        [DllImport(@"OFFREG_DLL", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern uint ORSaveHive(IntPtr hive, string path, uint majorVersion, uint minorVersion);

        [DllImport(@"OFFREG_DLL", ExactSpelling = true)]
        private static extern uint ORCloseKey(IntPtr key);

        [DllImport(@"OFFREG_DLL", ExactSpelling = true)]
        private static extern uint ORCloseHive(IntPtr hive);

        public static void Check(uint error)
        {
            if (error != 0)
            {
                throw new Win32Exception((int)error);
            }
        }

        public static IntPtr Create()
        {
            IntPtr hive;
            Check(ORCreateHive(out hive));
            return hive;
        }

        public static void SetString(IntPtr hive, string path, string name, string value)
        {
            IntPtr key = hive;
            try
            {
                foreach (string component in path.Split('\\'))
                {
                    IntPtr child;
                    uint disposition;
                    Check(ORCreateKey(key, component, null, 0, IntPtr.Zero, out child, out disposition));
                    IntPtr parent = key;
                    key = child;
                    if (parent != hive)
                    {
                        Check(ORCloseKey(parent));
                    }
                }

                byte[] data = Encoding.Unicode.GetBytes(value + "\0");
                Check(ORSetValue(key, name, 1, data, (uint)data.Length));
            }
            finally
            {
                if (key != hive)
                {
                    Check(ORCloseKey(key));
                }
            }
        }

        public static void Save(IntPtr hive, string path)
        {
            Check(ORSaveHive(hive, path, 6, 1));
        }

        public static void Close(IntPtr hive)
        {
            Check(ORCloseHive(hive));
        }
    }
}
'@).Replace('OFFREG_DLL', $offregPath.Replace('"', '""'))

$OutputPath = [System.IO.Path]::GetFullPath($OutputPath)
$null = [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($OutputPath))
$temporaryPath = "$OutputPath.$([guid]::NewGuid().ToString('N')).tmp"
$hive = [WinGet.InternalCOMRegistry.Hive]::Create()
try
{
    # The package hive is relative to HKLM\Software.
    $classes = 'Classes'
    $executable = '[{AppVPackageRoot}]\WinGetServer\WindowsPackageManagerServer.exe'
    $command = "`"$executable`" --internalPackageManager"

    foreach ($class in $classRegistrations)
    {
        $classId = ([guid]($class.GetAttribute('Id'))).ToString('B').ToUpperInvariant()
        # Local servers can be activated by either client architecture.
        foreach ($classRoot in @("$classes\CLSID", "$classes\Wow6432Node\CLSID"))
        {
            $key = "$classRoot\$classId"
            [WinGet.InternalCOMRegistry.Hive]::SetString($hive, $key, '', $class.GetAttribute('DisplayName'))
            [WinGet.InternalCOMRegistry.Hive]::SetString($hive, $key, 'AppID', $appId)
            [WinGet.InternalCOMRegistry.Hive]::SetString($hive, "$key\LocalServer32", '', $command)
            [WinGet.InternalCOMRegistry.Hive]::SetString($hive, "$key\LocalServer32", 'ServerExecutable', $executable)
        }
    }

    [WinGet.InternalCOMRegistry.Hive]::SetString($hive, "$classes\AppID\$appId", '', 'WinGet package-internal server')
    [WinGet.InternalCOMRegistry.Hive]::SetString($hive, "$classes\AppID\$appId", 'ApplicationId', $application.GetAttribute('Id'))
    [WinGet.InternalCOMRegistry.Hive]::Save($hive, $temporaryPath)
    Move-Item -LiteralPath $temporaryPath -Destination $OutputPath -Force
}
finally
{
    [WinGet.InternalCOMRegistry.Hive]::Close($hive)
    if (Test-Path -LiteralPath $temporaryPath)
    {
        Remove-Item -LiteralPath $temporaryPath
    }
}
