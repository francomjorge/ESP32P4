#!/usr/bin/env bash
set -euo pipefail
ref=4ced120669750ca7228fd0414211430c1d923166
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git init -q "$tmp"
git -C "$tmp" remote add origin https://github.com/ducalex/retro-go.git
git -C "$tmp" fetch -q --depth=1 origin "$ref"
git -C "$tmp" checkout -q FETCH_HEAD -- retro-core/components/nofrendo
mkdir -p lib/nofrendo/src dist/sources
cp -R "$tmp/retro-core/components/nofrendo/nes" "$tmp/retro-core/components/nofrendo/mappers" lib/nofrendo/src/
cp "$tmp/retro-core/components/nofrendo/"{config.h,database.h,nofrendo.h,nofrendo.c,palettes.h} lib/nofrendo/src/
cp "$tmp/retro-core/components/nofrendo/COPYING" lib/nofrendo/
python tools/patch_multicart.py
tar -czf dist/sources/nofrendo-source.tar.gz -C lib nofrendo
