import hashlib
import importlib.util
import pathlib
import subprocess
import sys
import tempfile

import pytest
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives import hashes

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/publish_ota.py'


def publisher():
    spec = importlib.util.spec_from_file_location('publish_ota', SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def inputs(root):
    root.chmod(0o700)
    key = ec.generate_private_key(ec.SECP256R1())
    private = root / 'ota-private.pem'
    private.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                          serialization.NoEncryption()))
    private.chmod(0o600)
    blob = bytearray(4096)
    blob[0] = 0xe9
    blob[12:14] = (9).to_bytes(2, 'little')
    blob[32:36] = (0xABCD5432).to_bytes(4, 'little')
    blob[48:54] = b'0.3.7\x00'
    blob[80:91] = b'ki-m-box3\x00'
    image = root / 'app.bin'
    image.write_bytes(blob)
    return private, image, blob


def test_rejects_public_key_parent():
    mod = publisher()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        private, image, _ = inputs(root)
        root.chmod(0o755)
        with pytest.raises(ValueError, match='parent'):
            mod.publish(image, '0.3.7', private, root / 'restricted')
        assert not (root / 'restricted').exists()


def test_rejects_public_output_parent():
    mod = publisher()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        private, image, _ = inputs(root)
        public = root / 'public'
        public.mkdir(mode=0o755)
        with pytest.raises(ValueError, match='parent'):
            mod.publish(image, '0.3.7', private, public / 'release')
        assert not (public / 'release').exists()


def test_rejects_symlink_in_key_parent_path():
    mod = publisher()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        private, image, _ = inputs(root)
        (root / 'alias').symlink_to(root, target_is_directory=True)
        with pytest.raises((OSError, ValueError)):
            mod.publish(image, '0.3.7', root / 'alias' / private.name, root / 'restricted')
        assert not (root / 'restricted').exists()


def test_rejects_symlinked_image_and_key():
    mod = publisher()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        private, image, _ = inputs(root)
        (root / 'key-link').symlink_to(private)
        with pytest.raises((OSError, ValueError)):
            mod.publish(image, '0.3.7', root / 'key-link', root / 'restricted')
        (root / 'image-link').symlink_to(image)
        with pytest.raises((OSError, ValueError)):
            mod.publish(root / 'image-link', '0.3.7', private, root / 'restricted')


def test_output_directory_remains_pinned_after_path_swap(monkeypatch):
    mod = publisher()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        private, image, blob = inputs(root)
        output = root / 'release'
        decoy = root / 'decoy'
        decoy.mkdir(mode=0o700)
        real_open = mod.os.open
        swapped = False

        def swap_on_release(name, flags, *args, **kwargs):
            nonlocal swapped
            if name == hashlib.sha256(blob).hexdigest() + '.bin' and not swapped:
                swapped = True
                output.rename(root / 'moved')
                output.symlink_to(decoy, target_is_directory=True)
            return real_open(name, flags, *args, **kwargs)

        monkeypatch.setattr(mod.os, 'open', swap_on_release)
        mod.publish(image, '0.3.7', private, output)
        assert swapped
        assert (root / 'moved' / 'manifest.txt').exists()
        assert (root / 'moved' / 'images' / (hashlib.sha256(blob).hexdigest() + '.bin')).read_bytes() == blob
        assert list(decoy.iterdir()) == []


def test_image_is_read_through_bounded_descriptor(monkeypatch):
    mod = publisher()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        private, image, blob = inputs(root)
        original_read = mod.os.read
        sizes = []

        def recording_read(fd, size):
            sizes.append(size)
            return original_read(fd, size)

        monkeypatch.setattr(mod.os, 'read', recording_read)
        mod.publish(image, '0.3.7', private, root / 'release')
        assert sizes and max(sizes) <= mod.MAX_IMAGE + 1
        assert (root / 'release' / 'images' / (hashlib.sha256(blob).hexdigest() + '.bin')).exists()


def test_key_replacement_before_open_is_revalidated(monkeypatch):
    mod = publisher()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        private, image, _ = inputs(root)
        substitute = root / 'replacement.pem'
        substitute.write_bytes(private.read_bytes())
        substitute.chmod(0o644)
        real_open = mod.os.open
        swapped = False

        def swap_key(name, flags, *args, **kwargs):
            nonlocal swapped
            if name == private.name and not swapped:
                swapped = True
                substitute.replace(private)
            return real_open(name, flags, *args, **kwargs)

        monkeypatch.setattr(mod.os, 'open', swap_key)
        with pytest.raises(ValueError, match='signing key'):
            mod.publish(image, '0.3.7', private, root / 'release')
        assert swapped and not (root / 'release').exists()


def test_rejects_symlinked_output_and_images():
    mod = publisher()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        private, image, _ = inputs(root)
        decoy = root / 'decoy'
        decoy.mkdir(mode=0o700)
        output = root / 'release'
        output.symlink_to(decoy, target_is_directory=True)
        with pytest.raises((OSError, ValueError)):
            mod.publish(image, '0.3.7', private, output)
        output.unlink()
        output.mkdir(mode=0o700)
        (output / 'images').symlink_to(decoy, target_is_directory=True)
        with pytest.raises((OSError, ValueError)):
            mod.publish(image, '0.3.7', private, output)
        assert list(decoy.iterdir()) == []


def test_rejects_growth_during_bounded_read(monkeypatch):
    mod = publisher()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        private, image, _ = inputs(root)
        original_read = mod.os.read
        read_sizes = []

        def growing_read(fd, size):
            if mod.os.fstat(fd).st_size == image.stat().st_size:
                read_sizes.append(size)
                if len(read_sizes) > 1:
                    return b'X' * size
            return original_read(fd, size)

        monkeypatch.setattr(mod.os, 'read', growing_read)
        with pytest.raises(ValueError, match='exceeds permitted size'):
            mod.publish(image, '0.3.7', private, root / 'release')
        assert max(read_sizes) <= mod.MAX_IMAGE + 1
        assert not (root / 'release').exists()


def test_publish_signed_roundtrip_and_tamper():
    spec = importlib.util.spec_from_file_location('publish_ota', SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        key = ec.generate_private_key(ec.SECP256R1())
        private = root / 'ota-private.pem'
        private.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                               serialization.NoEncryption()))
        private.chmod(0o600)
        image = root / 'app.bin'
        blob = bytearray(4096)
        blob[0] = 0xe9
        blob[12:14] = (9).to_bytes(2, 'little')  # ESP32-S3 chip ID
        blob[32:36] = (0xABCD5432).to_bytes(4, 'little')
        blob[48:54] = b'0.3.7\x00'
        blob[80:91] = b'ki-m-box3\x00'
        image.write_bytes(blob)
        output = root / 'restricted'
        subprocess.run([sys.executable, str(SCRIPT), '--image', str(image), '--version', '0.3.7',
                        '--key', str(private), '--output', str(output)], check=True, capture_output=True)
        manifest = (output / 'manifest.txt').read_bytes()
        digest = hashlib.sha256(blob).hexdigest()
        published = output / 'images' / (digest + '.bin')
        assert published.read_bytes() == image.read_bytes()
        assert b'image=' + digest.encode() + b'.bin\n' in manifest
        assert output.stat().st_mode & 0o777 == 0o700
        assert (output / 'images').stat().st_mode & 0o777 == 0o700
        assert (output / 'manifest.txt').stat().st_mode & 0o777 == 0o600
        assert published.stat().st_mode & 0o777 == 0o600
        signed, signature = manifest.split(b'signature=')
        key.public_key().verify(bytes.fromhex(signature.strip().decode()), signed, ec.ECDSA(hashes.SHA256()))
        with pytest.raises(Exception):
            key.public_key().verify(bytes.fromhex(signature.strip().decode()), signed.replace(b'0.3.7', b'0.3.8'), ec.ECDSA(hashes.SHA256()))
        assert mod.verify(manifest, published.read_bytes(), key.public_key())
        assert not mod.verify(manifest, b'evil' + published.read_bytes()[4:], key.public_key())
        assert not mod.verify(manifest.replace(b'0.3.7', b'0.3.8'), published.read_bytes(), key.public_key())


def test_publish_rejects_insecure_output_and_key():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        image = root / 'image.bin'
        image.write_bytes(b'\xe9' * 2048)
        key = root / 'key.pem'
        k = ec.generate_private_key(ec.SECP256R1())
        key.write_bytes(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                        serialization.NoEncryption()))
        key.chmod(0o644)
        result = subprocess.run([sys.executable, str(SCRIPT), '--image', str(image), '--version', '0.3.7',
                                 '--key', str(key), '--output', str(root / 'private')], capture_output=True)
        assert result.returncode != 0
        assert not (root / 'private' / 'manifest.txt').exists()
        key.chmod(0o600)
        with image.open('wb') as stream:
            stream.truncate(4 * 1024 * 1024 + 1)
        result = subprocess.run([sys.executable, str(SCRIPT), '--image', str(image), '--version', '0.3.7',
                                 '--key', str(key), '--output', str(root / 'private')], capture_output=True)
        assert result.returncode != 0
        assert not (root / 'private').exists()
