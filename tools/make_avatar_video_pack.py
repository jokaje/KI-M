#!/usr/bin/env python3
"""Convert seven authored MP4s to fixed-width, bounded indexed JPEG frame pack."""
import hashlib
import json
import subprocess
from io import BytesIO
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'assets/ki-m-video-v1/originals'
OUT = ROOT / 'firmware/main/generated/avatar.bin'
ORDER = ('idle', 'listening', 'thinking', 'speaking', 'laughing', 'crying', 'sleep_wake')
FPS = 12
SIZE = 192

def convert():
    data = bytearray()
    clips = []
    for name in ORDER:
        path = SRC / f'{name}.mp4'
        probe = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_entries',
            'format=duration:stream=width,height', '-of', 'json', str(path)]))
        duration = float(probe['format']['duration'])
        width, height = ((s['width'], s['height']) for s in probe['streams'] if 'width' in s).__next__()
        # Square centre crop retains the full image height. Keep all expressions
        # in identical screen coordinates without stretching or clipping vertically.
        side = min(width, height)
        x, y = (width-side)//2, (height-side)//2
        raw = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(path), '-vf',
            f'fps={FPS},crop={side}:{side}:{x}:{y},scale={SIZE}:{SIZE}:flags=lanczos',
            '-an', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'])
        stride = SIZE*SIZE*3
        assert len(raw) % stride == 0 and raw
        frames = []
        for i in range(len(raw)//stride):
            im = Image.frombytes('RGB', (SIZE,SIZE), raw[i*stride:(i+1)*stride])
            out = BytesIO()
            im.save(out, 'JPEG', quality=60, subsampling=2, optimize=True, progressive=False)
            jpeg = out.getvalue()
            assert jpeg[:2] == b'\xff\xd8' and len(jpeg) <= 65535
            frames.append((len(data), len(jpeg)))
            data.extend(jpeg)
        clips.append({'name':name, 'frames':frames, 'duration_ms':round(duration*1000),
                      'crop':[x,y,side,side], 'source_sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(data)
    manifest = {'fps':FPS, 'size':SIZE, 'bytes':len(data), 'sha256':hashlib.sha256(data).hexdigest(), 'clips':clips}
    (OUT.parent/'avatar.json').write_text(json.dumps(manifest, indent=2)+'\n')
    h = ['#pragma once','#include <stdint.h>', '#define KIM_AVATAR_SIZE 192',
         f'#define KIM_AVATAR_FPS {FPS}', f'#define KIM_AVATAR_BYTES {len(data)}',
         f'#define KIM_AVATAR_CLIPS {len(clips)}',
         'typedef struct { uint32_t offset; uint16_t length; } kim_avatar_frame_t;',
         'typedef struct { uint16_t start, count; uint32_t duration_ms; } kim_avatar_clip_t;',
         'static const kim_avatar_frame_t kim_avatar_frames[] = {']
    start=0
    for clip in clips:
        h.extend(f'    {{{offset}, {length}}},' for offset,length in clip['frames'])
    h += ['};','static const kim_avatar_clip_t kim_avatar_clips[] = {']
    for clip in clips:
        h.append(f'    {{{start}, {len(clip["frames"])}, {clip["duration_ms"]}}}, /* {clip["name"]} */')
        start += len(clip['frames'])
    h += ['};',f'#define KIM_AVATAR_FRAME_COUNT {start}']
    (OUT.parent/'avatar.h').write_text('\n'.join(h)+'\n')
    return manifest

if __name__ == '__main__':
    m=convert()
    print(m['bytes'], [(c['name'],len(c['frames'])) for c in m['clips']])
