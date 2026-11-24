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

# PRISMA 028: two driver-owned scanout buffers and public panel handle.
p=next(Path(".pio/libdeps").rglob("Arduino_ESP32RGBPanel.cpp"))
s=p.read_text()
assert s.count(".num_fbs = 1,")==1
p.write_text(s.replace(".num_fbs = 1,", ".num_fbs = 2,"))
p=next(Path(".pio/libdeps").rglob("Arduino_ESP32RGBPanel.h"))
s=p.read_text()
old="  uint16_t *getFrameBuffer(int16_t w, int16_t h);"
assert s.count(old)==1
p.write_text(s.replace(old,old+"\n  esp_lcd_panel_handle_t prismaPanelHandle() const { return _panel_handle; }"))
print("PRISMA RGB double framebuffer patch applied")
