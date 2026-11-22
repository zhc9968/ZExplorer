# Changelog

All notable changes to ZExplorer are documented in this file.
The version format follows [Semantic Versioning](https://semver.org/).

## [0.1.0] - 2026-11-22

First public Windows x64 release.

### Added

- Explorer-style navigation with breadcrumbs, back/forward history, editable
  addresses, environment-variable expansion, and directory watching.
- Responsive home page, custom quick access, drive cards, special folders, and
  recycle-bin browsing.
- Background task host with progress, cancellation, retries, partial-failure
  handling, task history, and taskbar progress.
- Copy, move, delete, permanent delete, rename, drag and drop, and recursive
  search.
- NTFS alternate data stream management, including create, edit, rename, import,
  export, and delete.
- File lock analysis using Restart Manager and system handle enumeration, with
  individual handle and process actions.
- Extended file attributes, hard links, symbolic links, and junctions, including
  cross-volume link-safe copy and move behavior.
- Windows Explorer shell integration for files, folders, drives, and directory
  backgrounds.
- `.lnk` target resolution and dynamic shortcut-target navigation.
- Light and dark themes, application notification drawer, and accessible control
  tree support.

### Verification

- Single-volume platform suite: 282 passed, 0 failed.
- Cross-volume platform suite with a 512 MB NTFS VHD: 320 passed, 0 failed.
- Cross-volume link laboratory: 23 checks, 0 failures.

[0.1.0]: https://github.com/zhc9968/ZExplorer/releases/tag/v0.1.0
