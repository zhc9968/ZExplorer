# Releasing ZExplorer

The release process is split into local verification, packaging, and GitHub
publication. GitHub Actions repeats the build on a clean Windows runner.

## 1. Prepare the version

1. Update `VERSION`.
2. Update `FILEVERSION`, `PRODUCTVERSION`, `FileVersion`, and `ProductVersion`
   in `app.rc`.
3. Add the release entry to `CHANGELOG.md`.
4. Update user-facing documentation when behavior has changed.

`tools/package_release.ps1` checks that `VERSION` and `app.rc` agree.

## 2. Build and test locally

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

When a second NTFS volume is available, run the cross-volume suite:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_tests.ps1 -SecondVolumeRoot "Z:\"
x64\Release\CrossVolumeLinkLab.exe "C:\project" "Z:\"
```

Do not publish if the Release build or either test suite fails.

## 3. Package

```powershell
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
```

The command reads `VERSION` and writes:

```text
release\ZExplorer-<version>-win64.zip
release\ZExplorer-<version>-win64.zip.sha256
```

The package contains the main executable, diagnostics, README, changelog,
license, and third-party notices. Dumps, PDB files, local virtual disks, build
intermediates, and machine-specific absolute paths are excluded.

## 4. Smoke test

Extract the ZIP into a clean directory and verify:

1. ZExplorer starts without Visual Studio installed on the test machine.
2. Directory browsing and file operations work.
3. The tray host starts and the application exits cleanly.
4. Shell context-menu registration can be enabled and removed.
5. `--show-about` and `--show-settings` open through the active host.

The binaries are currently unsigned, so Windows SmartScreen may warn on a new
download.

## 5. Publish

Commit the release changes, push `main`, then create and push an annotated tag:

```powershell
git add -A
git commit -m "release: v0.1.0"
git push origin main
git tag -a v0.1.0 -m "ZExplorer v0.1.0"
git push origin v0.1.0
```

The `Release` workflow validates that the tag matches `VERSION`, builds and tests
the project, creates the ZIP and SHA-256 file, and publishes a GitHub Release.

Manual workflow dispatch only builds and uploads workflow artifacts. It does not
create a release without a matching `v*` tag.

## Workflow files

- `.github/workflows/ci.yml`: build and test pushes to `main` and pull requests.
- `.github/workflows/release.yml`: build, test, package, and publish `v*` tags.
