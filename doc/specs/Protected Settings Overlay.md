---
author: JohnMcPMS, GitHub Copilot <Copilot>
created on: 2026-10-29
last updated: 2026-10-29
---

# Protected Settings Overlay

## Abstract

Add a per-user, administrator-writable settings file using the existing user
settings schema, whose explicitly configured values override the ordinary user
settings file. Mark selected settings so that their ordinary-file values are
ignored when WinGet runs with the non-default full token, while preserving the
existing settings consumption interface and authoritative in-process settings.

## Inspiration

The ordinary `settings.json` file is writable by a medium-integrity process but
can be read by an elevated process. Its values can influence privileged
filesystem destinations, recursive portable-package deletion, and configuration
module discovery. JSON type and path validation do not establish that an
administrator selected those values.

Ignoring sensitive values in that file prevents administrators from configuring
those settings for elevated use unless an alternative is provided. A protected
file supplies that alternative without introducing a second settings schema.
Applying its explicit values in ordinary processes as well makes the override
consistent across execution contexts.

## Solution Design

### Terminology and scope

| Term | Meaning |
|---|---|
| Ordinary file | Existing medium-writable user settings file and its backup. |
| Protected file | New `admin-settings.json`, writable only with administrative privileges and readable by the current user without elevation. |
| Full token | The condition identified by the existing `Runtime::IsRunningWithNonDefaultFullToken()` helper. |
| Restricted setting | A setting whose ordinary-file value is ignored with a full token. |
| In-process settings | Custom JSON supplied through the existing one-time settings initialization API instead of settings files. |

The full-token helper checks `TokenElevationTypeFull`: the non-default full side
of a split token. Reuse it without adding checks for administrator membership,
integrity level, SYSTEM, or impersonation. This restriction is not defined as a
new universal check for every privileged process.

The protected file is separate from the existing admin capability settings,
such as `InstallerHashOverride`, and does not change those controls.

### Protected storage

Store `admin-settings.json` directly in the existing protected settings
directory, using `PathName::SecureSettingsForRead` for reading and
`PathName::SecureSettingsForWrite` for creation and writes.

The existing paths are:

```text
Packaged:
%PROGRAMDATA%\Microsoft\WinGet\<UserSID>\settings\pkg\<PackageName>\admin-settings.json

Unpackaged:
%PROGRAMDATA%\Microsoft\WinGet\<UserSID>\settings\win\<RuntimeStateName>\admin-settings.json
```

These are per-user locations, not machine-wide settings. Reuse the same path
enum values in both contexts; their existing implementations separate package
identity and unpackaged runtime state. Discover the actual path through
`winget settings export` rather than reconstructing it in management tools.

Reuse the existing administrator ownership, SYSTEM access, and current-user
read permissions. Reads must not create the directory or change permissions.
Primary, backup, temporary files, and their containing directories must retain
protection against medium-token replacement, deletion, or redirection.

The existing `Type::Secure` container stores content separately from protected
verification metadata. This feature reuses the protected directory and its ACL
mechanisms, not that content-plus-hash arrangement. Existing admin capability
settings and source configuration hashes are unchanged.

Use `admin-settings.json.backup` for protected recovery. Mirror the existing
backup-on-open behavior: an elevated CLI open of a successfully loaded primary
can update the backup before editing. Do not back up every write, create a
backup during an unelevated open, or replace a good backup with a primary known
to be invalid.

### Setting classifications

Add explicit treatment metadata to every canonical `SettingMapping`, alongside
its JSON path, type, default, and policy mapping. Initially, only two treatments
are needed: unrestricted and ignore ordinary-file values with a full token.
Do not introduce a general flags framework for hypothetical future treatments.

Require an explicit classification for new settings, including settings
compiled only with test hooks.

The initial restricted set is:

| Setting | Restricted influence |
|---|---|
| `configureBehavior.defaultModuleRoot` | PowerShell module installation, search, and import location. |
| `installBehavior.portablePackageUserRoot` | Portable package destination; user scope does not reduce an already elevated token. |
| `installBehavior.portablePackageMachineRoot` | Machine-scope portable package destination. |
| `installBehavior.defaultInstallRoot` | Default installation destination passed into installer workflows. |
| `downloadBehavior.defaultDownloadDirectory` | Download-only installer and manifest destination. |
| `uninstallBehavior.purgePortablePackage` | Recursive deletion of the recorded portable installation directory. |

Keep all other settings unrestricted initially. Installer preferences select
among otherwise applicable artifacts; extraction settings select fixed
implementations; network settings do not supply endpoints or bypass
verification; log retention settings operate within the selected log location.
These controls affect behavior, but are not automatically equivalent to
supplying a privileged destination or module search path.

### Settings schema and examples

Both files use the existing settings schema. No new settings keys or manifest
schema version are required.

Update the descriptions of all six restricted properties in
`schemas\JSON\settings\settings.schema.0.2.json` to explain that ordinary-file
values are ignored with a non-default full token and direct users to
`winget settings --admin` to configure the protected file. This makes the
restriction discoverable in editors using the shared schema without changing
property types, defaults, or validation.

For example, the ordinary file can contain:

```json
{
  "$schema": "https://aka.ms/winget-settings.schema.json",
  "visual": {
    "progressBar": "rainbow"
  },
  "downloadBehavior": {
    "defaultDownloadDirectory": "C:\\UserDownloads"
  }
}
```

The protected file can contain:

```json
{
  "$schema": "https://aka.ms/winget-settings.schema.json",
  "downloadBehavior": {
    "defaultDownloadDirectory": "C:\\ManagedDownloads"
  }
}
```

The resulting view uses `C:\ManagedDownloads` in both token contexts and retains
the ordinary progress-bar preference. If the protected download setting is
absent, an ordinary-token process uses `C:\UserDownloads`; a full-token process
uses the compiled default and records a migration warning.

An administrator may override unrestricted settings as well. A protected file
containing only selected values does not default the remaining user settings.

### Loading and effective settings

Implement the change within `UserSettings`. Preserve `User().Get<S>()` and the
operational consumers that already use it.

Preserve the existing settings policy gate. If `EnableSettings` disables
settings, retain the early exit and do not load either file.

When in-process content is supplied, use that content instead of loading either
file. Do not filter its values by file classification, overlay protected-file
values, recover from disk on custom parse failure, or emit file migration
warnings. Preserve existing typed validation, policy handling, and one-time
initialization behavior. Its authority is over file loading, not Group Policy.

For file-backed initialization:

1. Select the ordinary primary or backup using existing behavior.
2. Load the protected primary or recover its protected backup.
3. Compose explicit entries at canonical setting granularity, filtering
   restricted ordinary entries when the existing full-token helper returns true.
4. Apply existing typed validation and expose the effective settings view.

Preserve the existing policy-first validation in `Validate<S>()`. It already
selects a mapped Group Policy value before reading JSON; an invalid policy value
uses the default rather than JSON. Do not add a separate policy precedence
layer or change unrelated existing no-valid-JSON behavior.

Absent a mapped policy, file-backed selection is:

| Context | Classification | Protected field absent | Protected field valid | Protected field invalid |
|---|---|---|---|---|
| Ordinary token | Unrestricted | Ordinary value or default | Protected value | Warn; compiled default |
| Ordinary token | Restricted | Ordinary value or default | Protected value | Warn; compiled default |
| Full token | Unrestricted | Ordinary value or default | Protected value | Warn; compiled default |
| Full token | Restricted | Compiled default | Protected value | Warn; compiled default |

An invalid protected field is not the same as an absent field. It blocks
ordinary-file fallback for that setting even in an ordinary-token process;
other valid protected settings still apply.

False, zero, and valid empty collections are explicit values. Arrays replace
the lower-precedence array as a whole; they are not concatenated. Overriding one
nested setting must preserve unrelated siblings.

Keep explicit presence and source information long enough to make these
decisions and produce diagnostics. Do not overlay two fully default-expanded
objects or implement a second schema/parser. Preserve the existing user source
and edit semantics exposed by `GetType()`; track overlay state separately as
needed.

### Recovery and initialization failures

| Protected-file condition | Result |
|---|---|
| Neither primary nor backup exists | Normal absence; continue with eligible ordinary values/defaults. |
| Primary loads successfully | Use primary; do not recover individual invalid fields from backup. |
| Primary is absent, unreadable, or malformed; backup loads | Use protected backup and record a recovery warning. |
| Existing protected configuration cannot be read/parsed or recovered | Fail normal settings initialization; do not proceed without administrator overrides. |
| Individual protected field is invalid | Warn and use its compiled default; retain other valid fields. |

Report source and setting path in diagnostics without unnecessarily exposing
setting values. Do not collapse access failures into successful empty loads.
For restricted full-token settings, no failure or recovery path reactivates an
ordinary-file value.

Keep protected-file opening and management path discovery available for repair
even when its contents would prevent normal initialization. This limited
management path does not relax failure behavior for normal operations.

### CLI management

Add `winget settings --admin` to target the protected file. Plain
`winget settings` continues to target the ordinary file.

An elevated protected open may create the directory/file and prepare its
backup, using the existing schema-linked settings template. An unelevated open
of an existing file launches the editor without creation or backup. Write
protection prevents saving changes without elevation; do not automatically
request UAC elevation or change ACLs.

If an unelevated open finds no file, report its path and explain that elevated
creation is required. Do not launch an editor with an unsaved template.

Reject combinations of `--admin` with existing capability mutation arguments
such as `--enable` and `--disable`. Existing `settings set` and `settings reset`
continue to manage their existing admin capability settings; they do not
silently become JSON file editors.

Extend `winget settings export` with an `adminSettingsFile` string alongside
`userSettingsFile`. Preserve the existing `adminSettings` object. Add the
property to the settings export schema without requiring it from older clients:

```json
{
  "properties": {
    "adminSettingsFile": {
      "type": "string"
    }
  }
}
```

This is an additive change to the export schema, not a settings or manifest
schema change. The export remains a path/capability discovery operation, not
an export of raw file contents or resolved effective values. Its output must
remain valid JSON, including when a settings file requires repair.

### Migration warnings

When a full-token file-backed load ignores a restricted ordinary-file value
without a valid protected replacement, add a setting-specific migration warning.
Identify the key, explain default selection, and point to protected-file
management. Do not print the ignored value.

A valid protected replacement suppresses this migration warning, even if its
value differs. An invalid protected field does not suppress it merely by being
present. Missing ordinary fields, ordinary-token loads, and authoritative
in-process initialization do not generate migration warnings.

Display migration warnings once on every affected elevated CLI invocation,
using a shared reporting point rather than changes to each consumer. Avoid
duplicates when opening settings. Keep the existing visibility of unrelated
settings warnings. Diagnostics must not contaminate structured JSON output.

### COM API and package operations

No IDL or public COM interface changes are required.
`PackageManagerSettings.SetUserSettings` retains its existing in-process,
one-time custom initialization semantics and bypasses both files.

File-backed COM consumers use the same effective settings view. COM remains
non-interactive; initialization failures follow existing exception/HRESULT
propagation rather than prompting or displaying CLI output.

CLI install, upgrade, uninstall, download, configuration, and other existing
settings consumers receive the composed values without changes to their
operational logic. Explicit CLI/API argument precedence, installer launching,
remoting protocols, and resume serialization remain unchanged.

This guarantee applies to values exposed by the view in its initialization
context. It does not redefine the authority of values already selected in a
medium process and subsequently forwarded to an elevated installer or worker.

### PowerShell cmdlets

Add an optional `-Admin` switch to `Get-WinGetUserSetting`,
`Test-WinGetUserSetting`, and `Set-WinGetUserSetting`. It selects the protected
path discovered through `adminSettingsFile`; the default remains the ordinary
file.

Preserve raw-file behavior: Get returns the selected file as a hashtable, Test
returns a Boolean, and Set returns the written settings object. Do not return
the effective overlaid view. Preserve existing `-IgnoreNotSet` and `-Merge`
semantics; management merging is not the runtime overlay operation.

Get and Test may run unelevated. Set requires elevation when targeting the
protected file, including directory/file creation. Do not create backups on Set
or invoke automatic elevation. Pipeline behavior and existing confirmation
behavior remain unchanged; `-Force`, where supported by a calling workflow,
cannot bypass protected-write authorization.

Package cmdlets such as Install/Update/Uninstall/Repair/Find/Show retain their
interfaces and receive applicable effective values through existing consumers.

### WinGet Configuration and DSC

Add an optional Boolean target selector, defaulting to false, to both
`WinGetUserSettings` in the PowerShell DSC module and the native
`UserSettingsFile` resource. Use `Admin` in PowerShell and `admin` in native
JSON input/output. Preserve existing default targeting and Full/Partial action
semantics.

Get/Test/Export describe the selected raw file, not effective settings. Set of
the protected target requires elevation; Get/Test can run unelevated. Return
the selected target in resource state so subsequent operations use the same
file. Do not add an interactive prompt or automatic elevation.

For example, the native resource input can select the protected file:

```json
{
  "admin": true,
  "action": "Partial",
  "settings": {
    "downloadBehavior": {
      "defaultDownloadDirectory": "C:\\ManagedDownloads"
    }
  }
}
```

The PowerShell DSC resource can express the same intent:

```powershell
WinGetUserSettings ProtectedDownloads
{
    SID = ''
    Admin = $true
    Action = 'Partial'
    Settings = @{
        downloadBehavior = @{
            defaultDownloadDirectory = 'C:\ManagedDownloads'
        }
    }
}
```

No package manifest fields are added, so a package manifest YAML example is
not applicable. Configuration workers consume the existing settings view;
this feature does not change configuration elevation/remoting protocols.

### Flow behavior matrix

| Flow or mode | Behavior |
|---|---|
| Normal interactive operation | Resolve the view; display applicable migration warnings; retain existing workflow prompts. |
| `--no-vt` | Same resolution and plain-text warnings; no dependence on color, cursor movement, or terminal images. |
| `--disable-interactivity` or silent package operation | Same resolution, warnings, and failure rules; no new prompts or automatic consent. |
| `settings` | Open ordinary file; preserve existing backup behavior. |
| Elevated `settings --admin` | Open/create protected file and prepare eligible backup. |
| Unelevated `settings --admin`, file exists | Open without creation, backup, automatic elevation, or permission changes. |
| Unelevated `settings --admin`, file absent | Report path and need for elevated creation; do not launch editor. |
| `settings --admin`, file malformed | Remain available for repair; do not replace a good backup with the malformed primary. |
| `settings export`, file malformed | Still report paths/capabilities as JSON for repair tooling. |
| File-backed operation, unrecoverable protected failure | Fail initialization without proceeding on ordinary settings alone. |
| Custom in-process initialization | Do not load either file; retain existing validation/policy semantics. |
| COM | No prompts or CLI output; propagate failures through existing API handling. |
| PowerShell Get/Test with `-Admin` | Read/test protected raw file without elevation. |
| PowerShell Set with `-Admin` | Require elevation; preserve object output and existing merge behavior. |
| DSC protected target | Non-interactive raw-file operations; protected writes require elevation. |
| Settings disabled by policy | Preserve early exit; load neither settings file. |

### Validation and cross-repository impact

Use existing unit and integration test infrastructure to cover:

- Both classifications and token contexts, with each source absent/present.
- Explicit false/zero/empty values, nested sibling preservation, and whole-array
  replacement.
- Invalid protected fields selecting defaults rather than ordinary fallback.
- Primary/backup recovery, unrecoverable failure, and management repair access.
- Authoritative custom initialization and unchanged Group Policy behavior.
- Migration warning presence, suppression, visibility, and duplicate avoidance.
- Protected directory/file permissions, read-only opens, and elevated creation,
  backup, and mutation.
- Additive export schema/property and PowerShell/DSC raw-file target selection.
- Explicit classification coverage for every canonical setting.

No package manifest fields, publisher validation rules, or source protocol
changes are involved. Consequently, no changes are required in winget-create,
winget-cli-restsource, or winget-pkgs validation. Non-interactive package
validation remains unchanged. Client-side settings and management tests belong
in winget-cli; this feature does not depend on downstream publisher adoption.

## UI/UX Design

Add the `--admin` flag to settings help and describe the protected file as
administrator-writable, not as the existing admin capability settings.

The following defines proposed English message formatting; substitute the
actual key and path and localize through the normal resource pipeline.

An affected full-token invocation displays:

```text
Warning: User setting 'downloadBehavior.defaultDownloadDirectory' was ignored because WinGet is running with a full token. The default will be used. Configure this setting with 'winget settings --admin'.
```

An unelevated protected open with no existing file displays:

```text
PS> winget settings --admin
Admin settings file: C:\ProgramData\Microsoft\WinGet\<UserSID>\settings\pkg\<PackageName>\admin-settings.json
The admin settings file does not exist. Run this command from an elevated terminal to create it.
```

Protected backup recovery displays through the existing settings-warning path:

```text
Warning: The admin settings file could not be loaded. Settings were loaded from its protected backup.
```

Unrecoverable normal initialization reports:

```text
The admin settings file could not be loaded and no usable protected backup was found. Run 'winget settings --admin' to repair the file.
```

Error details identify the failing source. Repair/path-discovery operations must
not be blocked by this normal-initialization failure.

`winget settings export` adds the path without changing existing fields:

```json
{
  "adminSettings": {},
  "userSettingsFile": "C:\\Users\\Example\\AppData\\Local\\Packages\\Example\\LocalState\\settings.json",
  "adminSettingsFile": "C:\\ProgramData\\Microsoft\\WinGet\\<UserSID>\\settings\\pkg\\<PackageName>\\admin-settings.json"
}
```

This example abbreviates the existing capability object; actual export retains
its existing entries. Warnings use diagnostic output, not the JSON output stream.

## Capabilities

### Accessibility

Warnings and instructions are plain text and include the affected setting key.
They must not rely on color, VT support, images, or interactive selection.
Existing editor and assistive-technology behavior remains unchanged.

### Security

Restricted values from ordinary files, including their backups, are ineligible
when the existing full-token check applies. Protected values require an
administrator write, and all associated storage retains the existing protected
directory permissions.

> [!IMPORTANT]
> A protected setting authenticates who configured its value; it does not make
> a referenced directory, module, or other payload trustworthy. This feature
> does not claim to resolve every filesystem or installer-specific attack.

Preserve existing hash, certificate, archive scanning, policy, and capability
gates. Do not promote ordinary settings into the protected file automatically.
Authoritative in-process input and explicit options retain their existing trust
contracts; they are not reclassified as ordinary-file input.

### Reliability

Protected backups provide recovery without silently discarding administrator
overrides. Individual invalid values use defaults and warnings; unrecoverable
file failures stop normal initialization. Management access remains available
to diagnose and repair the configuration.

### Compatibility

No setting keys, manifest fields, COM interfaces, or existing default management
targets are removed. Clients without this feature ignore the new file and do
not enforce its restrictions; deployments must not rely on it for older clients.

This intentionally changes full-token behavior for six ordinary-file settings.
Migration warnings and explicit protected configuration provide the transition;
there is no automatic trust promotion. Unrestricted settings retain ordinary
behavior unless explicitly overridden in the protected file.

The additive export property and optional PowerShell/DSC selectors preserve
existing callers. Raw-file management continues to differ from the effective
settings view.

### Performance, Power, and Efficiency

Load the additional small local file once during existing process-static settings
initialization. Read its backup only for recovery, and write backups only during
eligible CLI opens. No polling, network activity, or additional work in individual
settings consumers is introduced.

## Potential Issues

- Users may initially see different elevated paths or portable deletion behavior
  until an administrator configures the protected file.
- A malformed protected file can prevent normal operations; repair access and
  precise source diagnostics must remain independent of successful resolution.
- Shared startup code may currently read settings before identifying a repair
  command. Implementation must preserve management access without weakening
  normal initialization failures.
- Editor association can launch an existing process. Elevation does not guarantee
  that every associated editor can save a protected file; do not relax ACLs or
  automatically elevate an unrelated editor.
- A medium process can resolve values before elevation and forward them through
  an existing protocol. This view-level feature does not change those protocols
  or claim a broader provenance guarantee.
- Treating invalid protected values as absent would incorrectly restore ordinary
  values. Preserve that distinction through composition and validation.
- Backing up a malformed primary could destroy recovery. Backup-on-open must use
  known source/load state rather than copying unconditionally.
- Protected path and raw file discovery must remain correct across package
  identities, runtime-state names, and account contexts.

## Deprecation Path

No settings, APIs, schema fields, or management operations are deprecated or
removed. The intentional restriction on six ordinary-file values is accompanied
by migration warnings and an administrator-configurable alternative, not a
phased schema deprecation. No fixed removal date or publisher migration is
involved.

## Future considerations

- Additional setting classifications, supported by concrete impact analysis.
- Effective-settings inspection with per-value provenance, separate from raw
  file management.
- Further examination of values resolved before crossing an elevation boundary.

These are out of scope. The initial implementation does not add a machine-wide
overlay, automatic migration, or a new custom-settings authority model.

## Resources

- Existing settings documentation: `doc\Settings.md`.
- Settings schema: `schemas\JSON\settings\settings.schema.0.2.json`.
- Export schema: `schemas\JSON\settings\settings.export.schema.0.1.json`.
- Setting definitions: `src\AppInstallerCommonCore\Public\winget\UserSettings.h`.
- Loading and policy-first validation: `src\AppInstallerCommonCore\UserSettings.cpp`.
- Protected paths: `src\AppInstallerCommonCore\Runtime.cpp`.
- Full-token check: `src\AppInstallerSharedLib\Runtime.cpp`.
- CLI management: `src\AppInstallerCLICore\Workflows\SettingsFlow.cpp`.
- PowerShell management: `src\PowerShell\Microsoft.WinGet.Client.Engine\Commands\UserSettingsCommand.cs`.
- Native DSC resource: `src\AppInstallerCLICore\Commands\DscUserSettingsFileResource.cpp`.
- PowerShell DSC resource: `src\PowerShell\Microsoft.WinGet.DSC\Microsoft.WinGet.DSC.psm1`.
