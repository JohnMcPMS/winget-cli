## New Features

### Uninstall user-scope applications from elevated WinGet

Packaged WinGet can use its limited-token COM server to uninstall user-scope EXE-family and MSI/Wix applications while retaining progress, cancellation, and operation results. The limited server owns uninstall preparation, execution, and package tracking.

Explicit installed-version, all-versions, channel, and proxy overrides retain the existing elevation restriction, as do portable and font operations, repair, installation, and upgrades. In-process use without the helper DLL also retains the restriction. Limited and Default tokens, including administrators with UAC disabled, keep their existing behavior.
