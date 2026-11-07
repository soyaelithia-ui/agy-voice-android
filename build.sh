#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="/data/data/com.termux/files/home/.local/bin"
mkdir -p "$DEST"

echo "Compilando arecord (Android AAudio)..."
clang --target=aarch64-linux-android28 -O2 -Wall -Wextra \
    "$DIR/arecord.c" -L/system/lib64 -laaudio -o "$DEST/arecord"

echo "Compilando aplay (Android AAudio)..."
clang --target=aarch64-linux-android28 -O2 -Wall -Wextra \
    "$DIR/aplay.c" -L/system/lib64 -laaudio -o "$DEST/aplay"

chmod +x "$DEST/arecord" "$DEST/aplay"
echo "Instalados en $DEST/arecord y $DEST/aplay"
