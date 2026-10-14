#!/usr/bin/env python3
"""Publish a private, P-256 signed KI-M OTA bundle; never serve this directory publicly.

Usage: publish_ota.py --image app.bin --version 0.3.7 --key /secure/ota.pem
                      --output /restricted/ki-m-release
The output contains manifest.txt and images/<sha256>.bin, all private.
The key is an operator-generated P-256 PKCS8 PEM outside the repository (mode 0600).
The key and output parents must already be operator-owned private directories (mode 0700).
Pin its distinct public key PEM in KIM_OTA_PUBLIC_KEY_PEM on the device.
Requires cryptography. This tool never generates or stores a development signing key.
"""
import argparse
from contextlib import ExitStack
import hashlib
import os
from pathlib import Path
import re
import stat

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

VERSION = re.compile(r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z')
MAX_IMAGE = 4 * 1024 * 1024


def payload(version, image_name, blob):
    if not VERSION.fullmatch(version) or len(version) >= 32:
        raise ValueError('invalid version')
    digest = hashlib.sha256(blob).hexdigest()
    if image_name != digest + '.bin':
        raise ValueError('image name must match sha256 digest')
    if not 1024 <= len(blob) <= MAX_IMAGE:
        raise ValueError('invalid image size')
    # IDF v6 esp_image_header_t (24), segment header (8), esp_app_desc_t (256).
    # Full binary integrity/chip validation remains esp_ota_end()'s responsibility.
    def cstring(part):
        return part.split(b'\x00', 1)[0].decode('ascii') if b'\x00' in part else ''
    if (blob[0] != 0xe9 or int.from_bytes(blob[12:14], 'little') != 9 or
        int.from_bytes(blob[32:36], 'little') != 0xABCD5432 or
        cstring(blob[48:80]) != version or cstring(blob[80:112]) != 'ki-m-box3'):
        raise ValueError('image is not a matching ESP32-S3 KI-M app')
    return (f'KIM-OTA-v1\nversion={version}\nmodel=ki-m-box3\nsize={len(blob)}\n'
            f'sha256={digest}\nimage={image_name}\n').encode('ascii')


def verify(manifest, blob, public_key):
    try:
        signed, signature = manifest.split(b'signature=')
        if not manifest.endswith(b'\n') or manifest.count(b'signature=') != 1:
            return False
        fields = signed.decode('ascii').splitlines()
        if len(fields) != 6 or fields[0] != 'KIM-OTA-v1' or fields[2] != 'model=ki-m-box3':
            return False
        version, size, digest, image = [fields[i].split('=', 1)[1] for i in (1, 3, 4, 5)]
        if signed != payload(version, image, blob) or int(size) != len(blob) or digest != hashlib.sha256(blob).hexdigest():
            return False
        public_key.verify(bytes.fromhex(signature.strip().decode('ascii')), signed, ec.ECDSA(hashes.SHA256()))
        return True
    except (ValueError, UnicodeError, IndexError, TypeError, OverflowError):
        return False
    except Exception as exc:
        from cryptography.exceptions import InvalidSignature
        if isinstance(exc, InvalidSignature):
            return False
        raise


def _parts(path):
    path = Path(path)
    if '..' in path.parts:
        raise ValueError('parent traversal is not allowed')
    return Path(os.path.abspath(path)).parts[1:]


def _parent(path, stack, private=False):
    parts = _parts(path)
    if not parts:
        raise ValueError('path must name a file or directory')
    fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    stack.callback(os.close, fd)
    for component in parts[:-1]:
        next_fd = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
        stack.callback(os.close, next_fd)
        fd = next_fd
        info = os.fstat(fd)
        if info.st_uid not in (os.getuid(), 0):
            raise ValueError('path ancestor must be owned by operator or root')
    if private:
        info = os.fstat(fd)
        if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700:
            raise ValueError('private parent directory must be operator-owned mode 0700')
    return fd, parts[-1]


def _private_file(parent, name, stack, label):
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
    stack.callback(os.close, fd)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1:
        raise ValueError(f'{label} must be an operator-owned regular private file, mode 0600')
    return fd


def _read_limited(fd, maximum):
    data = bytearray()
    while len(data) <= maximum:
        chunk = os.read(fd, min(65536, maximum + 1 - len(data)))
        if not chunk:
            return bytes(data)
        data.extend(chunk)
    raise ValueError('file exceeds permitted size')


def _write_new(parent, name, contents):
    fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=parent)
    with os.fdopen(fd, 'wb') as stream:
        os.fchmod(stream.fileno(), 0o600)
        stream.write(contents)


def publish(image, version, key_file, output):
    image, key_file, output = Path(image), Path(key_file), Path(output)
    repository = Path(__file__).resolve().parents[1]
    if Path(os.path.abspath(key_file)).is_relative_to(repository):
        raise ValueError('key must be outside repository')
    if Path(os.path.abspath(output)).is_relative_to(repository):
        raise ValueError('output must be outside repository in a restricted directory')
    with ExitStack() as stack:
        key_parent, key_name = _parent(key_file, stack, private=True)
        key_fd = _private_file(key_parent, key_name, stack, 'signing key')
        key_bytes = _read_limited(key_fd, 65536)
        key = serialization.load_pem_private_key(key_bytes, password=None)
        if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
            raise ValueError('OTA signing key must be P-256 EC')
        image_parent, image_name = _parent(image, stack)
        image_fd = os.open(image_name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=image_parent)
        stack.callback(os.close, image_fd)
        image_info = os.fstat(image_fd)
        if not stat.S_ISREG(image_info.st_mode) or image_info.st_size > MAX_IMAGE:
            raise ValueError('image must be a regular file at most 4 MiB')
        blob = _read_limited(image_fd, MAX_IMAGE)
        image_name = hashlib.sha256(blob).hexdigest() + '.bin'
        signed = payload(version, image_name, blob)
        signature = key.sign(signed, ec.ECDSA(hashes.SHA256()))
        manifest = signed + b'signature=' + signature.hex().encode('ascii') + b'\n'
        if not verify(manifest, blob, key.public_key()):
            raise ValueError('bundle verification failed')

        output_parent, output_name = _parent(output, stack, private=True)
        try:
            os.mkdir(output_name, 0o700, dir_fd=output_parent)
        except FileExistsError:
            pass
        output_fd = os.open(output_name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=output_parent)
        stack.callback(os.close, output_fd)
        output_info = os.fstat(output_fd)
        if output_info.st_uid != os.getuid() or stat.S_IMODE(output_info.st_mode) != 0o700:
            raise ValueError('output directory must be operator-owned mode 0700')
        try:
            os.mkdir('images', 0o700, dir_fd=output_fd)
        except FileExistsError:
            pass
        images_fd = os.open('images', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=output_fd)
        stack.callback(os.close, images_fd)
        images_info = os.fstat(images_fd)
        if images_info.st_uid != os.getuid() or stat.S_IMODE(images_info.st_mode) != 0o700:
            raise ValueError('images directory must be operator-owned mode 0700')
        for parent, name in ((images_fd, image_name), (output_fd, 'manifest.txt')):
            try:
                os.stat(name, dir_fd=parent, follow_symlinks=False)
            except FileNotFoundError:
                continue
            raise ValueError('refusing to overwrite existing release file')
        _write_new(images_fd, image_name, blob)
        _write_new(output_fd, 'manifest.txt', manifest)
        return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('image', 'version', 'key', 'output'):
        parser.add_argument('--' + option, required=True)
    args = parser.parse_args()
    publish(args.image, args.version, args.key, args.output)
    print('Signed OTA bundle written to restricted output directory (not public).')


if __name__ == '__main__':
    main()
