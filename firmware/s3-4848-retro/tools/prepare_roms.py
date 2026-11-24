#!/usr/bin/env python3
"""Fetch pinned, redistributable DMG homebrew ROMs and generate flash assets."""
from pathlib import Path
from urllib.request import urlopen
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
GAMES = [
    ("2048", "wyattferguson/2048-gb", "167788105db659f2049dd2d537ee12f63d87b1e7", "2048.gb"),
    ("Breakout", "flozz/gameboy-examples", "e3796043a4f392a934a00f26f4ed20ce06ad5323", "10-breakout/breakout.gb"),
    ("TicTacToe", "flozz/gameboy-examples", "e3796043a4f392a934a00f26f4ed20ce06ad5323", "03-tic-tac-toe/tictactoe.gb"),
    ("Gamepad Test", "flozz/gameboy-examples", "e3796043a4f392a934a00f26f4ed20ce06ad5323", "02-gamepad/gamepad.gb"),
]
def fetch(repo, sha, path):
    url = f"https://raw.githubusercontent.com/{repo}/{sha}/{path}"
    with urlopen(url, timeout=30) as response:
        return response.read(), url

out = ROOT / "dist" / "sd-card"
roms = out / "roms" / "gb"
licenses = out / "licenses"
roms.mkdir(parents=True, exist_ok=True)
licenses.mkdir(parents=True, exist_ok=True)
header = ['#pragma once', '#include <stdint.h>', '#include <stddef.h>',
          'struct BuiltinROM { const char* name; const uint8_t* data; size_t size; };']
entries, manifest = [], []
for i, (name, repo, sha, path) in enumerate(GAMES):
    data, url = fetch(repo, sha, path)
    assert len(data) == 32768, (name, len(data))
    assert data[0x143] != 0xC0, name
    check = 0
    for byte in data[0x134:0x14D]:
        check = (check - byte - 1) & 255
    assert check == data[0x14D], name
    (roms / (name.replace(" ", "_") + ".gb")).write_bytes(data)
    license_data, license_url = fetch(repo, sha, "LICENSE")
    (licenses / (repo.replace("/", "_") + ".txt")).write_bytes(license_data)
    header.append(f"static const uint8_t builtin_{i}[] = {{")
    for pos in range(0, len(data), 32):
        header.append(",".join(f"0x{b:02x}" for b in data[pos:pos+32]) + ",")
    header.append("};")
    entries.append(f'{{"{name}",builtin_{i},sizeof(builtin_{i})}}')
    manifest.append({"name": name, "rom": url, "license": license_url,
                     "sha256": hashlib.sha256(data).hexdigest()})
header.append("static const BuiltinROM builtins[] = {" + ",".join(entries) + "};")
(ROOT / "src" / "builtin_roms.h").write_text("\n".join(header) + "\n")
(out / "manifest.json").write_text(json.dumps(manifest, indent=2))
print("Prepared", len(entries), "DMG ROMs")
