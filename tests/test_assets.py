import importlib.util
import tempfile
import unittest
from pathlib import Path
from PIL import Image

MODULE = Path(__file__).parents[1] / 'tools' / 'prepare_assets.py'
spec = importlib.util.spec_from_file_location('assets', MODULE)
assets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assets)

class AssetTests(unittest.TestCase):
    def test_red_rgb565_little_endian(self):
        self.assertEqual(assets.rgb565(Image.new('RGB', (1, 1), 'red')), b'\x00\xf8')

    def test_transparency_composited_black(self):
        image = Image.new('RGBA', (1, 1), (255, 0, 0, 0))
        self.assertEqual(assets.rgb565(image), b'\x00\x00')

    def test_pack_and_preview(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'source'
            source.mkdir()
            Image.new('RGBA', (20, 40), 'white').save(source / 'frame_000.png')
            manifest = assets.prepare(source, root / 'out', 160, 12)
            self.assertEqual(manifest['frame_count'], 1)
            self.assertEqual((root / 'out/frame_000.rgb565').stat().st_size, 160 * 160 * 2)
            with Image.open(root / 'out/frame_000.png') as preview:
                self.assertEqual(preview.size, (160, 160))
            self.assertTrue((root / 'out/preview.gif').exists())

    def test_reject_empty(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                assets.prepare(Path(tmp), Path(tmp) / 'out', 160, 12)

    def test_reject_invalid_rate(self):
        with self.assertRaises(ValueError):
            assets.prepare(Path('.'), Path('out'), 160, 0)

    def test_reject_existing_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(FileExistsError):
                assets.prepare(Path(tmp), Path(tmp), 160, 12)

if __name__ == '__main__':
    unittest.main()
