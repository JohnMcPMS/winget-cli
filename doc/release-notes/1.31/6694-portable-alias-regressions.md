## Bug Fixes

### Minor Bug Fixes

* Fix portable archive installations failing when a command alias targets an executable in a subdirectory, including multiple aliases for the same executable.
* Fix portable installations deleting their executable when a command alias differs from the original filename only by letter case.
* Reject conflicting portable archive aliases before changing installed files, preserving existing installations when an upgrade contains a conflict.
* Avoid creating empty portable uninstall registrations or removing an existing installation when preparation fails before installation begins.
* Allow retrying a fresh portable installation when an earlier failure left its uninstall registration completely empty, without automatically overwriting populated registrations.
