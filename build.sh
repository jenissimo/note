#!/bin/sh
# build.sh -- note for macOS: one .app, built by clang, out of this tree
#
# No Xcode project, no CocoaPods, no package manager.  A .app is a folder with
# a plist in it, clang is already on any machine with the command line tools,
# and the whole build is the two lines below plus the copying.
#
#   ./build.sh            a release build in build/cocoa/note.app
#   ./build.sh debug      the same with -O0 -g and the sanitiser-friendly bits
#   ./build.sh run        build, then start it
#
# The packs ride in Contents/Resources: the Cocoa backend's exe_dir is that
# folder, so the 143 languages and 338 palettes arrive through exactly the
# path a user's own .syntax file would.  The icon rides there too, drawn by
# tools/make_icon.py the same way the Windows .ico is -- no binary to commit.

set -e

root=$(cd "$(dirname "$0")" && pwd)
mode=${1:-release}
out="$root/build/cocoa"
app="$out/note.app"

case "$mode" in
  debug) cflags="-O0 -g -DDEBUG" ;;
  *)     cflags="-Os -fno-common" ;;
esac

core="$root/src/core"
cocoa="$root/src/platform/cocoa"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$out/obj"

# The core is C89 and knows nothing about any of this; its own warnings are
# worth keeping loud, because a port is exactly when they start to matter.
for f in note_core note_conf note_syntax note_regex note_theme note_palette \
         note_pack note_buffer note_reduce; do
    clang -c -std=c89 -Wall -Wextra -Wno-unused-parameter $cflags \
          -o "$out/obj/$f.o" "$core/$f.c"
done

# The backend is Objective-C and may talk to the system all it likes.
for f in cocoa_main cocoa_host cocoa_edit cocoa_chrome cocoa_dialogs cocoa_palette; do
    clang -c -x objective-c -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -Wno-deprecated-declarations -fno-objc-arc $cflags \
          -o "$out/obj/$f.o" "$cocoa/$f.m"
done

clang -o "$app/Contents/MacOS/note" "$out/obj"/*.o \
      -framework Cocoa -framework AppKit -framework Foundation

cp "$root/assets/syntax.pack" "$app/Contents/Resources/syntax.pack"
cp "$root/assets/themes.pack" "$app/Contents/Resources/themes.pack"

# The icon is drawn by the same generator that draws the Windows one, in the
# macOS shape, and folded into an .icns by iconutil.  Drawing ten sizes up to
# 1024 takes the better part of a minute in plain Python, so the result is
# kept in build/ and redrawn only when the generator that made it changed.
icns="$out/note.icns"
if [ ! -f "$icns" ] || [ "$root/tools/make_icon.py" -nt "$icns" ]; then
    python3 "$root/tools/make_icon.py" --icns "$icns"
fi
cp "$icns" "$app/Contents/Resources/note.icns"

cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>note</string>
  <key>CFBundleDisplayName</key><string>note</string>
  <key>CFBundleIdentifier</key><string>com.note.editor</string>
  <key>CFBundleExecutable</key><string>note</string>
  <key>CFBundleIconFile</key><string>note.icns</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>1.0</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key><string>Text Document</string>
      <key>CFBundleTypeRole</key><string>Editor</string>
      <key>LSItemContentTypes</key>
      <array><string>public.text</string><string>public.plain-text</string></array>
    </dict>
  </array>
</dict>
</plist>
PLIST

# Ad hoc, so Gatekeeper treats it as a program the user built rather than one
# that arrived unsigned from somewhere.  A failure here is not fatal: the
# binary still runs on the machine that made it.
codesign --force --sign - "$app" 2>/dev/null || true

size=$(du -h "$app/Contents/MacOS/note" | cut -f1)
echo "built $app  ($size)"

[ "$mode" = run ] && open "$app"
exit 0
