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
assert s.count("unsigned char *ROM_DATA; // 68K Main Program (uncompressed)")==1
s=s.replace("unsigned char *ROM_DATA; // 68K Main Program (uncompressed)","unsigned char *ROM_DATA; // 68K Main Program (uncompressed)\nunsigned int prisma_rom_size;")
assert s.count("ROM_DATA = buffer;")==1
s=s.replace("ROM_DATA = buffer;","ROM_DATA = buffer;\n    prisma_rom_size = (unsigned int)size;")
p.write_text(s)
p=Path("lib/gwenesis/src/cpus/M68K/m68k.h")
s=p.read_text()
start=s.index("#define FETCH8ROM(A)")
end=s.index("#if GNW_TARGET_MARIO",start)
old=s[start:end]
assert old.count("#define FETCH")==3 and "ROM_DATA[((A) ^ 1)]" in old
# Preserve previous 4 MiB address masking and 0xff padding, now with only four physical guard bytes.
# 16/32-bit reads may straddle the real end; guard bytes reproduce previous padded contents.
s=s[:start]+"""extern unsigned int prisma_rom_size;
static inline unsigned char prisma_fetch8(unsigned int a) {
    a=(a&0x3fffffu)^1u;
    return a<prisma_rom_size ? ROM_DATA[a] : 0xffu;
}
static inline unsigned short prisma_fetch16(unsigned int a) {
    a&=0x3fffffu;
    if(a>=prisma_rom_size)return 0xffffu;
    unsigned short v; __builtin_memcpy(&v,ROM_DATA+a,2); return v;
}
static inline unsigned int prisma_fetch32(unsigned int a) {
    a&=0x3fffffu;
    if(a>=prisma_rom_size)return 0xffffffffu;
    unsigned int v; __builtin_memcpy(&v,ROM_DATA+a,4);
    return (v<<16)|(v>>16);
}
#define FETCH8ROM(A) prisma_fetch8((unsigned int)(A))
#define FETCH16ROM(A) prisma_fetch16((unsigned int)(A))
#define FETCH32ROM(A) prisma_fetch32((unsigned int)(A))

"""+s[end:]
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
