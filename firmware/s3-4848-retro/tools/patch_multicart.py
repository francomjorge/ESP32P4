from pathlib import Path
import shutil
root=Path("lib/nofrendo/src")
def replace(path,old,new):
    p=root/path;s=p.read_text()
    assert s.count(old)==1,(path,old)
    p.write_text(s.replace(old,new))
shutil.copyfile("tools/map_multicart.c",root/"mappers/map_multicart.c")
replace("mappers/mappers.h","extern mapintf_t map229_intf;","extern mapintf_t map225_intf;\nextern mapintf_t map227_intf;\nextern mapintf_t map229_intf;")
replace("mappers/mappers.h","    &map229_intf,","    &map225_intf,\n    &map227_intf,\n    &map229_intf,")
replace("nes/ppu.h","void ppu_reset(void);","void ppu_reset(void);\nvoid ppu_setchrwrite(bool enabled);")
replace("nes/ppu.c","static ppu_t ppu;","static ppu_t ppu;\nstatic bool chrwrite=true;\nvoid ppu_setchrwrite(bool enabled){chrwrite=enabled;}")
replace("nes/ppu.c","      if (ppu.vaddr < 0x3F00)","      if (ppu.vaddr < 0x2000 && (!ppu.vram_present || !chrwrite)) { /* ignore CHR ROM/protected RAM write */ }\n      else if (ppu.vaddr < 0x3F00)")
replace("nes/ppu.c","void ppu_reset(void)\n{","void ppu_reset(void)\n{\n   chrwrite=true;")
