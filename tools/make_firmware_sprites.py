"""Convert approved KI-M 64px originals to packed RGB565+alpha; no resampling."""
import json
import struct
import sys
from pathlib import Path
from PIL import Image

STATES = ('neutral', 'idle', 'blink', 'listening', 'thinking', 'speaking', 'happy', 'offline', 'sleep')


def build(source: Path, target: Path):
    declared = json.loads((source / 'validation.json').read_text())
    blob = bytearray()
    manifest = {}
    for state in STATES:
        entries = declared[state]['frames']
        manifest[state] = []
        for entry in entries:
            path = source / state / entry['name']
            with Image.open(path) as image:
                if image.size != (64, 64) or image.mode != 'RGBA':
                    raise ValueError(f'invalid pixel art: {path}')
                manifest[state].append(len(blob))
                for r, g, b, a in image.get_flattened_data():
                    blob.extend(struct.pack('<HB', ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3), a))
    if sum(map(len, manifest.values())) != declared['total_frames']:
        raise ValueError('frame count mismatch')
    target.mkdir(parents=True, exist_ok=True)
    (target / 'sprites.bin').write_bytes(blob)
    (target / 'sprites.json').write_text(json.dumps(manifest, indent=2) + '\n')
    header = ['#pragma once', '#include <stdint.h>', '#define KIM_SPRITE_COUNT 46',
              'typedef enum { ' + ', '.join('KIM_' + name.upper() for name in STATES) + ' } kim_state_t;',
              'static const uint8_t kim_frame_counts[] = {' + ', '.join(str(len(manifest[name])) for name in STATES) + '};',
              'static const uint32_t kim_frame_offsets[] = {' + ', '.join(str(offset) for offsets in manifest.values() for offset in offsets) + '};']
    (target / 'sprites.h').write_text('\n'.join(header) + '\n')
    print(f'Packed {declared["total_frames"]} original frames into {len(blob)} bytes')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: make_firmware_sprites.py INPUT OUTPUT')
    build(Path(sys.argv[1]), Path(sys.argv[2]))
