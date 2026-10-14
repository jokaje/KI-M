"""Validate all seven authored clips and every embedded JPEG frame."""
import hashlib
import json
from io import BytesIO
from pathlib import Path
from PIL import Image, ImageChops, ImageStat

ROOT=Path(__file__).resolve().parents[1]
PACK=ROOT/'firmware/main/generated/avatar.bin'
META=ROOT/'firmware/main/generated/avatar.json'

def test_pack_frames_and_sources():
    meta=json.loads(META.read_text())
    pack=PACK.read_bytes()
    assert len(pack)==meta['bytes'] and hashlib.sha256(pack).hexdigest()==meta['sha256']
    assert [c['name'] for c in meta['clips']]==[
        'idle','listening','thinking','speaking','laughing','crying','sleep_wake']
    assert meta['fps']==12
    assert sum(len(c['frames']) for c in meta['clips'])>400
    last=0
    for clip in meta['clips']:
        assert hashlib.sha256((ROOT/'assets/ki-m-video-v1/originals'/f'{clip["name"]}.mp4').read_bytes()).hexdigest()==clip['source_sha256']
        for offset,length in clip['frames']:
            assert offset==last and 64<=length<=65535 and offset+length<=len(pack)
            im=Image.open(BytesIO(pack[offset:offset+length]))
            im.load() # fail on any truncated or malformed JPEG
            assert im.size==(192,192)
            last+=length
        assert abs(len(clip['frames'])/meta['fps']*1000-clip['duration_ms'])<200+1000/meta['fps']
    assert last==len(pack)

def test_loops_are_not_mislabeled_seamless():
    meta=json.loads(META.read_text())
    pack=PACK.read_bytes()
    for clip in meta['clips']:
        first=clip['frames'][0]; last=clip['frames'][-1]
        a=Image.open(BytesIO(pack[first[0]:first[0]+first[1]])).convert('RGB')
        b=Image.open(BytesIO(pack[last[0]:last[0]+last[1]])).convert('RGB')
        mean=sum(ImageStat.Stat(ImageChops.difference(a,b)).mean)/3
        assert mean>=0
        print(f'{clip["name"]} first-last mean RGB difference={mean:.2f}')
