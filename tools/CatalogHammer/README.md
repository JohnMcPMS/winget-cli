# CatalogHammer

Repro harness for the CET (shadow stack) fail-fast observed in `WindowsPackageManagerServer.exe`
(Watson bug #63388223).

This is a **diagnostic tool**, not a product component. It is not shipped, not referenced by any
other project, and not covered by tests.

## What it does

It hammers the out-of-process COM API's installed-package catalog in a tight, multi-threaded loop,
invalidating WinGet's cached installed index between iterations so that each `Connect()` is forced
to rebuild it.

The rebuild path is where both observed crash buckets live:

| Rebuild step (`PredefinedInstalledSourceFactory.cpp`) | Faulting module in the dumps |
| --- | --- |
| `CreateAndPopulateIndex(Filter::ARP)` → `ManifestMetadataTable::SetMetadataByManifestId` | `winsqlite3` |
| `PopulateIndexFromMSIX` → `Package.DisplayName()` | `BCP47mrm` |

`CachedInstalledIndex::CheckForUpdate()` returns `(!m_index || m_forceNextUpdate)`, so without an
explicit invalidation only the very first `Connect()` builds anything. The harness therefore writes
to `HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall` before each iteration, which trips the
`wil::unique_registry_watcher` installed by `ARPHelper::CreateRegistryWatchers` and sets
`m_forceNextUpdate`.

Iterations that miss the rebuild are still useful: `GetCopy()` performs a full
`SQLiteIndex::CopyFrom` under a shared lock, so every `Connect()` is heavy allocation churn.

## Building

The project references `src\Microsoft.Management.Deployment.Projection`, which in turn references a
`vcxproj`. Use `msbuild` from a Developer Command Prompt, not `dotnet build`:

```
msbuild tools\CatalogHammer\CatalogHammer.csproj /p:Platform=x64 /p:Configuration=Release /restore
```

Easiest alternative: build `src\AppInstallerCLI.sln` first (which you likely do anyway), then build
this project. The `SolutionDir` override in the `.csproj` exists so a standalone build still places
the projection's output where a solution build would.

Output lands in `tools\CatalogHammer\bin\<Platform>\<Configuration>\`.

## Usage

Prepare the VM first (elevated):

```powershell
.\Setup-ReproVM.ps1                 # WER full dumps + CET check + baseline versions
.\Setup-ReproVM.ps1 -AddLanguages   # optional amplifier, see below
.\Setup-ReproVM.ps1 -CheckHardware  # is this machine even CET-capable? run this first
.\Setup-ReproVM.ps1 -CheckServer    # effective shadow stack state of the live server process
.\Setup-ReproVM.ps1 -Cleanup        # removes all WinGetCetRepro_* ARP entries
```

Then run the harness:

```
CatalogHammer.exe --threads 16 --seed-arp 3000
```

| Option | Meaning |
| --- | --- |
| `--threads N` | Worker threads. Default is `max(4, ProcessorCount)`. Higher increases SRW lock contention on `CachedInstalledIndex::m_lock`. |
| `--seed-arp N` | Pre-create N static ARP entries to bloat the index. Makes each rebuild do far more `SetMetadataByManifestId` inserts and forces more SQLite journal growth. |
| `--minutes N` | Stop after N minutes. Default is unbounded; Ctrl+C also stops cleanly. |
| `--dev` | Use the `wingetdev` CLSIDs instead of the production ones. |
| `--allow-lower-trust` | Adds `CLSCTX_ALLOW_LOWER_TRUST_REGISTRATION` to the activation. Needed when running the client elevated. |
| `--stop-on-death` | Halt on the first server disappearance instead of continuing. |

Workers run in **MTA** deliberately, to match the RPC/threadpool dispatch context of the faulting
thread in the dumps.

## Reading the result

A server-side fail-fast surfaces to the client as `RPC_S_SERVER_UNAVAILABLE`, `RPC_E_DISCONNECTED`,
or similar. Those are counted as `serverDeaths` and logged with a timestamp; the process exits with
code 1 if any occurred. When one fires, go to the WER dump in `C:\CetReproDumps`.

Do **not** trust `k` / `kv` on the resulting dump. In every dump analyzed so far the faulting address
had no `.pdata` entry, so the stack walker falls back to leaf rules, reads `[rsp]` (which is `1`) as
the return address, and fabricates every frame after it. Use instead:

```
.ecxr
dx @$curthread.CetBacktrace
dps @rsp L20
.fnent @rip
```

## Troubleshooting

**Every iteration fails with `0x80073D54` ("The process has no package identity") and `iters=0`.**

The usual cause is `Microsoft.Management.Deployment.winmd` not being next to `CatalogHammer.exe`.
WinRT metadata resolution fails and surfaces as `APPMODEL_ERROR_NO_PACKAGE`, which is misleading —
it reads like a packaging problem rather than a missing file. The project copies the `.winmd` from
the projection output directory automatically; confirm it is actually present in
`bin\<Platform>\<Configuration>\`.

If the metadata is present and it still fails:

- Run with `--dev` if you deployed `AppInstallerCLIPackage` rather than using the shipping
  App Installer, since the dev build registers a different CLSID.
- Add `--allow-lower-trust` if the client is elevated.
- Confirm `Microsoft.DesktopAppInstaller` is registered for the *current user*
  (`Get-AppxPackage Microsoft.DesktopAppInstaller`), not just present on the machine.
- Confirm the client architecture matches the installed package.

Resolve this before starting a long run — `iters=0` means nothing is being exercised no matter how
long the harness runs.

## Amplifiers, in priority order

1. `--seed-arp 3000` — more ARP entries means more inserts and more journal growth, which is the
   exact allocation that faulted in the `winsqlite3` bucket.
2. `Setup-ReproVM.ps1 -AddLanguages` — the `BCP47mrm` bucket faulted inside that module's
   language-resolution path, which builds a vector from the user's preferred language list. Ten
   languages instead of one makes that reallocation far more frequent. This is the most narrowly
   targeted amplifier available.
3. Many installed MSIX packages; low core count and high CPU contention.

## Prerequisite: shadow stacks must be active in the server

If the server process is not actually running with user shadow stacks, this cannot reproduce
regardless of load. There are two gates, and hardware is the first one.

### 1. The CPU must support CET

User shadow stacks require **Intel 11th gen (Tiger Lake) or newer**, or **AMD Zen 3 (Ryzen 5000 /
Threadripper PRO 5000) or newer**. On older silicon the feature does not exist, every process
reports `UserShadowStack OFF` no matter how policy is configured, and no amount of load will
produce the fault. In a VM the hypervisor must also expose CET to the guest.

```powershell
.\Setup-ReproVM.ps1 -CheckHardware
```

This reports the CPU and counts how many running processes have shadow stacks active. **Zero out
of everything means the platform cannot do it at all** — move to newer hardware before going
further. A capable, enabled machine has many system processes running with shadow stacks.

### 2. The server process must have them active

Do **not** check this with `Get-ProcessMitigation -Name WindowsPackageManagerServer.exe`. That
reports only what IFEO or policy has been configured for the image name, not a process's effective
mitigations, so it will normally show shadow stacks off even when they are active. Packaged
processes take different defaults from unpackaged ones, and user shadow stacks additionally depend
on the binary being built `/CETCOMPAT` and on every loaded module being compatible.

Check a live process instead:

```powershell
Get-Process WindowsPackageManagerServer | ForEach-Object { (Get-ProcessMitigation -Id $_.Id).UserShadowStack }
```

`Setup-ReproVM.ps1 -CheckServer` does exactly this. The server is demand-started, so run it once
the harness is going rather than beforehand.

The field dumps show shadow stacks enforced rather than in audit mode, so the VM must match.

## The actual goal

The WER dump is the consolation prize. The evidence that no user-mode dump can provide is the raw
hardware `#CP` error code, which distinguishes a shadow-stack return mismatch from the other faults
that share the same exception. Intel documents the error code values in the SDM under control
protection exceptions.

That value is only recorded in kernel state, so capturing it needs a kernel debugger attached to
the VM before the run. Coordinate with the OS team on how to extract it — it is the first thing
they are likely to ask for, and the reason this repro is worth the VM time.
