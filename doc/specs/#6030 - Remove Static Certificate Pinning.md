---
author: John McPherson <JohnMcPMS>, GitHub Copilot <Copilot>
created on: 2026-10-07
last updated: 2026-10-07
issue id: 6030
---

# Remove Static Certificate Pinning

"For [#6030](https://github.com/microsoft/winget-cli/issues/6030)"

## Abstract

The Windows Package Manager currently performs static certificate pinning for the Microsoft Store
(`msstore`) source by requiring that one of two hard-coded Microsoft TLS root public keys appear in
the server's certificate chain. This spec proposes removing that static pinning with no
replacement, retaining the already-shipped dynamic validation override for callers that require
their own pinning, and explains why an "is Microsoft certificate" style OS API is not an adequate
substitute.

## Inspiration

Three independent drivers:

1. **Guidance.** Microsoft's published guidance,
   [Certificate pinning and Azure services](https://learn.microsoft.com/azure/security/fundamentals/certificate-pinning),
   recommends against static pinning of publicly trusted TLS server certificates, and states that
   applications connecting to Azure services should rely on standard TLS validation,
   platform-managed trust stores, and Certificate Transparency instead. The same article notes that
   this guidance applies to the public Web PKI — which is exactly where the `msstore` endpoint's
   certificates are issued — and not to private PKIs or controlled trust environments where one
   organization owns the whole certificate lifecycle. WinGet is in the first category, pinning
   against externally governed trust anchors it does not control.

   WinGet's implementation is both static and bespoke — hand-rolled chain walking against
   certificates compiled into the binary, rather than a maintained library — which is the shape of
   pinning this guidance most directly warns about. It also does not meet current Microsoft
   engineering requirements for the practice.

2. **Reliability.** The pinned material is embedded in the client, so it can only change when the
   client ships. When the Store endpoint's certificate chain changes in a way the pinned
   configuration does not anticipate, every already-deployed client fails to reach the source, with
   no client-side mitigation available to the user. This has happened:
   [#3109](https://github.com/microsoft/winget-cli/issues/3109) and
   [#2879](https://github.com/microsoft/winget-cli/issues/2879) are both reports of
   `0x8a15005e APPINSTALLER_CLI_ERROR_PINNED_CERTIFICATE_MISMATCH` breaking the `msstore` source.
   [#6030](https://github.com/microsoft/winget-cli/issues/6030) asks directly for a mechanism that
   does not require a client update when default source certificates change.

3. **The original justification no longer holds.** The pinning was added largely in response to a
   specific partner request. That partner subsequently needed behavior the static configuration
   could not express, so a dynamic override was added for them
   (`PackageCatalogReference.ConnectionValidationHandler`, COM contract 29). The partner scenario is
   now served by the override; the static implementation is carrying no remaining requirement.

It is worth being explicit about what pinning was and was not doing here. The control that prevents
a man-in-the-middle against the `msstore` source is TLS certificate validation — chain building,
trust anchoring, name matching, and revocation — performed by Schannel/WinHTTP. Pinning narrows the
set of acceptable issuers *on top of* that. It is a defense-in-depth measure against a compromised
or coerced public CA, not the primary defense. Every other REST source WinGet talks to already
relies solely on standard TLS validation.

It is also worth being precise about how much narrowing the current configuration actually achieves.
The pin is to the public key of a widely used Microsoft TLS root, with `AnyIssuer` set and partial
chain matching — so it accepts *any* certificate chaining to that root, for any subject. Those roots
issue for a very large number of Microsoft and Azure properties, not solely for
`storeedgefd.dsx.mp.microsoft.com`. The published Azure guidance makes this point generally:
pinning to a shared public CA "doesn't guarantee exclusivity and might not provide the expected
security benefit." An attacker able to obtain a certificate from anywhere under those roots is not
impeded by this pin. The residual benefit is therefore narrower than the implementation's apparent
strictness suggests, while the outage risk is borne in full.

## Solution Design

### What is removed

The static pinning configuration for the Microsoft Store source, constructed in
`GetWellKnownSourceDetailsInternal` in `src/AppInstallerRepositoryCore/SourceList.cpp`:

```cpp
// Removed
PinningChain chain1;
chain1.PartialChain().Root()->
    LoadCertificate(IDX_CERTIFICATE_MS_TLS_ECC_ROOT_G2, CERTIFICATE_RESOURCE_TYPE).
    SetPinning(PinningVerificationType::PublicKey | PinningVerificationType::AnyIssuer | PinningVerificationType::RequireNonLeaf);
// ... and the RSA equivalent
```

The Microsoft Store source is left with a default-constructed `CertificatePinningConfiguration`.
`PinningConfiguration::Validate` already returns `true` for an empty configuration, so no other code
path requires modification to make connections succeed.

Also removed:

- The embedded certificate resources in `src/CertificateResources`
  (`Microsoft_TLS_ECC_Root_G2.crt`, `Microsoft_TLS_RSA_Root_G2.crt`) and their resource IDs.
- The `winget debug validate-store-pinning` command, whose only purpose was to test a rotated server
  certificate against the static configuration ahead of deployment.
- In a follow-up change, the static-pinning machinery itself — `PinningDetails`, `PinningChain`,
  `PinningVerificationType`, and the associated JSON loading — which has no remaining production
  consumer. This is deliberately a separate change so the compliance-relevant removal stays small.

### What is retained

**The dynamic validation override.** `PackageCatalogReference.ConnectionValidationHandler` and
`IsConnectionValidationHandlerEnabled` are unchanged. A caller that requires pinning implements it
itself, against live certificate material, with its own update cadence. The plumbing
(`ISourceReference::SetServerCertificateValidationCallback` →
`CallbackPinningChainValidation` → `HttpClientHelper::SetPinningConfiguration`) is unchanged, as is
the restriction to in-process callers (out-of-process callers receive `E_ACCESSDENIED`).

**`APPINSTALLER_CLI_ERROR_PINNED_CERTIFICATE_MISMATCH` (`0x8A15005E`).** It remains the error
surfaced when a caller-supplied handler rejects a certificate. It is documented in
`doc/windows/package-manager/winget/returnCodes.md` and removing it would be a breaking change for
no benefit. After this change the error can only originate from caller-supplied validation, never
from WinGet's own configuration.

### Group policy and admin setting

`BypassCertificatePinningForMicrosoftStore` (policy value name
`EnableBypassCertificatePinningForMicrosoftStore`) is **retained with a narrowed meaning**.

Today it does two things: it suppresses the static pinning, and it gates whether an in-process
caller may install its own `ConnectionValidationHandler` for the Store catalog. After this change
only the second remains, and that is still a meaningful administrative control — it decides whether
a caller may add on its own judgment after the system's TLS validation.

| Policy state | Behavior today | Behavior after this change |
| --- | --- | --- |
| Not Configured (default) | Static pinning enforced; handler may be set | No pinning; handler may be set |
| Enabled | Static pinning suppressed; handler may be set | No pinning; handler may be set |
| Disabled | Static pinning enforced; handler **cannot** be set for `msstore` | No pinning; handler **cannot** be set for `msstore` |

The registry value name, the admin setting name, the settings export schema entry, the DSC admin
settings resource property, and the PowerShell `Enable-/Disable-/Get-WinGetSetting` accepted values
are all unchanged, so no existing administrative configuration breaks. The user-facing description
strings in `winget.resw` and in the ADMX/ADML files are updated to describe the narrowed meaning.

Deleting the policy outright was considered and rejected: it would remove the administrative gate on
who may override connection validation, which is a security regression introduced by a change made
for security reasons.

### COM API surface changes

None. No IDL signature changes, therefore no contract version bump. Only the documentation comments
on `ConnectionValidationHandler` and `IsConnectionValidationHandlerEnabled` change, since they
currently describe the policy in terms of pinning.

### PowerShell cmdlet changes

None functionally. `Enable-WinGetSetting`, `Disable-WinGetSetting`, and `Get-WinGetSetting` continue
to accept and report `BypassCertificatePinningForMicrosoftStore`. Help text is updated where it
describes the setting's effect.

### Settings changes

No schema change. `schemas/JSON/settings/settings.export.schema.0.1.json` retains the
`BypassCertificatePinningForMicrosoftStore` property. An existing `settings.json` or admin settings
file that sets it continues to parse; the value simply no longer influences pinning.

### Manifest schema changes

None. This change does not touch manifests, and therefore requires no manifest schema version bump
and has no `winget-pkgs` validation pipeline impact.

### Cross-repository impact

None. No schema fields are added or removed, so `winget-create`, `winget-cli-restsource`, and
`winget-pkgs` are unaffected.

### Rejected alternative: an "is Microsoft certificate" OS check

The most plausible replacement is `CertVerifyCertificateChainPolicy` with
`CERT_CHAIN_POLICY_MICROSOFT_ROOT`. It is dynamic, OS-maintained, and superficially expresses the
same intent as the current configuration ("this chain terminates in a Microsoft root"). It is not
adopted here, as it describes a different trust hierarchy. `CERT_CHAIN_POLICY_MICROSOFT_ROOT` is defined
against the Microsoft Root Certificate Program as used for **code signing** — it is the check
behind `WinVerifyTrust` deciding that a binary is Microsoft-signed. The Store endpoint's TLS
certificates are issued through Azure front-end CAs, which are governed and rotated
independently. There is no contract that an Azure TLS chain satisfies the code-signing root
policy, now or after the next rotation.

## UI/UX Design

For the ordinary user, the visible change is the absence of a failure. A user on a client whose
pinned configuration no longer matches the server currently sees:

```
Failed when searching source: msstore
An unexpected error occurred while executing the command:
0x8a15005e : The server certificate did not match any of the expected values.
```

After this change that error cannot be produced by WinGet's own configuration, and such a search
succeeds.

`winget --info` and `winget settings` continue to list
`BypassCertificatePinningForMicrosoftStore` among admin settings and policies, with updated
description text.

`winget debug validate-store-pinning` is removed. `winget debug` is not a supported end-user
surface, so this is not a breaking change to the published CLI.

## Capabilities

### Accessibility

No impact. No new or changed interactive surfaces, prompts, or output formatting beyond the removal
of one debug subcommand and revised policy description strings.

### Security

This is a net reduction in defense-in-depth, accepted deliberately.

What is lost: protection against an attacker who can obtain a mis-issued certificate not chaining to one
of the two pinned Microsoft TLS roots *and* can intercept the connection.

What is unchanged: full TLS validation by Schannel/WinHTTP — chain building to the system trust
store, hostname matching, expiry, and revocation checking — continues to apply to every connection.
These are the controls that actually prevent a man-in-the-middle, and they are the ones Microsoft's
published guidance directs applications to rely on.

What else constrains the threat, independently of WinGet: Certificate Transparency means publicly
trusted certificates must be logged in public append-only logs to be accepted by major clients,
making mis-issuance for a Microsoft domain detectable rather than silent. CAA records, root program
requirements, and CA/Browser Forum baseline requirements further constrain who may issue. All of
these keep working after this change, and none of them depend on the client pinning anything.

What offsets it: a caller with a genuine requirement for stronger validation retains
`ConnectionValidationHandler`, and can implement pinning correctly — against live material, updated
on its own schedule, without a WinGet release in the loop. Administrators retain the ability to
forbid that override.

The judgment is that aligning with published guidance, and eliminating a class of self-inflicted
outage, outweighs a defense-in-depth measure that has never been observed to block an attack but has
repeatedly blocked legitimate use.

### Reliability

This is the clearest win. It eliminates an entire failure class in which a server-side certificate
change bricks the `msstore` source on every already-shipped client, with the only remedy being a
client update that cannot be delivered through the broken source. Issues
[#2879](https://github.com/microsoft/winget-cli/issues/2879) and
[#3109](https://github.com/microsoft/winget-cli/issues/3109) are instances of this.

It also removes a standing operational obligation: tracking Azure CA rotation plans and shipping
client updates ahead of them.

### Compatibility

No API, schema, settings, or manifest compatibility breaks.

- Existing group policy and admin settings configurations continue to apply; only the effect of the
  `Disabled` state narrows.
- No COM contract version bump; no IDL signature changes.
- `APPINSTALLER_CLI_ERROR_PINNED_CERTIFICATE_MISMATCH` is retained.
- The removed `winget debug validate-store-pinning` is on an unsupported diagnostic surface.

The one behavioral break worth stating plainly: a caller that relied on WinGet's built-in pinning,
and never adopted `ConnectionValidationHandler`, silently loses that protection. There is no
runtime signal for this; it must be communicated in release notes and directly to known partners.

### Performance, Power, and Efficiency

Marginally positive and practically unmeasurable. Each connection to the Store source no longer
builds and walks a certificate chain for pinning evaluation, and the client binary no longer carries
two embedded certificates.

## Potential Issues

- **Partner surprise.** The main risk. Mitigation is release-note visibility plus direct
  notification of the partner that originally requested pinning, confirming they are on the
  `ConnectionValidationHandler` path.
- **Optics.** "WinGet removed a security feature" reads badly without context. This spec is the
  durable record of the reasoning and should be linked from the release notes and the PR.
- **Residual naming confusion.** A policy named `BypassCertificatePinningForMicrosoftStore` that no
  longer controls pinning is a wart. Renaming it would break existing administrative configuration,
  which is worse. The description strings carry the clarification.
- **Partially stale translations.** Changing the English `<value>` queues retranslation, but
  localized ADML policy text under `Localization/Policies/**` is synced separately and will describe
  the old behavior until the next sync.
- **Reversal cost.** If a future requirement reinstates pinning, it should be built on an approved
  library with a dynamic update mechanism, not by reverting this change.

## Deprecation Path

Not applicable in the usual sense — no manifest field or public API is being replaced, so the
phased Introduction → Transition → Deprecation → Removal model does not apply. The static pinning is
an internal implementation detail with no public contract, and it is removed in a single release.

The one element with a deprecation-like character is the group policy, and it is explicitly *not*
deprecated: it is retained indefinitely with a narrowed meaning, precisely to avoid breaking
existing administrative configuration. WinGet 1.X makes no breaking changes.

## Future considerations

- **A dynamic trust-material mechanism.** [#6030](https://github.com/microsoft/winget-cli/issues/6030)
  asks for default source certificate information that can be updated without a client update.
  Removing static pinning resolves the issue's immediate pain. If a future requirement genuinely
  needs source-specific trust constraints, the right shape is server-delivered, signed, expiring
  trust material evaluated through an approved library — not compiled-in certificates.
- **Uniform source validation policy.** With `msstore` no longer special-cased, all REST sources are
  validated identically. That makes it practical to reason about, document, and test source
  transport security as one behavior.
- **Broader use of `ConnectionValidationHandler`.** The override is currently in-process only. If
  additional partners need it, the out-of-process restriction and the administrative gate are the
  levers to revisit, with their own spec.

## Resources

- [#6030 — Default source certificate updates shouldn't require an update to the WinGet Client](https://github.com/microsoft/winget-cli/issues/6030)
- [#3109 — The current msstore certificate chain is rejected by winget CLI](https://github.com/microsoft/winget-cli/issues/3109)
- [#2879 — "0x8a15005e : The server certificate did not match any of the expected values."](https://github.com/microsoft/winget-cli/issues/2879)
- [Certificate pinning and Azure services](https://learn.microsoft.com/azure/security/fundamentals/certificate-pinning)
  — Microsoft's published position on static pinning of publicly trusted TLS certificates
- [`CertVerifyCertificateChainPolicy`](https://learn.microsoft.com/windows/win32/api/wincrypt/nf-wincrypt-certverifycertificatechainpolicy)
  and `CERT_CHAIN_POLICY_MICROSOFT_ROOT` — the evaluated and rejected alternative
- [RFC 7469 — Public Key Pinning Extension for HTTP](https://datatracker.ietf.org/doc/html/rfc7469)
  — the web's pinning mechanism, since removed from all major browsers after repeated self-inflicted
  outages; useful background on why static pinning fell out of favor industry-wide
- [RFC 9162 — Certificate Transparency Version 2.0](https://datatracker.ietf.org/doc/html/rfc9162)
  — one of the ecosystem controls that constrains mis-issuance without client-side pinning
- `doc/specs/#190 - Proxy Support.md` — references Store source pinning; updated by this change
- `doc/windows/package-manager/winget/returnCodes.md` — `APPINSTALLER_CLI_ERROR_PINNED_CERTIFICATE_MISMATCH`
