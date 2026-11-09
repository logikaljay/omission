<img src="assets/icon.svg" width="64" height="64" alt="">

# omission

Mission Control for Omarchy. One keystroke shows every workspace and every
window on it, live, and your current workspace zooms out of the screen into
its place among them.

![omission showing six workspaces](assets/screenshot.png)

## Why it's fast

Every choice here comes from measurements on a real Omarchy machine (an M3
MacBook under Asahi), using `OMISSION_DEBUG=1`.

- **It stays resident.** `omission daemon` starts once with the overview
  already built. Opening it is one socket write and a Hyprland state refresh
  of a couple of milliseconds, with no process start and no QML to load.
- **It's a full-screen window, not an overlay.** Under a full-screen layer
  surface, Hyprland redraws every window beneath it on each frame, and on
  this machine that ran at about 30 fps. Under a full-screen window it
  doesn't. A runtime window rule keeps it floating (so tiling never
  reflows), borderless and unanimated. The first frame arrives about 25 ms
  after the keypress.
- **Captures never touch the CPU.** Windows are captured with
  `ext-image-copy-capture` into dmabufs in the GPU's own tiled format.
  Hyprland just renders each window into the buffer. The overview imports
  the buffer as an EGL image, halves it and builds mipmaps on the GPU.
  Shared-memory capture made Hyprland read every window back on its render
  loop, stalling the whole desktop (the cursor included) for ~12 ms per
  window. That path remains only as a fallback.
- **Very little is captured.** The current workspace is captured when the
  overview opens. A hidden workspace is captured once, a moment after you
  leave it, and isn't captured again until it has been on screen. Nothing is
  captured while the overview sits open, and a buffer the compositor hasn't
  finished is never waited on.
- **No per-frame effects.** The blurred backdrop is a tiny decode of your
  wallpaper stretched across the screen.

## Use

| Key | Does |
| --- | --- |
| type | Find windows by title or app |
| `↵` | Go to the selected window |
| `⇧ ↵` / Shift+click | Bring the selected window to this workspace |
| `← → ↑ ↓` | Select the nearest window that way, across workspaces |
| `Tab` / `⇧ Tab` | Select the next / previous window |
| `1` … `9` | Go to that workspace |
| `Del` / `Ctrl+W` | Close the selected window |
| `?` | Show every shortcut |
| `Esc` | Clear the search, or close the overview |

With the mouse: click a window to go to it, or click a workspace to switch
to it. Drag a window onto another workspace to move it there, or onto
**+ New workspace** to give it one of its own. A middle click closes a
window.

When the overview opens, the window you used before the current one is
selected, so `↵` flips back to it the way Alt+Tab does.

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/logikaljay/omission/master/install.sh | sh
```

[`install.sh`](install.sh) is the whole installer, written to be read before
you run it. It downloads the binary for your machine (linux-amd64 or
linux-arm64) from the latest release, checks it against the release's
`SHA256SUMS`, and installs it to `~/.local/bin` with a launcher entry, without
sudo. Then it asks whether to set up:

- **the Mission Control key** (F3 on Apple keyboards, `XF86LaunchA`) and
  **SUPER + D** to open and close the overview
- **three-finger swipes**: up opens it, down closes it, like on a Mac

It also adds omission to Hyprland's autostart, so the overview always opens
instantly, and starts it. A key or swipe that's already used by something else
is left alone. On Arch without Omarchy, it offers to install missing Qt
packages with pacman.

Options go after `sh -s --`: `--yes` answers yes to every question,
`--no-hotkeys`, `--no-gestures` and `--no-autostart` leave those parts out,
and `--version v0.1.0` installs a specific release.

```sh
curl -fsSL https://raw.githubusercontent.com/logikaljay/omission/master/install.sh | sh -s -- --uninstall
```

The Hyprland setup goes between `-- omission: start` and `-- omission: end`
markers in `~/.config/hypr/bindings.lua` and `autostart.lua`. Each file is
backed up beside itself first. If Hyprland reports config errors after the
change, the backups go back. omission also adds its own Hyprland window rule
at runtime (floating, full screen, no animation or border), so uninstalling
leaves nothing behind.

Commands: `omission [toggle|show|hide|daemon|quit]`.

### From source

```sh
bin/install                   # build, install the Arch package, then run install.sh against it
bin/uninstall                 # install.sh --uninstall, then remove the package
bin/uninstall --keep-package  # only remove the Hyprland setup
```

`bin/install` passes its options (`--yes`, `--no-hotkeys`, `--no-gestures`,
`--no-autostart`) on to `install.sh`.

## Develop

```sh
bin/build            # build/omission
bin/test             # unit tests, offscreen
OMISSION_DEBUG=1 build/omission daemon   # traces captures, imports and frame gaps
OMISSION_SHM=1 build/omission daemon     # force the shared-memory fallback
OMISSION_NO_CAPTURE=1 build/omission daemon  # rule captures out when profiling
```

The code is C++ and Qt Quick, built with qmake. `src/capturer.cpp` handles
Wayland capture, `src/model.cpp` turns Hyprland's JSON into workspaces and
the grid, `src/backend.cpp` opens and closes the overview, and
`src/Main.qml` draws the overview and plays the zoom.

GitHub Actions builds linux-amd64 and linux-arm64 binaries on every push
(`.github/workflows/build.yml`), in Ubuntu 26.04 containers. Pushing a `v*`
tag publishes a release with both binaries and their `SHA256SUMS`, which is
what `install.sh` downloads.

## License

MIT
