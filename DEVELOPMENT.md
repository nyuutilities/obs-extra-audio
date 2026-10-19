# OBS Extra Audio: development

This file covers building, packaging and installing the plugin from source. For what the plugin does and how to use it, see [README.md](README.md).

## What a release contains

`scripts\package.ps1` makes two ZIPs in `dist\`:

| Package | Contents | Who it's for |
|---|---|---|
| `obs-extra-audio-<version>-windows-x64.zip` | Only the files OBS loads, in OBS's own folder layout. The README, license and checksums are inside the plugin's data folder. | Anyone using the plugin: they extract it into their OBS folder. |
| `obs-extra-audio-<version>-source.zip` | `src\`, `data\`, `scripts\`, this file, the README and the license: the same files as this repository. | Anyone who wants the source. The plugin is GPL-2.0-or-later, so whoever gets the user ZIP must also be able to get this. |

The user ZIP's layout:

```text
obs-plugins\64bit\obs-extra-audio.dll                  the plugin
bin\64bit\obs-extra-audio-mux.exe                      adds the extra tracks to each MP4 after Stop (uses OBS's own FFmpeg)
data\obs-plugins\obs-extra-audio\manifest.json         name and version for Tools > Plugin Manager
data\obs-plugins\obs-extra-audio\locale\en-US.ini      panel and status text
data\obs-plugins\obs-extra-audio\README.md, LICENSE, SHA256SUMS.txt
```

**What OBS needs at run time is only those four files.** Everything else in this repository is for building and installing:

- **`src\`** builds the two binaries:
  - 12 C files and `qt-ui.cpp` make the DLL;
  - `mp4-finalizer.c` and `mp4-inplace.c` make the helper.

  Each `.c` file holds one part of the plugin's code. Its `.h` header declares what other parts may call (`aac-recorder.c` with `aac-recorder.h`, for example). They are pairs, not two versions, and both are needed to build.
- **`scripts\`** builds, installs and packages.

## Requirements

- Windows x64.
- Portable OBS Studio **32.2.2** or newer. The plugin is tested with 32.2.2 (`EA_TESTED_OBS` in `src\routing.h`). It refuses older versions, and loads on newer ones with a "not yet tested" note in its Status box.
- Optional: Aitum Vertical **1.6.6** or newer, for vertical recordings and Backtrack clips. It's tested with 1.6.6 (`EA_TESTED_AITUM`); a newer one is used with a "not yet tested" note. With an older Aitum, or none, the plugin works on the main canvas only.

## Build

`scripts\bootstrap.ps1` and `scripts\bootstrap-ui.ps1` download everything the build needs into `.deps\`. Each download is pinned and checksum-verified: a portable Microsoft C/C++ compiler (no Visual Studio install), Zig, Qt 6.11.1, and the OBS 32.2.2 and FFmpeg 8.0 headers. They skip anything already unpacked. Nothing is installed on the system.

The build links against the DLLs of a portable OBS 32.2.2, so pass its folder with `-ObsRoot`:

```powershell
.\scripts\build.ps1 -ObsRoot <OBS folder>    # the plugin and its helper, in build\
.\scripts\package.ps1                        # user ZIP and source ZIP in dist\, from what build.ps1 last built
```

`build\` holds everything generated, and `build.ps1` recreates it.

## Install from source

Close OBS, then:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -ObsRoot <OBS folder> -Profile <profile>
```

- **Source.** By default it installs what `scripts\build.ps1` last built in `build\`. `-From <folder>` installs an extracted user ZIP instead.
- **Refusals.** It won't run while that OBS is open, and needs portable OBS 32.2.2 or newer. It warns about a newer, untested OBS, and about an Aitum that's older (no vertical extra tracks) or newer (not yet tested) than 1.6.6.
- **What it copies.** Only the plugin's own files, never profiles, scenes or Aitum settings.
- **Backups.** It first backs up the files it replaces to `backups\<date-time-id>\`, with a `manifest.json`. To undo the install, with OBS closed, run `scripts\uninstall.ps1 -Manifest backups\<date-time-id>\manifest.json`. It refuses if any of those files changed since.

Restart OBS. The log should show `[extra-audio] Loaded <version>`, and Tools > Plugin Manager must have **OBS Extra Audio** enabled.

## Making a release

1. Bump the version in `src\routing.h` (`EA_VERSION`), `data\manifest.json` and the README's heading. `package.ps1` refuses a README whose heading names another version.
2. Build, install into a test copy of OBS, and check recordings, pauses and replay clips on both canvases.
3. Run `package.ps1`. It refuses to overwrite an existing package.

**The build deliberately doesn't use OBS's CMake plugin template.** OBS loads any DLL with the standard module entry points, however it was built. The template exists for cross-platform source releases with automated builds, and needs CMake, Visual Studio and several gigabytes of OBS dependencies and Qt. This plugin is Windows-only and distributed as DLLs, so the template would add downloads without changing the plugin.

The build follows the template conventions that affect the plugin itself:

- OBS's module and locale macros, and `data/locale/en-US.ini`;
- the standard `obs-plugins/64bit` and `data/obs-plugins/obs-extra-audio` layout;
- Plugin Manager metadata;
- a load-time check that refuses an OBS older than the one it was built against.

**OBS internals.** One file, `src\native-pause-clock.c`, reads two values that aren't part of OBS's plugin interface: an audio encoder's pause start and paused total, from OBS 32.2.2's `obs-internal.h`. Before using the start, it checks that the paused total matches OBS's public value and that the start is close to now. If either check fails, as with a changed layout in another OBS version, pauses are placed the way OBS places them, on its public video clock (`ea_pause_grid_start`). The Status box and log then say so.

Undeclared functions are compile errors (`/we4013`).
