# Build-time-only conversion of public reference previews. No networking on device.
from pathlib import Path
from urllib.request import urlopen, Request
from urllib.parse import quote
from io import BytesIO
from PIL import Image, ImageOps
import time
W,H=240,180
nes="https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Nintendo_Entertainment_System/29607117d7dd3a0d42273dc5599ad5982cf5eb91/Named_Snaps/"
items=[
 ("64-in-1",nes+"64-in-1 (Asia) (En) (Pirate).png"),
 ("1200-in-1",nes+"1200 in 1 (199x)(-)(AS)[p].png"),
 ("Super Mario Bros.",nes+"Super Mario Bros. (World).png"),
 ("Super Mario Bros. 3",nes+"Super Mario Bros. 3 (USA).png"),
 ("Tobu Tobu Girl DX","https://raw.githubusercontent.com/SimonLarsen/tobutobugirl-dx/4985d4d282aa43229f250ffa3b0d86998ebd55d3/data/bg/titlescreen.png"),
]
out=["#pragma once","#include <stdint.h>","namespace PreviewArt {","constexpr int W=240,H=180;"]
rows=[]
for i,(name,url) in enumerate(items):
    data=None
    for attempt in range(3):
        try:
            with urlopen(Request(quote(url,safe=":/"),headers={"User-Agent":"PrismaRetro-build"}),timeout=15) as f:
                im=Image.open(BytesIO(f.read())).convert("RGB")
            data=ImageOps.contain(im,(W,H),Image.Resampling.NEAREST)
            break
        except Exception as e:
            print("preview unavailable",name,e)
            if attempt<2: time.sleep(1)
    if data is None:
        rows.append('{"'+name+'",nullptr}')
        continue
    bg=Image.new("RGB",(W,H),(12,16,28))
    bg.paste(data,((W-data.width)//2,(H-data.height)//2))
    values=[(rr>>3)<<11|(gg>>2)<<5|(bb>>3) for rr,gg,bb in bg.getdata()]
    out.append("static const uint16_t art%d[]={"%i)
    for n in range(0,len(values),24):
        out.append(",".join("0x%04x"%v for v in values[n:n+24])+",")
    out.append("};")
    rows.append('{"'+name+'",art%d}'%i)
    print("preview ready",name,url)
out.extend(["struct Entry {const char* name;const uint16_t* pixels;};","static const Entry entries[]={"+",".join(rows)+"};","}"])
Path("src/preview_art.h").write_text("\n".join(out)+"\n")
