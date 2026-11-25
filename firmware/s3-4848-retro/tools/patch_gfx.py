from pathlib import Path
# Apply the verified rotation-0 fix independently to each installed environment.
paths=sorted(Path(".pio/libdeps").rglob("Arduino_RGB_Display.cpp"))
assert paths, "Arduino_GFX not installed"
old="void Arduino_RGB_Display::writePixelPreclipped(int16_t x, int16_t y, uint16_t color)\n{\n  int32_t y2;"
new=old.replace("int32_t y2;","int32_t y2 = y;")
for p in paths:
    s=p.read_text()
    if s.count(old)==1 and s.count(new)==0:
        p.write_text(s.replace(old,new))
    else:
        assert s.count(old)==0 and s.count(new)==1, f"Unexpected Arduino_GFX source: {p}"
    print("Verified RGB pixel rotation-0 fix:",p)
