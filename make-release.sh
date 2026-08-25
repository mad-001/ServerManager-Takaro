#!/usr/bin/env bash
# make-release.sh — build ONE canonical ServerManager-Takaro release archive.
#
#   ./make-release.sh [VERSION]        # default VERSION below
#
# The archive is a true single drop-in for any Unreal Engine dedicated server:
#   winmm.dll + version.dll   (our in-DLL Takaro core; also UE4SS's loader)
#   ue4ss/                    (bundled RE-UE4SS runtime — no separate download)
#   ue4ss/Mods/TakaroConnector/  (our universal Lua mod)
#   profiles/                 (one profile per supported game; copy to profile.lua)
#
# UE4SS runtime source (big binary, kept out of git) is taken from, in order:
#   $UE4SS_SRC, then ./vendor/ue4ss, then the local Palworld test install.
set -euo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO"
# Version comes from the tracked VERSION file (bump it on every change); an explicit
# arg overrides. Keep VERSION in sync with what you push.
VERSION="${1:-$(cat "$REPO/VERSION" 2>/dev/null || echo 0.0.0)}"

# ── locate the UE4SS runtime to bundle ────────────────────────────────────────
TEST_INSTALL="/mnt/c/Program Files (x86)/Steam/steamapps/common/TakaroTestServers/Palworld/Pal/Binaries/Win64/ue4ss"
UE4SS_SRC="${UE4SS_SRC:-}"
if [[ -z "$UE4SS_SRC" ]]; then
  if   [[ -f "$REPO/vendor/ue4ss/UE4SS.dll" ]]; then UE4SS_SRC="$REPO/vendor/ue4ss"
  elif [[ -f "$TEST_INSTALL/UE4SS.dll"      ]]; then UE4SS_SRC="$TEST_INSTALL"
  else echo "ERROR: no UE4SS runtime found. Set UE4SS_SRC=/path/to/ue4ss" >&2; exit 1
  fi
fi
echo "UE4SS runtime : $UE4SS_SRC"

# ── sanity: our build artifacts must exist ────────────────────────────────────
for f in core/winmm.dll core/version.dll \
         mod/TakaroConnector/Scripts/main.lua \
         mod/TakaroConnector/Scripts/json.lua \
         mod/TakaroConnector/Scripts/autodetect.lua \
         mod/TakaroConnector/Scripts/profile.template.lua \
         mod/TakaroConnector/TakaroConfig.txt; do
  [[ -f "$REPO/$f" ]] || { echo "ERROR: missing $f (build the DLLs first: cd core && ./build.sh)" >&2; exit 1; }
done

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
# The staging root IS the Win64 overlay: no wrapper folder, so unzipping straight
# into ...\Binaries\Win64\ drops winmm.dll / version.dll / ue4ss\ exactly in place.
ROOT="$STAGE/root"
mkdir -p "$ROOT/ue4ss/Mods/TakaroConnector/Scripts" "$ROOT/ue4ss/Mods/TakaroConnector/profiles"

# 1) our proxy DLLs (Takaro core + UE4SS loader)
cp core/winmm.dll core/version.dll "$ROOT/"

# 2) bundled UE4SS runtime
cp "$UE4SS_SRC/UE4SS.dll" "$UE4SS_SRC/UE4SS-settings.ini" "$UE4SS_SRC/MemberVariableLayout.ini" "$ROOT/ue4ss/"
for d in BPML_GenericFunctions BPModLoaderMod Keybinds shared; do
  [[ -d "$UE4SS_SRC/Mods/$d" ]] && cp -r "$UE4SS_SRC/Mods/$d" "$ROOT/ue4ss/Mods/"
done
[[ -f "$UE4SS_SRC/Mods/mods.json" ]] && cp "$UE4SS_SRC/Mods/mods.json" "$ROOT/ue4ss/Mods/"

# 3) our universal Lua mod
cp mod/TakaroConnector/Scripts/main.lua \
   mod/TakaroConnector/Scripts/json.lua \
   mod/TakaroConnector/Scripts/autodetect.lua \
   mod/TakaroConnector/Scripts/profile.template.lua \
   "$ROOT/ue4ss/Mods/TakaroConnector/Scripts/"
cp mod/TakaroConnector/TakaroConfig.txt "$ROOT/ue4ss/Mods/TakaroConnector/TakaroConfig.txt"

# default active profile = universal auto-detect (works on most games unchanged)
printf -- '-- Default universal profile: chat/death auto-discover at runtime.\n-- For a tuned profile, copy profiles/<game>.lua over this file.\nreturn { name = "Auto-detect (universal)" }\n' \
  > "$ROOT/ue4ss/Mods/TakaroConnector/Scripts/profile.lua"

# 4) mods.txt — UE4SS built-ins + our mod enabled
cat > "$ROOT/ue4ss/Mods/mods.txt" <<'EOF'
BPML_GenericFunctions : 1
BPModLoaderMod : 1
Keybinds : 0
TakaroConnector : 1
EOF

# 5) per-game profiles (reference; tucked in the mod folder, not the Win64 root)
cp profiles/*.lua "$ROOT/ue4ss/Mods/TakaroConnector/profiles/"

# 6) install note (inside the mod folder, so the Win64 root stays clean)
cat > "$ROOT/ue4ss/Mods/TakaroConnector/INSTALL.txt" <<EOF
ServerManager-Takaro v$VERSION — ALL-IN-ONE (UE4SS bundled)

One universal in-game mod connecting any Unreal Engine dedicated server to Takaro.
The UE4SS runtime is INCLUDED — you do NOT download it separately.

This zip's root IS the Win64 overlay (winmm.dll, version.dll, ue4ss\\ at the top).

INSTALL (any UE dedicated server):
  1. Find the server's Win64 folder (contains the dedicated-server .exe):
        <game-server>\\<ProjectName>\\Binaries\\Win64\\
  2. Unzip this archive straight INTO that Win64 folder (Extract Here / merge if asked).
     winmm.dll, version.dll and ue4ss\\ now sit directly in Win64.
  3. (optional, per game) copy
        ue4ss\\Mods\\TakaroConnector\\profiles\\<game>.lua
     over ue4ss\\Mods\\TakaroConnector\\Scripts\\profile.lua
     Leaving the default is fine — chat/death auto-discover at runtime.
  4. Paste your token in ue4ss\\Mods\\TakaroConnector\\TakaroConfig.txt:
        REGISTRATION_TOKEN=<Takaro -> create a "Generic" game server -> copy token>
     and set SERVER_NAME to whatever you like.
  5. Start the server. It connects to Takaro itself — no bridge, no Node.

winmm.dll is UE4SS's loader here (it LoadLibrary's ue4ss\\UE4SS.dll), so you do NOT
add UE4SS's own dwmapi/xinput proxy — ours replaces it. version.dll is a fallback
for the few games that don't import winmm; shipping both is safe (single-instance guard).

UE4SS (RE-UE4SS) is bundled under its own MIT license: https://github.com/UE4SS-RE/RE-UE4SS
EOF

# ── zip it ────────────────────────────────────────────────────────────────────
OUT="$REPO/ServerManager-Takaro-v$VERSION.zip"
rm -f "$OUT"
python3 - "$ROOT" "$OUT" <<'PY'
import sys, os, zipfile
base, out = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED)
for root, _, files in os.walk(base):
    for f in files:
        p = os.path.join(root, f)
        z.write(p, os.path.relpath(p, base))   # root of the zip = Win64 overlay
z.close()
print("wrote", out, round(os.path.getsize(out)/1e6, 1), "MB")
PY
