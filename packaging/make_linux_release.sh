#!/usr/bin/env bash
# Build a redistributable Linux folder out of an existing linux-release build.
#
# What ships: the engine, its runtime library, an empty dlc/ folder to drop map
# packs into, and a launcher. What does NOT ship, and cannot: the game itself.
# The player supplies their own Xbox 360 disc image; the engine's setup screen
# installs it into game/ on first run.
#
#   packaging/make_linux_release.sh [output-dir]
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build=$here/out/build/linux-release
out=${1:-$here/out/release}
version=$(git -C "$here" describe --always --dirty 2>/dev/null || echo unknown)
stage=$out/Skate3Recomp-Linux

for required in "$build/skate3" "$build/librexruntime.so"; do
  [ -f "$required" ] || { echo "missing $required - build the linux-release preset first" >&2; exit 1; }
done

rm -rf "$stage"
mkdir -p "$stage/dlc"
install -m 755 "$build/skate3" "$stage/skate3"
install -m 755 "$build/librexruntime.so" "$stage/librexruntime.so"

# NO GAME DATA IS PACKAGED, and that includes the title update.
#
# An earlier version copied default.xexp and EAWebkit.xexp in so a local build
# would run out of the box. They are EA's Title Update 3 payloads - raw game
# data, not recompiled code - so shipping them in a public release would be
# redistributing copyrighted material. The engine has its own title-update
# installer for exactly this; players supply their own copy, the same way they
# supply the disc image.
#
# The guard below is what enforces it, so this cannot regress quietly.

cat > "$stage/dlc/README.txt" <<'TXT'
Drop map packs in this folder.

Both shapes community maps arrive in work as they are:

  dlc/MYMAP/mymap_00000000.big        (with its .header beside it)
  dlc/SomeOfficialDLCContainer        (a LIVE/CON package, no extension)

Put each pack in its own folder, or drop the files loose - the game scans this
whole tree. With one pack installed it loads straight away. With more than one
it asks which to play every time the game starts, and only that one is
installed, which is what keeps a large collection from breaking the game's
content scan.

Nothing here is modified or moved out of this folder.
TXT

cat > "$stage/play-skate3.sh" <<'TXT'
#!/usr/bin/env bash
# Launch Skate 3. Everything lives beside this script; nothing is installed
# system-wide.
cd "$(dirname "$(readlink -f "$0")")" || exit 1

# The engine loads librexruntime.so from its own folder.
export LD_LIBRARY_PATH="$PWD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# GTK is used for the setup screens; force X11 so they behave the same under
# both X11 and Wayland sessions.
export GDK_BACKEND=${GDK_BACKEND:-x11}

exec ./skate3 "$@"
TXT
chmod 755 "$stage/play-skate3.sh"

cat > "$stage/skate3.desktop" <<TXT
[Desktop Entry]
Type=Application
Name=Skate 3
Comment=Skate 3 recompilation
Exec=$stage/play-skate3.sh
Terminal=false
Categories=Game;
TXT

cp "$here/packaging/RELEASE_README.md" "$stage/README.md"
printf '%s\n' "$version" > "$stage/VERSION"

# Refuse to build an archive containing anything that is not ours to ship:
# game data (the disc, the title update) or map packs someone left in dlc/.
leaked=$(find "$stage" \( -name '*.xex' -o -name '*.xexp' -o -name '*.big' \
                        -o -name '*.header' -o -name '*.iso' \) -print)
if [ -n "$leaked" ]; then
  echo "REFUSING to package - these are not ours to redistribute:" >&2
  echo "$leaked" >&2
  exit 1
fi

tarball=$out/Skate3Recomp-Linux-$version.tar.gz
tar -C "$out" -czf "$tarball" "$(basename "$stage")"
echo "staged   $stage"
echo "tarball  $tarball  ($(du -h "$tarball" | cut -f1))"
