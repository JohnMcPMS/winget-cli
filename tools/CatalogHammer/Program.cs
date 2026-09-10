// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//
// CET fail-fast repro harness for WindowsPackageManagerServer.exe (Watson #63388223).
//
// Targets PredefinedInstalledSourceFactory::CachedInstalledIndex::UpdateIndexIfNeeded().
// Each Connect() to the installed catalog either rebuilds the cached index or, at minimum,
// performs a SQLiteIndex::CopyFrom of it. A rebuild runs BOTH observed crash paths:
//
//   CreateAndPopulateIndex(Filter::ARP) -> ManifestMetadataTable::SetMetadataByManifestId
//                                       -> winsqlite3 alloc      (bucket A)
//   PopulateIndexFromMSIX               -> Package.DisplayName()
//                                       -> BCP47mrm alloc        (bucket C)
//
// A rebuild only happens when CachedInstalledIndex::CheckForUpdate() is true, i.e. the index
// is absent or m_forceNextUpdate was set by one of the ARP registry watchers. Hence the
// registry poke between iterations.

using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Management.Deployment;
using Microsoft.Management.Deployment.Projection;
using Microsoft.Win32;

internal static class Program
{
    // HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall is watched by
    // ARPHelper::CreateRegistryWatchers for ScopeEnum::User (ARPHelper.h:20).
    private const string ArpUserPath = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall";
    private const string KeyPrefix = "WinGetCetRepro_";

    // Server death surfaces to the client as one of these.
    private static readonly int[] ServerGoneHResults =
    {
        unchecked((int)0x800706BA), // RPC_S_SERVER_UNAVAILABLE
        unchecked((int)0x800706BE), // RPC_S_CALL_FAILED
        unchecked((int)0x80010108), // RPC_E_DISCONNECTED
        unchecked((int)0x80010105), // RPC_E_SERVERFAULT
    };

    private static long s_iterations;
    private static long s_serverDeaths;
    private static long s_otherErrors;
    private static volatile bool s_stop;
    private static readonly ConcurrentBag<string> s_failures = new();

    private static int Main(string[] args)
    {
        int threads = GetArg(args, "--threads", Math.Max(4, Environment.ProcessorCount));
        int seed = GetArg(args, "--seed-arp", 0);
        int minutes = GetArg(args, "--minutes", 0);
        bool dev = Array.IndexOf(args, "--dev") >= 0;
        bool allowLowerTrust = Array.IndexOf(args, "--allow-lower-trust") >= 0;
        bool stopOnDeath = Array.IndexOf(args, "--stop-on-death") >= 0;

        Console.WriteLine($"threads={threads} seedArp={seed} minutes={(minutes == 0 ? "unbounded" : minutes.ToString())} clsids={(dev ? "Dev" : "Prod")} allowLowerTrust={allowLowerTrust}");

        if (seed > 0)
        {
            SeedArpEntries(seed);
            Console.WriteLine($"Seeded {seed} static ARP entries under HKCU\\{ArpUserPath}\\{KeyPrefix}static_*");
        }

        Console.CancelKeyPress += (_, e) => { e.Cancel = true; s_stop = true; };

        var workers = new Thread[threads];
        for (int i = 0; i < threads; i++)
        {
            int id = i;
            workers[i] = new Thread(() => Worker(id, dev, allowLowerTrust, stopOnDeath)) { IsBackground = true, Name = $"hammer{id}" };
            workers[i].SetApartmentState(ApartmentState.MTA); // match the server-side RPC/threadpool context
            workers[i].Start();
        }

        var sw = Stopwatch.StartNew();
        long last = 0;
        while (!s_stop)
        {
            Thread.Sleep(5000);
            long now = Interlocked.Read(ref s_iterations);
            Console.WriteLine(
                $"[{sw.Elapsed:hh\\:mm\\:ss}] iters={now} (+{now - last}) serverDeaths={Interlocked.Read(ref s_serverDeaths)} otherErrors={Interlocked.Read(ref s_otherErrors)}");
            last = now;

            if (minutes > 0 && sw.Elapsed.TotalMinutes >= minutes)
            {
                s_stop = true;
            }
        }

        foreach (var t in workers)
        {
            t.Join(TimeSpan.FromSeconds(30));
        }

        Console.WriteLine();
        Console.WriteLine($"DONE  iterations={s_iterations}  serverDeaths={s_serverDeaths}  otherErrors={s_otherErrors}");
        foreach (var f in s_failures)
        {
            Console.WriteLine(f);
        }

        return Interlocked.Read(ref s_serverDeaths) > 0 ? 1 : 0;
    }

    private static void Worker(int id, bool dev, bool allowLowerTrust, bool stopOnDeath)
    {
        var factory = new WinGetProjectionFactory(new LocalServerInstanceInitializer
        {
            UseDevClsids = dev,
            AllowLowerTrustRegistration = allowLowerTrust,
        });

        string keyName = $@"{ArpUserPath}\{KeyPrefix}t{id}";
        var rng = new Random(id * 7919 + Environment.TickCount);
        long localIter = 0;

        var pm = factory.CreatePackageManager();

        while (!s_stop)
        {
            try
            {
                // Invalidate the cached index. Any write under the watched subtree sets
                // m_forceNextUpdate via the wil::unique_registry_watcher callback.
                PokeArp(keyName, localIter);

                // Deliberately do NOT wait for the watcher to fire. Racing the notification
                // against the Connect() is closer to the field conditions in the dumps and
                // yields a mix of rebuild and CopyFrom-only iterations.
                if ((localIter & 3) == 0)
                {
                    Thread.Sleep(rng.Next(0, 15));
                }
                var catalogRef = pm.GetLocalPackageCatalog(LocalPackageCatalog.InstalledPackages);

                var connectResult = catalogRef.Connect();

                if (connectResult.Status != ConnectResultStatus.Ok)
                {
                    Interlocked.Increment(ref s_otherErrors);
                    s_failures.Add($"t{id}: Connect status {connectResult.Status}");
                }
                else
                {
                    // Force a traversal so the returned index copy is actually walked.
                    var filters = factory.CreateFindPackagesOptions();
                    var filter = factory.CreatePackageMatchFilter();
                    filter.Field = PackageMatchField.Id;
                    filter.Option = PackageFieldMatchOption.ContainsCaseInsensitive;
                    filter.Value = string.Empty;
                    filters.Filters.Add(filter);

                    var found = connectResult.PackageCatalog.FindPackages(filters);
                    _ = found.Matches.Count;
                }

                localIter++;
                Interlocked.Increment(ref s_iterations);
            }
            catch (COMException ce) when (Array.IndexOf(ServerGoneHResults, ce.HResult) >= 0)
            {
                Interlocked.Increment(ref s_serverDeaths);
                s_failures.Add($"t{id}: SERVER GONE 0x{ce.HResult:X8} at {DateTime.Now:O} — {ce.Message}");
                Console.WriteLine($"*** t{id}: server disappeared, HRESULT 0x{ce.HResult:X8} — check WER LocalDumps ***");

                if (stopOnDeath)
                {
                    s_stop = true;
                }

                Thread.Sleep(2000); // let the server restart before hammering again
            }
            catch (Exception ex)
            {
                Interlocked.Increment(ref s_otherErrors);
                s_failures.Add($"t{id}: {ex.GetType().Name} 0x{ex.HResult:X8} {ex.Message}");
                Thread.Sleep(100);
            }
        }

        TryDeleteKey(keyName);
    }

    private static void PokeArp(string keyName, long iteration)
    {
        using var key = Registry.CurrentUser.CreateSubKey(keyName, writable: true);
        key.SetValue("DisplayName", $"WinGet CET Repro {iteration}", RegistryValueKind.String);
        key.SetValue("DisplayVersion", $"1.0.{iteration % 1000}", RegistryValueKind.String);
        key.SetValue("Publisher", "CetReproHarness", RegistryValueKind.String);
        key.SetValue("InstallLocation", @"C:\CetRepro", RegistryValueKind.String);
    }

    private static void SeedArpEntries(int count)
    {
        for (int i = 0; i < count; i++)
        {
            using var key = Registry.CurrentUser.CreateSubKey($@"{ArpUserPath}\{KeyPrefix}static_{i:D5}", writable: true);
            key.SetValue("DisplayName", $"CET Repro Filler Package {i:D5}", RegistryValueKind.String);
            key.SetValue("DisplayVersion", $"{i % 20}.{i % 13}.{i % 7}", RegistryValueKind.String);
            key.SetValue("Publisher", $"Filler Publisher {i % 97}", RegistryValueKind.String);
            key.SetValue("InstallLocation", $@"C:\CetRepro\Filler\{i:D5}", RegistryValueKind.String);
        }
    }

    private static void TryDeleteKey(string keyName)
    {
        try
        {
            Registry.CurrentUser.DeleteSubKeyTree(keyName, throwOnMissingSubKey: false);
        }
        catch
        {
            // best effort
        }
    }

    private static int GetArg(string[] args, string name, int fallback)
    {
        int i = Array.IndexOf(args, name);
        return (i >= 0 && i + 1 < args.Length && int.TryParse(args[i + 1], NumberStyles.Integer, CultureInfo.InvariantCulture, out int v))
            ? v
            : fallback;
    }
}
