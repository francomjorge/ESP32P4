#!/usr/bin/env python3
from pathlib import Path
from urllib.request import urlopen, Request
import hashlib,json
ROOT=Path(__file__).resolve().parents[1]
GAMES=[
("Tobu Tobu Girl DX","gb","SimonLarsen/tobutobugirl-dx","4985d4d282aa43229f250ffa3b0d86998ebd55d3","https://raw.githubusercontent.com/mxmgorin/oxgbc/b95ff230bcfbf9ec21186cbac4b954d377a7507d/crates/web/assets/tobudx.gb","LICENSE"),
("Thwaite","nes","pinobatch/thwaite-nes","v0.04","https://github.com/pinobatch/thwaite-nes/releases/download/v0.04/thwaite.nes","LICENSE.txt"),
("Big2Small","gb","mdsteele/big2small","v1.0.0","https://github.com/mdsteele/big2small/releases/download/v1.0.0/big2small.gb","LICENSE"),
("2048","gb","wyattferguson/2048-gb","167788105db659f2049dd2d537ee12f63d87b1e7","2048.gb","LICENSE"),
("Breakout","gb","flozz/gameboy-examples","e3796043a4f392a934a00f26f4ed20ce06ad5323","10-breakout/breakout.gb","LICENSE"),
("Gamepad Test","gb","flozz/gameboy-examples","e3796043a4f392a934a00f26f4ed20ce06ad5323","02-gamepad/gamepad.gb","LICENSE"),
("Platformer","nes","nesdoug/17_Platformer","c10167f3072e10b27e89ecc869c3475744ddfa51","BUILD/platformer.nes","License.txt"),
("NES Hello","nes","nesdoug/01_Hello","940b0aab0f306176f8d1c21854690c9a206d183d","BUILD/hello.nes","License.txt"),
]
GAMES=[g for g in GAMES if g[0]=="Tobu Tobu Girl DX"]
assert len(GAMES)==1

def get(url):
    with urlopen(Request(url,headers={"User-Agent":"PrismaRetroCI"}),timeout=60) as r:return r.read()
out=ROOT/"dist"/"sd-card"
lic=out/"licenses";lic.mkdir(parents=True,exist_ok=True)
header=['#pragma once','#include <stdint.h>','#include <stddef.h>','struct BuiltinROM {const char *name;const uint8_t *data;size_t size;bool nes;};']
entries=[];manifest=[]
for i,(name,system,repo,ref,path,license_name) in enumerate(GAMES):
    base=f"https://raw.githubusercontent.com/{repo}/{ref}/"
    url=path if path.startswith("https:") else base+path
    data=get(url)
    if system=="gb":
        assert len(data)>=32768 and len(data)<=4194304 and data[0x143]!=0xC0,name
        c=0
        for b in data[0x134:0x14D]:c=(c-b-1)&255
        assert c==data[0x14D],name
    else:
        assert data[:4]==b"NES\x1a" and len(data)>=16+data[4]*16384+data[5]*8192+(512 if data[6]&4 else 0),name
    dest=out/"roms"/system;dest.mkdir(parents=True,exist_ok=True)
    (dest/(name.replace(" ","_")+"."+system)).write_bytes(data)
    (lic/(repo.replace("/","_")+".txt")).write_bytes(get(base+license_name))
    if name=="Tobu Tobu Girl DX":
        (lic/"Tobu-credits.txt").write_bytes(get(base+"README.md"))
        (lic/"CC-BY-4.0.txt").write_bytes(get("https://creativecommons.org/licenses/by/4.0/legalcode.txt"))
    header.append(f"static const uint8_t builtin_{i}[]={{")
    for pos in range(0,len(data),32):header.append(",".join(f"0x{b:02x}" for b in data[pos:pos+32])+",")
    header.append("};")
    entries.append(f'{{"{name}",builtin_{i},sizeof(builtin_{i}),{str(system=="nes").lower()}}}')
    manifest.append(dict(name=name,system=system,rom=url,source=f"https://github.com/{repo}/tree/{ref}",license=base+license_name,sha256=hashlib.sha256(data).hexdigest()))
header.append("static const BuiltinROM builtins[]={"+",".join(entries)+"};")
(ROOT/"src"/"builtin_roms.h").write_text("\n".join(header)+"\n")
(out/"manifest.json").write_text(json.dumps(manifest,indent=2))
print("Prepared",len(entries),"ROMs")
