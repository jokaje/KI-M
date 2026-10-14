#!/usr/bin/env python3
"""Feed every converted clip to real LVGL host renderer; preserve PNG proofs."""
import json
import subprocess
from io import BytesIO
from pathlib import Path
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
manifest=json.loads((ROOT/'firmware/main/generated/avatar.json').read_text())
data=(ROOT/'firmware/main/generated/avatar.bin').read_bytes()
assert len(data)==manifest['bytes']
scratch=Path('/home/coldnet/.hermes/cache/scratch/kim-avatar-proofs')
scratch.mkdir(parents=True,exist_ok=True)
proof=ROOT/'tests/menu_host/avatar-proofs'
proof.mkdir(exist_ok=True)
exe=Path('/home/coldnet/.hermes/cache/scratch/kim-menu-build/avatar_render_probe')
images=[]
for clip in manifest['clips']:
    name=clip['name']
    offset,length=clip['frames'][len(clip['frames'])//2]
    image=Image.open(BytesIO(data[offset:offset+length])).convert('RGB')
    raw=bytearray()
    for red,green,blue in image.getdata():
        v=(red>>3)<<11|(green>>2)<<5|(blue>>3)
        raw.extend((v&255,v>>8))
    src=scratch/f'{name}.565'; src.write_bytes(raw)
    ppm=scratch/f'{name}.ppm'
    subprocess.run([str(exe),str(src),str(ppm),f'KI-M | {name}'],check=True)
    rendered=Image.open(ppm).convert('RGB')
    assert rendered.getpixel((160,110)) != rendered.getpixel((0,110))
    rendered.save(proof/f'{name}.png',optimize=True)
    images.append(rendered)
print('LVGL rendered',len(images),'clips at 320x240:',', '.join(c['name'] for c in manifest['clips']))
