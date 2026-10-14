"""Socket-level private OTA route contract; no live services or credentials."""
import socket
import threading

import pytest

from transport import Handler

TOKEN = 't' * 32
DIGEST = 'a' * 64


def fetch(root, path='/v1/ota/manifest', method='GET', auth=True):
    client, server = socket.socketpair()
    client.settimeout(3)
    handler = object.__new__(Handler)
    handler.server = type('Config', (), {'token': TOKEN, 'ota_dir': root})()
    request = (f'{method} {path} HTTP/1.1\r\n' +
               (f'Authorization: Bearer {TOKEN}\r\n' if auth else '') + '\r\n').encode()
    def run():
        with server:
            handler.serve(server)
    thread = threading.Thread(target=run)
    thread.start()
    with client:
        client.sendall(request)
        reply = bytearray()
        while chunk := client.recv(65536):
            reply.extend(chunk)
    thread.join(3)
    assert not thread.is_alive()
    head, body = bytes(reply).split(b'\r\n\r\n', 1)
    return head, body


@pytest.fixture
def bundle(tmp_path):
    tmp_path.chmod(0o700)
    (tmp_path / 'manifest.txt').write_bytes(b'KIM-OTA-v1\n')
    (tmp_path / 'manifest.txt').chmod(0o600)
    images = tmp_path / 'images'
    images.mkdir(mode=0o700)
    (images / (DIGEST + '.bin')).write_bytes(b'firmware')
    (images / (DIGEST + '.bin')).chmod(0o600)
    return tmp_path


def test_ota_serves_exact_files_with_content_length(bundle):
    for path, expected in [('/v1/ota/manifest', b'KIM-OTA-v1\n'),
                           ('/v1/ota/images/' + DIGEST + '.bin', b'firmware')]:
        head, body = fetch(str(bundle), path)
        assert head.startswith(b'HTTP/1.1 200 OK\r\n')
        assert b'Content-Length: ' + str(len(expected)).encode() + b'\r\n' in head + b'\r\n'
        assert b'Cache-Control: no-store' in head
        assert body == expected


def test_ota_disabled_and_auth_precedes_lookup(bundle):
    assert b'404 Not Found' in fetch(None)[0]
    assert b'401 Unauthorized' in fetch(str(bundle), auth=False)[0]
    assert b'401 Unauthorized' in fetch(str(bundle), '/v1/ota/images/notfound.bin', auth=False)[0]


@pytest.mark.parametrize('path', ['/v1/ota/images/', '/v1/ota/images/../manifest.txt',
    '/v1/ota/images/%2e%2e/manifest.txt', '/v1/ota/images/' + 'A'*64 + '.bin',
    '/v1/ota/images/' + 'a'*63 + '.bin', '/v1/ota/images/' + DIGEST + '.bin/extra',
    '/v1/ota/manifest/', '/v1/ota/images/' + DIGEST + '.bin?x=1'])
def test_ota_rejects_noncanonical_paths(bundle, path):
    assert b'404 Not Found' in fetch(str(bundle), path)[0]


def test_ota_get_only(bundle):
    assert b'200 OK' not in fetch(str(bundle), method='HEAD')[0]
    assert b'200 OK' not in fetch(str(bundle), method='POST')[0]


def test_ota_rejects_symlinks_and_private_mode(bundle, tmp_path):
    outside = tmp_path.parent / 'outside-ota-test.bin'
    outside.write_bytes(b'secret')
    image = bundle / 'images' / (DIGEST + '.bin')
    image.unlink()
    image.symlink_to(outside)
    assert b'200 OK' not in fetch(str(bundle), '/v1/ota/images/' + DIGEST + '.bin')[0]
    image.unlink()
    image.write_bytes(b'public')
    image.chmod(0o644)
    assert b'200 OK' not in fetch(str(bundle), '/v1/ota/images/' + DIGEST + '.bin')[0]
    bundle.chmod(0o755)
    assert b'200 OK' not in fetch(str(bundle))[0]


def test_ota_rejects_symlinked_images_directory(bundle, tmp_path):
    images = bundle / 'images'
    images.rename(bundle / 'other-images')
    images.symlink_to(bundle / 'other-images', target_is_directory=True)
    assert b'200 OK' not in fetch(str(bundle), '/v1/ota/images/' + DIGEST + '.bin')[0]


def test_ota_rejects_oversized_and_empty(bundle):
    manifest = bundle / 'manifest.txt'
    manifest.write_bytes(b'm' * (8192 + 1))
    assert b'200 OK' not in fetch(str(bundle))[0]
    manifest.write_bytes(b'')
    assert b'200 OK' not in fetch(str(bundle))[0]
    image = bundle / 'images' / (DIGEST + '.bin')
    with image.open('wb') as file:
        file.truncate(4 * 1024 * 1024 + 1)
    assert b'200 OK' not in fetch(str(bundle), '/v1/ota/images/' + DIGEST + '.bin')[0]
