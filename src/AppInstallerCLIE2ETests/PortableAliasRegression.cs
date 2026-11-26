// -----------------------------------------------------------------------------
// <copyright file="PortableAliasRegression.cs" company="Microsoft Corporation">
//     Copyright (c) Microsoft Corporation. Licensed under the MIT License.
// </copyright>
// -----------------------------------------------------------------------------

namespace AppInstallerCLIE2ETests
{
    using System;
    using System.Collections.Generic;
    using System.ComponentModel;
    using System.IO;
    using System.Linq;
    using System.Runtime.InteropServices;
    using System.Runtime.InteropServices.ComTypes;
    using System.Security.Cryptography;
    using AppInstallerCLIE2ETests.Helpers;
    using Microsoft.Win32;
    using Microsoft.Win32.SafeHandles;
    using NUnit.Framework;

    /// <summary>
    /// Portable alias acceptance tests using the real CLI and generated test installers.
    /// </summary>
    [NonParallelizable]
    public class PortableAliasRegression : BaseCommand
    {
        private const string SymlinkFirstPackage = "AppInstallerTest.NestedPortableAliases";
        private const string PathFirstPackage = "AppInstallerTest.NestedPortableAliasesPath";
        private static readonly string[] Aliases = { "PortableNestedOne.exe", "PortableNestedTwo.exe" };
        private readonly HashSet<string> attemptedPackages = new (StringComparer.OrdinalIgnoreCase);
        private string workDirectory;
        private string installDirectory;
        private string linksDirectory;
        private string initialLinksEntry;
        private int initialLinksIndex;
        private bool initialPathExisted;
        private bool setupComplete;

        /// <summary>
        /// Establish ownership before creating package state or command links.
        /// </summary>
        [SetUp]
        public void Setup()
        {
            this.setupComplete = false;
            this.attemptedPackages.Clear();
            this.linksDirectory = TestCommon.GetPortableSymlinkDirectory(TestCommon.Scope.User);
            foreach (string package in new[] { SymlinkFirstPackage, PathFirstPackage, Constants.PortableExeWithCommandPackageId })
            {
                using RegistryKey registration = Registry.CurrentUser.OpenSubKey(RegistrationPath(package));
                Assert.That(registration, Is.Null, $"Test package already registered: {package}");
            }

            string[] existingLinks = Directory.Exists(this.linksDirectory) ? Directory.GetFileSystemEntries(this.linksDirectory) : Array.Empty<string>();
            foreach (string alias in Aliases.Append(Constants.TestCommandExe))
            {
                Assert.That(existingLinks.Any(link => Path.GetFileName(link).Equals(alias, StringComparison.OrdinalIgnoreCase)), Is.False, $"Command already exists: {alias}");
            }

            using RegistryKey environment = Registry.CurrentUser.OpenSubKey(Constants.PathSubKey_User);
            string rawPath = (string)environment?.GetValue("Path", null, RegistryValueOptions.DoNotExpandEnvironmentNames);
            this.initialPathExisted = rawPath != null;
            string[] entries = (rawPath ?? string.Empty).Split(';');
            this.initialLinksIndex = Array.FindIndex(entries, entry => SamePath(entry, this.linksDirectory));
            this.initialLinksEntry = this.initialLinksIndex < 0 ? null : entries[this.initialLinksIndex];
            this.workDirectory = TestCommon.GetRandomTestDir();
            this.installDirectory = Path.Combine(this.workDirectory, "package");
            this.setupComplete = true;
        }

        /// <summary>
        /// Remove only state whose absence was established by this fixture.
        /// </summary>
        [TearDown]
        public void TearDown()
        {
            try
            {
                if (this.setupComplete)
                {
                    Assert.Multiple(() =>
                    {
                        foreach (string package in this.attemptedPackages)
                        {
                            var result = TestCommon.RunAICLICommand("uninstall", $"--product-code {ProductCode(package)} --scope user --force");
                            Assert.That(result.ExitCode, Is.EqualTo(Constants.ErrorCode.S_OK).Or.EqualTo(Constants.ErrorCode.ERROR_NO_APPLICATIONS_FOUND), result.StdOut);
                        }
                    });
                }
            }
            finally
            {
                if (this.setupComplete)
                {
                    IEnumerable<string> aliases = this.attemptedPackages.Contains(SymlinkFirstPackage) || this.attemptedPackages.Contains(PathFirstPackage) ? Aliases : Array.Empty<string>();
                    if (this.attemptedPackages.Contains(Constants.PortableExeWithCommandPackageId))
                    {
                        aliases = aliases.Append(Constants.TestCommandExe);
                    }

                    foreach (string alias in aliases)
                    {
                        string link = Path.Combine(this.linksDirectory, alias);
                        if (Directory.Exists(link))
                        {
                            Directory.Delete(link, false);
                        }
                        else
                        {
                            File.Delete(link);
                        }
                    }

                    foreach (string package in this.attemptedPackages)
                    {
                        Registry.CurrentUser.DeleteSubKeyTree(RegistrationPath(package), false);
                    }

                    this.RestoreOwnedPathEntries();
                }

                if (Directory.Exists(this.workDirectory))
                {
                    Directory.Delete(this.workDirectory, true);
                }
            }
        }

        /// <summary>
        /// Install, upgrade across PATH modes and executable directories, and uninstall.
        /// </summary>
        /// <param name="startsOnPath">Whether the first version requires its executable directory on PATH.</param>
        [TestCase(false)]
        [TestCase(true)]
        [Category("PortableAliasRegression")]
        public void NestedArchiveLifecycle(bool startsOnPath)
        {
            string package = startsOnPath ? PathFirstPackage : SymlinkFirstPackage;
            this.Install(package);
            this.VerifyInstalled("1.0.0.0", startsOnPath);

            var upgrade = TestCommon.RunAICLICommand("upgrade", $"--id {package} --exact --version 2.0.0.0 --scope user");
            Assert.That(upgrade.ExitCode, Is.EqualTo(Constants.ErrorCode.S_OK), upgrade.StdOut);
            this.VerifyInstalled("2.0.0.0", !startsOnPath);
            Assert.That(Directory.Exists(Path.Combine(this.installDirectory, "nested")), Is.False);
            Assert.That(TestCommon.PathContainsValue(this.PayloadDirectory("1.0.0.0")), Is.False);

            var uninstall = TestCommon.RunAICLICommand("uninstall", $"--product-code {ProductCode(package)} --scope user");
            Assert.That(uninstall.ExitCode, Is.EqualTo(Constants.ErrorCode.S_OK), uninstall.StdOut);
            this.VerifyRemoved(package);
        }

        /// <summary>
        /// A failed nested install can be retried, including recovery of a legacy empty registration.
        /// </summary>
        /// <param name="legacyEmptyKey">Whether to reproduce a legacy empty key before retrying.</param>
        [TestCase(false)]
        [TestCase(true)]
        [Category("PortableAliasRegression")]
        public void NestedArchiveFailureRetry(bool legacyEmptyKey)
        {
            this.attemptedPackages.Add(SymlinkFirstPackage);
            string blocker = Path.Combine(this.linksDirectory, Aliases[0]);
            Directory.CreateDirectory(blocker);
            var failed = TestCommon.RunAICLICommand("install", $"--id {SymlinkFirstPackage} --exact --version 1.0.0.0 --scope user --location \"{this.installDirectory}\"");
            Assert.That(failed.ExitCode, Is.EqualTo(Constants.ErrorCode.ERROR_PORTABLE_INSTALL_FAILED), failed.StdOut);
            Assert.That(Directory.Exists(blocker), Is.True);
            Assert.That(Directory.Exists(this.installDirectory), Is.False);
            using (RegistryKey registration = Registry.CurrentUser.OpenSubKey(RegistrationPath(SymlinkFirstPackage)))
            {
                Assert.That(registration, Is.Null);
            }

            Directory.Delete(blocker, false);
            if (legacyEmptyKey)
            {
                using RegistryKey registration = Registry.CurrentUser.CreateSubKey(
                    RegistrationPath(SymlinkFirstPackage), RegistryKeyPermissionCheck.ReadWriteSubTree, RegistryOptions.Volatile);
                Assert.That(registration.ValueCount, Is.Zero);
                Assert.That(registration.SubKeyCount, Is.Zero);
            }

            this.Install(SymlinkFirstPackage);
            this.VerifyInstalled("1.0.0.0", false);
            var uninstall = TestCommon.RunAICLICommand("uninstall", $"--product-code {ProductCode(SymlinkFirstPackage)} --scope user");
            Assert.That(uninstall.ExitCode, Is.EqualTo(Constants.ErrorCode.S_OK), uninstall.StdOut);
            this.VerifyRemoved(SymlinkFirstPackage);
        }

        /// <summary>
        /// Removing the nested package leaves another package's command and Links PATH entry usable.
        /// </summary>
        [Test]
        [Category("PortableAliasRegression")]
        public void NestedArchivePreservesSharedLinks()
        {
            this.Install(Constants.PortableExeWithCommandPackageId, Path.Combine(this.workDirectory, "foreign"));
            this.Install(SymlinkFirstPackage);
            this.VerifyInstalled("1.0.0.0", false);
            var uninstall = TestCommon.RunAICLICommand("uninstall", $"--product-code {ProductCode(SymlinkFirstPackage)} --scope user");
            Assert.That(uninstall.ExitCode, Is.EqualTo(Constants.ErrorCode.S_OK), uninstall.StdOut);
            this.VerifyRemoved(SymlinkFirstPackage);
            string foreignLink = Path.Combine(this.linksDirectory, Constants.TestCommandExe);
            Assert.That(new FileInfo(foreignLink).LinkTarget, Is.Not.Null);
            Assert.That(TestCommon.PathContainsValue(this.linksDirectory), Is.True);
            this.VerifyCommand(Constants.TestCommandExe, foreignLink);
        }

        private static string ProductCode(string package) => package + "_" + Constants.TestSourceIdentifier;

        private static string RegistrationPath(string package) => Constants.UninstallSubKey + "\\" + ProductCode(package);

        private static bool SamePath(string first, string second) => NormalizePath(first).Equals(NormalizePath(second), StringComparison.OrdinalIgnoreCase);

        private static string NormalizePath(string path) => Environment.ExpandEnvironmentVariables(path).Replace(@"\\?\", string.Empty).TrimEnd('\\', '/');

        private static (uint Volume, uint High, uint Low, uint Links) ReadFileIdentity(string path)
        {
            using SafeFileHandle handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (!GetFileInformationByHandle(handle, out FileInformation information))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), $"Cannot read file identity: {path}");
            }

            return (information.VolumeSerialNumber, information.FileIndexHigh, information.FileIndexLow, information.NumberOfLinks);
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetFileInformationByHandle(SafeFileHandle file, out FileInformation information);

        private void Install(string package, string location = null)
        {
            this.attemptedPackages.Add(package);
            var result = TestCommon.RunAICLICommand("install", $"--id {package} --exact --version 1.0.0.0 --scope user --location \"{location ?? this.installDirectory}\"");
            Assert.That(result.ExitCode, Is.EqualTo(Constants.ErrorCode.S_OK), result.StdOut);
        }

        private string PayloadDirectory(string version) => version == "1.0.0.0" ?
            Path.Combine(this.installDirectory, "nested", "bin") : Path.Combine(this.installDirectory, "replacement", "tools");

        private void VerifyInstalled(string version, bool onPath)
        {
            string directory = this.PayloadDirectory(version);
            string original = Path.Combine(directory, Constants.ExeInstallerFileName);
            Assert.That(File.Exists(original), Is.True, original);
            byte[] expectedHash = SHA256.HashData(File.ReadAllBytes(TestIndex.ExeInstaller));
            Assert.That(SHA256.HashData(File.ReadAllBytes(original)), Is.EqualTo(expectedHash));
            var originalIdentity = ReadFileIdentity(original);
            bool requireHardlinks = new DriveInfo(Path.GetPathRoot(original)).DriveFormat.Equals("NTFS", StringComparison.OrdinalIgnoreCase);
            foreach (string alias in Aliases)
            {
                string aliasFile = Path.Combine(directory, alias);
                Assert.That(File.Exists(aliasFile), Is.True, aliasFile);
                Assert.That(SHA256.HashData(File.ReadAllBytes(aliasFile)), Is.EqualTo(expectedHash));
                var identity = ReadFileIdentity(aliasFile);
                if (requireHardlinks || identity.Links > 1)
                {
                    Assert.That((identity.Volume, identity.High, identity.Low), Is.EqualTo((originalIdentity.Volume, originalIdentity.High, originalIdentity.Low)));
                    Assert.That(identity.Links, Is.GreaterThanOrEqualTo(requireHardlinks ? 3u : 2u));
                }

                string link = Path.Combine(this.linksDirectory, alias);
                string target = new FileInfo(link).LinkTarget;
                if (onPath)
                {
                    Assert.That(target, Is.Null);
                }
                else
                {
                    Assert.That(target, Is.Not.Null);
                    Assert.That(SamePath(target, aliasFile), Is.True, target);
                }

                this.VerifyCommand(alias, onPath ? aliasFile : link);
            }

            Assert.That(TestCommon.PathContainsValue(directory), Is.EqualTo(onPath));
            if (!onPath)
            {
                Assert.That(TestCommon.PathContainsValue(this.linksDirectory), Is.True);
            }
        }

        private void VerifyCommand(string alias, string expectedPath)
        {
            var environment = new Dictionary<string, string>
            {
                ["PATH"] = Environment.GetEnvironmentVariable("Path", EnvironmentVariableTarget.Machine) + ";" +
                    Environment.GetEnvironmentVariable("Path", EnvironmentVariableTarget.User),
            };
            string system = Environment.GetFolderPath(Environment.SpecialFolder.System);
            var lookup = TestCommon.RunProcess(Path.Combine(system, "where.exe"), alias, string.Empty, null, 60000, true, environment);
            Assert.That(lookup.ExitCode, Is.EqualTo(0), lookup.StdOut + lookup.StdErr);
            Assert.That(SamePath(lookup.StdOut.Trim(), expectedPath), Is.True, lookup.StdOut);
            string commandDirectory = Path.Combine(this.workDirectory, "command");
            var execute = TestCommon.RunProcess(Path.Combine(system, "cmd.exe"), "/d /s /c", $"\"{alias} /NoOperation /InstallDir \"{commandDirectory}\"\"", null, 60000, true, environment);
            Assert.That(execute.ExitCode, Is.EqualTo(0), execute.StdOut + execute.StdErr);
        }

        private void VerifyRemoved(string package)
        {
            Assert.That(Directory.Exists(this.installDirectory), Is.False);
            foreach (string alias in Aliases)
            {
                Assert.That(new FileInfo(Path.Combine(this.linksDirectory, alias)).LinkTarget, Is.Null);
                Assert.That(File.Exists(Path.Combine(this.linksDirectory, alias)), Is.False);
            }

            Assert.That(TestCommon.PathContainsValue(this.PayloadDirectory("1.0.0.0")), Is.False);
            Assert.That(TestCommon.PathContainsValue(this.PayloadDirectory("2.0.0.0")), Is.False);
            using RegistryKey registration = Registry.CurrentUser.OpenSubKey(RegistrationPath(package));
            Assert.That(registration, Is.Null);
            if (!Directory.Exists(this.linksDirectory) || !Directory.EnumerateFileSystemEntries(this.linksDirectory).Any())
            {
                Assert.That(TestCommon.PathContainsValue(this.linksDirectory), Is.False);
            }
        }

        private void RestoreOwnedPathEntries()
        {
            var ownedDirectories = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            {
                NormalizePath(this.PayloadDirectory("1.0.0.0")),
                NormalizePath(this.PayloadDirectory("2.0.0.0")),
                NormalizePath(Path.Combine(this.workDirectory, "foreign")),
            };
            bool emptyLinks = !Directory.Exists(this.linksDirectory) || !Directory.EnumerateFileSystemEntries(this.linksDirectory).Any();
            using RegistryKey environment = Registry.CurrentUser.CreateSubKey(Constants.PathSubKey_User);
            string rawPath = (string)environment.GetValue("Path", null, RegistryValueOptions.DoNotExpandEnvironmentNames);
            var entries = (rawPath ?? string.Empty).Split(';').Where(entry =>
                !ownedDirectories.Contains(NormalizePath(entry)) &&
                !(this.initialLinksEntry == null && emptyLinks && SamePath(entry, this.linksDirectory))).ToList();
            if (this.initialLinksEntry != null && !entries.Any(entry => SamePath(entry, this.linksDirectory)))
            {
                entries.Insert(Math.Min(this.initialLinksIndex, entries.Count), this.initialLinksEntry);
            }

            string restored = string.Join(";", entries);
            if (!this.initialPathExisted && restored.Length == 0)
            {
                environment.DeleteValue("Path", false);
            }
            else if (rawPath != restored)
            {
                environment.SetValue("Path", restored, rawPath == null ? RegistryValueKind.ExpandString : environment.GetValueKind("Path"));
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct FileInformation
        {
            public uint FileAttributes;
            public FILETIME CreationTime;
            public FILETIME LastAccessTime;
            public FILETIME LastWriteTime;
            public uint VolumeSerialNumber;
            public uint FileSizeHigh;
            public uint FileSizeLow;
            public uint NumberOfLinks;
            public uint FileIndexHigh;
            public uint FileIndexLow;
        }
    }
}
