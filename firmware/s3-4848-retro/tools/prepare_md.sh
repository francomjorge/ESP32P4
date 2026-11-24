#!/usr/bin/env bash
set -euo pipefail
ref=4ced120669750ca7228fd0414211430c1d923166
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git init -q "$tmp"
git -C "$tmp" remote add origin https://github.com/ducalex/retro-go.git
git -C "$tmp" fetch -q --depth=1 origin "$ref"
git -C "$tmp" checkout -q FETCH_HEAD -- gwenesis/components/gwenesis
mkdir -p lib/gwenesis/src dist/sources
cp -R "$tmp/gwenesis/components/gwenesis/src/"* lib/gwenesis/src/
cp "$tmp/gwenesis/components/gwenesis/"{LICENSE,README.md,gwenesis.h} lib/gwenesis/
# PlatformIO LDF needs the public header under src and an explicit manifest.
sed 's@"src/@"@g' lib/gwenesis/gwenesis.h > lib/gwenesis/src/gwenesis.h
cat > lib/gwenesis/library.json <<'JSON'
{"name":"gwenesis","version":"0.0.1","build":{"srcDir":"src","flags":["-O2"],"srcFilter":["+<*>","-<cpus/Z80/Debug.c>","-<cpus/Z80/ConDebug.c>"]},"frameworks":"*","platforms":"*"}
JSON
find lib/gwenesis -name '*.o' -delete
# Use caller-owned padded ROM, not a redundant second copy.
python - <<'PY'
from pathlib import Path
p=Path("lib/gwenesis/src/bus/gwenesis_bus.c")
s=p.read_text().replace("#ifdef RETRO_GO","#if 1 /* PRISMA: caller owns padded ROM */")
p.write_text(s)
p=Path("lib/gwenesis/src/cpus/M68K/m68k.h")
s=p.read_text().replace("ROM_DATA[((A) ^ 1)]","ROM_DATA[(((A)&0x3fffff) ^ 1)]").replace("ROM_DATA[(A)]","ROM_DATA[((A)&0x3fffff)]")
p.write_text(s)
# GCC 14 treats the upstream indexed-pixel pointer mismatch as an error.
# The interface accepts uint16_t*, but its actual pixel format is uint8_t indices.
p=Path("lib/gwenesis/src/vdp/gwenesis_vdp_gfx.c")
s=p.read_text()
assert "screen_buffer_line = ptr_screen_buffer;" in s and "screen_buffer = ptr_screen_buffer;" in s
s=s.replace("screen_buffer_line = ptr_screen_buffer;","screen_buffer_line = (uint8_t *)ptr_screen_buffer;").replace("screen_buffer = ptr_screen_buffer;","screen_buffer = (uint8_t *)ptr_screen_buffer;")
p.write_text(s)
# Force performance optimization only for Gwenesis translation units (not NES/GB).
for p in Path("lib/gwenesis/src").rglob("*.c"):
    s=p.read_text()
    # A/B: optimize interpreter and scanline renderer; preserve instruction cycles and bus timing.
    opt="O3" if p.name in ("gwenesis_vdp_gfx.c","m68kcpu.c","Z80.c") else "O2"
    p.write_text('#pragma GCC optimize ("'+opt+'")\n'+s)
PY
tar -czf dist/sources/gwenesis-source.tar.gz -C lib gwenesis
