"""Prepare original KI-M PNG frames; not yet a firmware-integrated player."""
import argparse
import hashlib
import json
from pathlib import Path
from PIL import Image, ImageOps


def rgb565(image):
    rgba = image.convert('RGBA')
    background = Image.new('RGBA', rgba.size, (0, 0, 0, 255))
    rgb = Image.alpha_composite(background, rgba).convert('RGB')
    result = bytearray()
    for r, g, b in rgb.get_flattened_data():
        value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        result.extend(value.to_bytes(2, 'little'))
    return bytes(result)


def prepare(source, output, size=160, fps=12):
    source, output = Path(source), Path(output)
    if not 1 <= fps <= 25 or not 16 <= size <= 240:
        raise ValueError('fps must be 1..25; size must be 16..240')
    if output.exists():
        raise FileExistsError('Output must be new; no assets will be overwritten')
    frames = sorted(source.glob('*.png'))
    if not frames:
        raise ValueError('No PNG frames found')
    if len(frames) > 120:
        raise ValueError('Maximum 120 frames per preparation batch')
    # Decode before creating the output so corrupt input does not leave a partial pack.
    images = []
    for path in frames:
        with Image.open(path) as image:
            images.append(ImageOps.pad(image.convert('RGBA'), (size, size),
                          method=Image.Resampling.LANCZOS, color=(0, 0, 0, 0)))
    output.mkdir(parents=True)
    records, previews = [], []
    for index, (path, image) in enumerate(zip(frames, images)):
        name = f'frame_{index:03d}'
        data = rgb565(image)
        (output / f'{name}.rgb565').write_bytes(data)
        image.save(output / f'{name}.png')
        previews.append(Image.alpha_composite(Image.new('RGBA', image.size, 'black'), image).convert('RGB'))
        records.append({'source': path.name, 'file': f'{name}.rgb565',
                        'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    previews[0].save(output / 'preview.gif', save_all=True, append_images=previews[1:],
                     duration=round(1000 / fps), loop=0)
    manifest = {'format': 'RGB565_LE', 'width': size, 'height': size, 'fps': fps,
                'frame_count': len(records), 'total_raw_bytes': sum(r['bytes'] for r in records),
                'firmware_integrated': False, 'frames': records}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--size', type=int, default=160)
    parser.add_argument('--fps', type=int, default=12)
    args = parser.parse_args()
    print(json.dumps(prepare(args.source, args.output, args.size, args.fps), indent=2))

if __name__ == '__main__':
    main()
