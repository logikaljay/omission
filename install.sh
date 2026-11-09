#!/bin/sh
# Installs omission, Mission Control for Omarchy, and sets up Hyprland for it.
#
#   curl -fsSL https://raw.githubusercontent.com/logikaljay/omission/master/install.sh | sh
#
# What it does, in order (read on for each step):
#   1. Downloads the binary for your machine from the latest GitHub release
#      and checks it against the release's SHA256SUMS.
#   2. Installs it to ~/.local/bin, with a launcher entry and icon under
#      ~/.local/share. No sudo, unless Qt packages are missing and you agree
#      to install them with pacman.
#   3. Asks before adding each of these to ~/.config/hypr:
#        - the Mission Control key (F3 on Apple keyboards) and SUPER + D
#        - three-finger swipes: up opens omission, down closes it
#      and starts omission with Hyprland. A key that's already bound to
#      something else is left alone.
#   4. Starts omission.
#
# Options (after `sh -s --` when piped):
#   --yes            answer yes to every question
#   --no-hotkeys     don't bind the Mission Control key or SUPER + D
#   --no-gestures    don't add the three-finger swipes
#   --no-autostart   don't start omission with Hyprland
#   --version vX.Y.Z install that release instead of the latest
#   --binary PATH    use an omission already at PATH (a source build)
#                    instead of downloading one
#   --uninstall      stop omission, remove its Hyprland setup and its files
set -eu

REPO="logikaljay/omission"
PREFIX="${OMISSION_PREFIX:-$HOME/.local}"
HYPR="${OMISSION_HYPR_DIR:-$HOME/.config/hypr}"
BINDINGS="$HYPR/bindings.lua"
AUTOSTART="$HYPR/autostart.lua"
DESKTOP="$PREFIX/share/applications/omission.desktop"
ICON="$PREFIX/share/icons/hicolor/scalable/apps/omission.svg"
DESCRIPTION="Mission control (omission)"
STAMP="$(date +%s)"

BIN="$PREFIX/bin/omission"
download=1
version="latest"
assume_yes=0
hotkeys=yes
gestures=yes
autostart=yes
uninstall=0

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
say() { printf '%s\n' "$*"; }
die() { printf 'omission: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --yes|-y) assume_yes=1 ;;
    --no-hotkeys) hotkeys=no ;;
    --no-gestures) gestures=no ;;
    --no-autostart) autostart=no ;;
    --version) [ $# -ge 2 ] || die "--version needs a value"; version="$2"; shift ;;
    --version=*) version="${1#--version=}" ;;
    --binary) [ $# -ge 2 ] || die "--binary needs a path"; BIN="$2"; download=0; shift ;;
    --binary=*) BIN="${1#--binary=}"; download=0 ;;
    --uninstall) uninstall=1 ;;
    -h|--help) sed -n '2,29p' "$0" 2>/dev/null | sed 's/^# \{0,1\}//' || true; exit 0 ;;
    *) die "unknown option $1" ;;
  esac
  shift
done

# ── Helpers ────────────────────────────────────────────────────────────────

# Questions go to the terminal even when this script arrives through a pipe.
# With no terminal (or --yes) every answer is yes.
ask() {
  # A subshell: a failed redirection on a builtin would end this script.
  if [ "$assume_yes" = 1 ] || ! (: </dev/tty) 2>/dev/null; then
    return 0
  fi
  printf '%s [Y/n] ' "$1" >/dev/tty
  read -r answer </dev/tty || answer=""
  case "$answer" in
    [nN]*) return 1 ;;
    *) return 0 ;;
  esac
}

# OMISSION_NO_RELOAD keeps the running session out of it, for testing.
hyprland_running() {
  [ -z "${OMISSION_NO_RELOAD:-}" ] && [ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] && command -v hyprctl >/dev/null 2>&1
}

stop_daemon() {
  hyprland_running || return 0
  for candidate in "$BIN" omission; do
    command -v "$candidate" >/dev/null 2>&1 && "$candidate" quit 2>/dev/null || true
  done
  sleep 0.3
}

# Starts omission through Hyprland, the way autostart will, so it gets the
# session's environment.
start_daemon() {
  hyprland_running || return 0
  if command -v uwsm-app >/dev/null 2>&1; then
    hyprctl dispatch "hl.dsp.exec_cmd(\"uwsm-app -- $BIN daemon\")" >/dev/null
  else
    hyprctl dispatch "hl.dsp.exec_cmd(\"$BIN daemon\")" >/dev/null
  fi
}

# ── Hyprland config ────────────────────────────────────────────────────────
#
# omission's setup is one block per file, between "-- omission: start" and
# "-- omission: end", appended to your own bindings.lua and autostart.lua.
# Nothing outside those markers is touched. Each file is backed up beside
# itself (file.bak.<time>) first, and if Hyprland reports config errors after
# the change, the backups go back.

backed_up=""

backup() {
  [ -f "$1" ] || return 0
  cp -p "$1" "$1.bak.$STAMP"
  backed_up="$backed_up $1"
}

restore() {
  for file in $backed_up; do
    mv -f "$file.bak.$STAMP" "$file"
  done
}

# Removes an earlier omission block, so installing twice changes nothing.
remove_block() {
  [ -f "$1" ] && grep -q '^-- omission: start' "$1" || return 0
  awk '/^-- omission: start/ { skip = 1; next } /^-- omission: end/ { skip = 0; next } !skip' "$1" \
    | awk 'NF { for (; blank > 0; blank--) print ""; print; next } { blank++ }' > "$1.omission-new"
  cat "$1.omission-new" > "$1"
  rm -f "$1.omission-new"
}

add_block() {
  file="$1"
  body="$2"
  mkdir -p "$(dirname "$file")"
  [ -f "$file" ] || : > "$file"
  if [ -s "$file" ]; then
    [ "$(tail -c 1 "$file" | od -An -c | tr -d ' ')" = '\n' ] || printf '\n' >> "$file"
    printf '\n' >> "$file"
  fi
  printf -- '-- omission: start\n%s\n-- omission: end\n' "$body" >> "$file"
}

reload_or_restore() {
  hyprland_running || return 0
  hyprctl reload >/dev/null
  errors="$(hyprctl configerrors 2>/dev/null | grep -v '^[[:space:]]*$' || true)"
  if [ -n "$errors" ]; then
    restore
    hyprctl reload >/dev/null
    die "Hyprland reported config errors, so your files were put back:
$errors"
  fi
}

# What a key is bound to now, other than omission; empty when it's free.
# Takes Hyprland's modifier mask (SUPER is 64) and the key name.
key_taken_by() {
  hyprland_running && command -v jq >/dev/null 2>&1 || return 0
  hyprctl binds -j | jq -r --argjson mask "$1" --arg key "$2" --arg mine "$DESCRIPTION" '
    .[] | select(.modmask == $mask and (.key | ascii_upcase) == ($key | ascii_upcase) and .description != $mine)
        | if (.description // "") != "" then .description else .dispatcher + " " + .arg end' | head -n 1
}

# Whether a three-finger swipe this way ("up" or "down"), or a vertical one,
# is already set in your config or Omarchy's.
swipe_taken() {
  for dir in "$HYPR" "${OMARCHY_PATH:-/usr/share/omarchy}/default/hypr"; do
    [ -d "$dir" ] || continue
    grep -rhE --include='*.lua' "gesture\\(\\{[^}]*fingers *= *3[^}]*direction *= *\"($1|vertical)\"" "$dir" 2>/dev/null \
      | grep -vE '^[[:space:]]*--' | grep -qv 'omission' && return 0
  done
  return 1
}

configure_hyprland() {
  if [ ! -d "$HYPR" ]; then
    say "No Hyprland config in $HYPR, so no keys or gestures were set up."
    return 0
  fi
  backup "$BINDINGS"
  backup "$AUTOSTART"
  remove_block "$BINDINGS"
  remove_block "$AUTOSTART"

  lines=""
  add_line() { lines="${lines:+$lines
}$1"; }

  if [ "$hotkeys" = yes ]; then
    # Apple's Mission Control key arrives as KEY_SCALE, which XKB calls
    # XF86LaunchA. Keyboards without it never send it.
    taken="$(key_taken_by 0 XF86LaunchA)"
    if [ -n "$taken" ]; then
      say "The Mission Control key is already bound to \"$taken\"; leaving it alone."
    else
      add_line "-- The Mission Control key (F3 on Apple keyboards)."
      add_line "o.bind(\"XF86LaunchA\", \"$DESCRIPTION\", \"$BIN toggle\")"
      say "Bound the Mission Control key."
    fi
    taken="$(key_taken_by 64 D)"
    if [ -n "$taken" ]; then
      say "SUPER + D is already bound to \"$taken\"; leaving it alone."
    else
      add_line "o.bind(\"SUPER + D\", \"$DESCRIPTION\", \"$BIN toggle\")"
      say "Bound SUPER + D."
    fi
  fi

  if [ "$gestures" = yes ]; then
    if swipe_taken up; then
      say "A three-finger swipe up is already set; leaving it alone."
    else
      add_line "-- Three-finger swipe up opens it, like Mission Control on a Mac."
      add_line "hl.gesture({ fingers = 3, direction = \"up\", action = function() hl.exec_cmd(\"$BIN show\") end })"
      say "Added a three-finger swipe up to open."
    fi
    if swipe_taken down; then
      say "A three-finger swipe down is already set; leaving it alone."
    else
      add_line "-- Three-finger swipe down closes it (and does nothing when it's closed)."
      add_line "hl.gesture({ fingers = 3, direction = \"down\", action = function() hl.exec_cmd(\"$BIN hide\") end })"
      say "Added a three-finger swipe down to close."
    fi
  fi
  [ -n "$lines" ] && add_block "$BINDINGS" "$lines"

  if [ "$autostart" = yes ]; then
    add_block "$AUTOSTART" "-- Keep omission running so it opens instantly.
o.launch_on_start(\"$BIN daemon\")"
    say "omission now starts with Hyprland."
  fi

  reload_or_restore
  [ -n "$backed_up" ] && say "Backups:$(for f in $backed_up; do printf ' %s' "$f.bak.$STAMP"; done)"
  return 0
}

unconfigure_hyprland() {
  for file in "$BINDINGS" "$AUTOSTART"; do
    if [ -f "$file" ] && grep -q '^-- omission: start' "$file"; then
      backup "$file"
      remove_block "$file"
    fi
  done
  if [ -n "$backed_up" ]; then
    reload_or_restore
    say "Removed omission's keys, gestures and autostart from $HYPR."
  fi
}

# ── Uninstall ──────────────────────────────────────────────────────────────

if [ "$uninstall" = 1 ]; then
  bold "Uninstalling omission"
  stop_daemon
  unconfigure_hyprland
  for file in "$PREFIX/bin/omission" "$DESKTOP" "$ICON"; do
    if [ -e "$file" ]; then
      rm -f "$file"
      say "Removed $file"
    fi
  done
  say "Done."
  exit 0
fi

# ── Install ────────────────────────────────────────────────────────────────

[ "$(uname -s)" = Linux ] || die "omission runs on Linux with Hyprland."
bold "Installing omission"

# Qt 6 with Quick and its Wayland plugin, and Mesa for the GPU. Omarchy has
# all of them; on another Arch install, offer to add what's missing.
if command -v pacman >/dev/null 2>&1; then
  missing=""
  for package in qt6-base qt6-declarative qt6-wayland mesa wayland; do
    pacman -Q "$package" >/dev/null 2>&1 || missing="$missing $package"
  done
  if [ -n "$missing" ]; then
    say "omission needs:$missing"
    ask "Install them with pacman (asks for your password)?" || die "install$missing first, then run this again."
    # shellcheck disable=SC2086
    sudo pacman -S --needed --noconfirm $missing </dev/tty
  fi
fi

if [ "$download" = 1 ]; then
  case "$(uname -m)" in
    x86_64|amd64) arch=amd64 ;;
    aarch64|arm64) arch=arm64 ;;
    *) die "there's no build for $(uname -m) yet; build it from source with bin/install." ;;
  esac
  command -v curl >/dev/null 2>&1 || die "curl is needed to download omission."
  if [ "$version" = latest ]; then
    base="${OMISSION_DOWNLOAD_BASE:-https://github.com/$REPO/releases/latest/download}"
  else
    base="${OMISSION_DOWNLOAD_BASE:-https://github.com/$REPO/releases/download/$version}"
  fi

  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT INT TERM
  say "Downloading omission-linux-$arch from $base"
  curl -fsSL --retry 3 -o "$tmp/omission-linux-$arch" "$base/omission-linux-$arch" \
    || die "couldn't download omission-linux-$arch (is there a release yet?)"
  curl -fsSL --retry 3 -o "$tmp/SHA256SUMS" "$base/SHA256SUMS" || die "couldn't download SHA256SUMS"
  (cd "$tmp" && grep " omission-linux-$arch\$" SHA256SUMS | sha256sum -c --quiet -) \
    || die "the download doesn't match the release's checksum; not installing it."

  stop_daemon
  mkdir -p "$(dirname "$BIN")"
  install -m 755 "$tmp/omission-linux-$arch" "$BIN"
  say "Installed $BIN"

  # The launcher entry and icon, so omission shows up in the app launcher.
  mkdir -p "$(dirname "$DESKTOP")" "$(dirname "$ICON")"
  cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=Omission
Comment=See every workspace and window at once
Exec=$BIN toggle
Icon=omission
Terminal=false
Categories=Utility;
Keywords=mission control;expose;overview;workspaces;windows;
StartupWMClass=omission
EOF
  cat > "$ICON" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">
  <rect width="64" height="64" rx="14" fill="#FFD60A"/>
  <rect x="9" y="12" width="21" height="15" rx="2" fill="#141418"/>
  <rect x="34" y="12" width="21" height="15" rx="2" fill="#141418" opacity="0.55"/>
  <rect x="9" y="31" width="21" height="15" rx="2" fill="#141418" opacity="0.55"/>
  <rect x="34" y="31" width="21" height="15" rx="2" fill="#141418" opacity="0.55"/>
  <rect x="22" y="51" width="20" height="3" rx="1.5" fill="#141418"/>
</svg>
EOF
else
  [ -x "$BIN" ] || die "no omission binary at $BIN."
  stop_daemon
fi

# A library the binary can't find shows up now rather than as a silent
# failure on the first keypress.
if ldd "$BIN" 2>/dev/null | grep -q "not found"; then
  say "Warning: libraries omission needs are missing:"
  ldd "$BIN" | grep "not found" | sed 's/^/  /'
fi

if [ "$hotkeys" = yes ]; then
  ask "Bind the Mission Control key (F3 on Apple keyboards) and SUPER + D to omission?" || hotkeys=no
fi
if [ "$gestures" = yes ]; then
  ask "Add three-finger swipes: up opens omission, down closes it?" || gestures=no
fi
[ "$hotkeys" = no ] && [ "$gestures" = no ] && [ "$autostart" = no ] || configure_hyprland

start_daemon
say ""
bold "omission is installed."
[ "$hotkeys" = yes ] && say "Press the Mission Control key or SUPER + D to open it."
[ "$gestures" = yes ] && say "Swipe up with three fingers to open it, down to close it."
say "Uninstall: curl -fsSL https://raw.githubusercontent.com/$REPO/master/install.sh | sh -s -- --uninstall"
