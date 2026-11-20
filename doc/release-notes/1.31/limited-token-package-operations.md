## New Features

### Delegate user-scope package operations from elevated WinGet

Packaged WinGet can use its limited-token COM server for user-scope uninstall, repair, uninstall-previous upgrades, portable installation and upgrades, and font installation and removal. The limited server owns preparation, execution, rollback, and package tracking; the caller retains progress, cancellation, and operation results.

Unpackaged in-process use without the helper DLL retains the existing elevation restriction. Default tokens, including administrators with UAC disabled, keep their existing behavior. Installer hash-override restrictions and manifests that prohibit elevation are unchanged.

Delegated installs use catalog package/version identity and typed installer-selection options, including architecture order, installer type, scope, and locale. No manifest contents are passed through COM. Affected CLI local-manifest operations retain the existing elevation restriction.

Delegated operations currently reject explicit `--proxy` and `--no-proxy` overrides rather than ignoring them.
