from pathlib import Path
# Arduino_GFX 1.6.4 RGB display: rotation 0 falls through switch with y2 uninitialized.
# Match exact function and declaration so upstream changes cannot silently bypass the fix.
paths=list(Path(".pio/libdeps").rglob("Arduino_RGB_Display.cpp"))
assert len(paths)==1, paths
p=paths[0]
s=p.read_text()
old="void Arduino_RGB_Display::writePixelPreclipped(int16_t x, int16_t y, uint16_t color)\n{\n  int32_t y2;"
new=old.replace("int32_t y2;","int32_t y2 = y;")
assert s.count(old)==1, "Unexpected Arduino_GFX source; inspect rotation 0 before build"
p.write_text(s.replace(old,new))
print("Verified RGB pixel rotation-0 fix:",p)
