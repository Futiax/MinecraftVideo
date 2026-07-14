#!/usr/bin/env bash
# Lance MinecraftVideo (version C) sous Linux/macOS.
# Compile automatiquement si nécessaire.
set -e
cd "$(dirname "$0")"

if ! command -v ffmpeg >/dev/null || ! command -v ffprobe >/dev/null; then
    echo "Erreur: ffmpeg/ffprobe introuvables dans le PATH."
    echo "Installez-les, par exemple : sudo apt install ffmpeg"
    exit 1
fi

BUILD_DIR="c version/build"
if [ ! -x "$BUILD_DIR/mcmm" ]; then
    echo "Compilation de mcmm..."
    cmake -S "c version" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_DIR" --config Release
fi

# mcmm résout ses chemins relativement au répertoire courant (../minecraft/...) :
# on se place dans "c version/build" pour que ../minecraft = "c version/minecraft"
cd "$BUILD_DIR"
exec ./mcmm "$@"
