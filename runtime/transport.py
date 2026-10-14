"""Strict close-delimited BOX-3 TLS/HTTP transport with bounded ingress."""
import hmac
import io
import os
import re
import stat
import wave
import select
import socket
import socketserver
import ssl
import struct
import threading
import time

MAX_PCM = 256000
OTA_MANIFEST_MAX = 8192
OTA_IMAGE_MAX = 4 * 1024 * 1024
OTA_IMAGE_NAME = re.compile(r'[0-9a-f]{64}\.bin\Z')
TYPE = 'application/vnd.kim.pcm;rate=16000;channels=1;format=s16le'


def read_private_ota(root, image_name=None):
    """Open each component relative to a private directory, without following links."""
    directory_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    file_flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC
    root_fd = os.open(root, directory_flags)
    try:
        if os.fstat(root_fd).st_mode & 0o077:
            raise ValueError('OTA directory is not private')
        directory_fd = root_fd
        if image_name is not None:
            directory_fd = os.open('images', directory_flags, dir_fd=root_fd)
        try:
            if os.fstat(directory_fd).st_mode & 0o077:
                raise ValueError('OTA images directory is not private')
            name = image_name if image_name is not None else 'manifest.txt'
            fd = os.open(name, file_flags, dir_fd=directory_fd)
            try:
                info = os.fstat(fd)
                limit = OTA_IMAGE_MAX if image_name is not None else OTA_MANIFEST_MAX
                if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077 or not 0 < info.st_size <= limit:
                    raise ValueError('invalid OTA file')
                chunks = []
                remaining = info.st_size
                while remaining:
                    chunk = os.read(fd, min(65536, remaining))
                    if not chunk:
                        raise ValueError('short OTA file')
                    chunks.append(chunk)
                    remaining -= len(chunk)
                if os.read(fd, 1) or os.fstat(fd).st_size != info.st_size:
                    raise ValueError('changed OTA file')
                return b''.join(chunks)
            finally:
                os.close(fd)
        finally:
            if directory_fd != root_fd:
                os.close(directory_fd)
    finally:
        os.close(root_fd)


def frame(kind, payload=b''):
    if kind not in (b'A', b'E', b'X', b'M') or not isinstance(payload, bytes) or (kind == b'A' and (not payload or len(payload) > MAX_PCM or len(payload) % 2)) or (kind == b'M' and payload != b'\x01') or (kind in (b'E', b'X') and payload):
        raise ValueError('invalid frame')
    return kind + struct.pack('<I', len(payload)) + payload


def read_until(sock, terminator, limit, deadline):
    buf = bytearray()
    while terminator not in buf:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or len(buf) >= limit:
            raise ValueError('header deadline or size')
        sock.settimeout(remaining)
        chunk = sock.recv(1)
        if time.monotonic() >= deadline or not chunk:
            raise ValueError('short header')
        buf.extend(chunk)
    return bytes(buf)


def read_body(sock, size, deadline):
    buf = bytearray()
    while len(buf) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ValueError('body deadline')
        sock.settimeout(remaining)
        chunk = sock.recv(min(16384, size - len(buf)))
        if time.monotonic() >= deadline or not chunk:
            raise ValueError('short body')
        buf.extend(chunk)
    return bytes(buf)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        raw = self.request
        raw.settimeout(3)
        try:
            conn = self.server.context.wrap_socket(raw, server_side=True)
            with conn:
                self.serve(conn)
        except (OSError, ssl.SSLError, ValueError):
            pass

    def reply(self, conn, status, payload=b'', headers=b'Content-Type: text/plain\r\n'):
        conn.sendall(b'HTTP/1.1 ' + status + b'\r\nCache-Control: no-store\r\nConnection: close\r\n' + headers + b'Content-Length: ' + str(len(payload)).encode() + b'\r\n\r\n' + payload)

    def serve(self, conn):
        try:
            header = read_until(conn, b'\r\n\r\n', 8192, time.monotonic() + 10)
            lines = header[:-4].decode('ascii').split('\r\n')
            first = lines.pop(0).split(' ')
            if len(first) != 3 or first[2] != 'HTTP/1.1':
                raise ValueError('request line')
            fields = {}
            for line in lines:
                if ':' not in line:
                    raise ValueError('header syntax')
                key, value = line.split(':', 1)
                key = key.strip().lower()
                if key in fields or not key or any(c.isspace() for c in key):
                    raise ValueError('duplicate or invalid header')
                fields[key] = value.strip()
        except (UnicodeError, OSError, ValueError):
            return
        method, path, _ = first
        if method == 'GET' and path == '/v1/health':
            if not hmac.compare_digest(fields.get('authorization', ''), 'Bearer ' + self.server.token):
                self.reply(conn, b'401 Unauthorized'); return
            self.reply(conn, b'204 No Content'); return
        if path == '/v1/ota/manifest' or path.startswith('/v1/ota/images/'):
            if not hmac.compare_digest(fields.get('authorization', ''), 'Bearer ' + self.server.token):
                self.reply(conn, b'401 Unauthorized'); return
            if method != 'GET' or not self.server.ota_dir:
                self.reply(conn, b'404 Not Found'); return
            image_name = None
            if path != '/v1/ota/manifest':
                image_name = path[len('/v1/ota/images/'):]
                if not OTA_IMAGE_NAME.fullmatch(image_name):
                    self.reply(conn, b'404 Not Found'); return
            try:
                payload = read_private_ota(self.server.ota_dir, image_name)
            except (OSError, ValueError):
                self.reply(conn, b'404 Not Found'); return
            self.reply(conn, b'200 OK', payload, b'Content-Type: application/octet-stream\r\n')
            return
        if method != 'POST' or path not in ('/v1/turn-stream', '/v1/turn'):
            self.reply(conn, b'404 Not Found'); return
        if not hmac.compare_digest(fields.get('authorization', ''), 'Bearer ' + self.server.token):
            self.reply(conn, b'401 Unauthorized'); return
        length = fields.get('content-length', '')
        if ('transfer-encoding' in fields or not length.isdecimal() or
            not 3200 <= int(length) <= MAX_PCM or int(length) % 2 or
            fields.get('content-type') != TYPE):
            self.reply(conn, b'422 Unprocessable Entity'); return
        try:
            pcm = read_body(conn, int(length), time.monotonic() + 10)
        except (OSError, ValueError):
            self.reply(conn, b'422 Unprocessable Entity'); return
        if not self.server.turn.acquire(blocking=False):
            self.reply(conn, b'503 Service Unavailable'); return
        try:
            self.turn_reply(conn, pcm, path, fields.get('x-kim-capabilities', '') == 'emotion-v1')
        finally:
            self.server.turn.release()

    def turn_reply(self, conn, pcm, path, emotions=False):
        deadline = time.monotonic() + 150
        streaming = path == '/v1/turn-stream'
        started = False
        count = total = 0
        parts = []

        def disconnected():
            if time.monotonic() >= deadline:
                return True
            ready, _, _ = select.select([conn], [], [], 0)
            if not ready:
                return False
            try:
                return not conn.recv(1, socket.MSG_PEEK)
            except (OSError, ssl.SSLError, ValueError):
                # SSL sockets do not support MSG_PEEK: use the regular read there.
                try:
                    return not conn.recv(1)
                except (OSError, ssl.SSLError):
                    return True

        try:
            with self.server.engine._lock:
                source = self.server.engine.stream(pcm, disconnected)
                try:
                    if streaming:
                        conn.settimeout(min(10, max(0.001, deadline - time.monotonic())))
                        conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: application/vnd.kim.stream-v1\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n')
                        started = True
                    for kind, payload in source:
                        if time.monotonic() >= deadline or disconnected():
                            raise TimeoutError('turn expired or disconnected')
                        if kind == b'M':
                            frame(kind, payload)  # validate even when the peer cannot use it
                            if streaming and emotions and not count:
                                conn.settimeout(min(10, max(0.001, deadline - time.monotonic())))
                                conn.sendall(frame(kind, payload))
                            continue
                        encoded = frame(kind, payload)
                        if kind == b'X':
                            if streaming:
                                conn.settimeout(min(10, max(0.001, deadline - time.monotonic())))
                                conn.sendall(encoded)
                                return
                            raise ValueError('worker error')
                        if kind == b'E':
                            if not count:
                                raise ValueError('empty success')
                            if streaming:
                                conn.settimeout(min(10, max(0.001, deadline - time.monotonic())))
                                conn.sendall(encoded)
                            else:
                                # Firmware valid_wav requires a canonical 44-byte header.
                                with io.BytesIO() as buffer:
                                    with wave.open(buffer, 'wb') as wav:
                                        wav.setnchannels(1)
                                        wav.setsampwidth(2)
                                        wav.setframerate(16000)
                                        wav.writeframes(b''.join(parts))
                                    audio = buffer.getvalue()
                                started = True  # a partial 200 cannot be replaced by a 502
                                self.reply(conn, b'200 OK', audio,
                                           b'Content-Type: audio/wav\r\nX-KIM-Text: \r\n')
                            return
                        count += 1
                        total += len(payload)
                        if count > 16 or total > 1280000:
                            raise ValueError('turn audio bound')
                        if streaming:
                            conn.settimeout(min(10, max(0.001, deadline - time.monotonic())))
                            conn.sendall(encoded)
                        else:
                            parts.append(payload)
                    raise ValueError('missing terminal')
                finally:
                    if hasattr(source, 'close'):
                        source.close()
        except (OSError, ValueError, TimeoutError, RuntimeError):
            if started:
                if not streaming:
                    return  # A broken WAV reply must close, never append stream frames.
                try:
                    conn.settimeout(min(10, max(0.001, deadline - time.monotonic())))
                    conn.sendall(frame(b'X'))
                except OSError:
                    pass
            else:
                try:
                    self.reply(conn, b'502 Bad Gateway')
                except OSError:
                    pass


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True
    def __init__(self, address, context, token, engine, ota_dir=None):
        self.context, self.token, self.engine = context, token, engine
        self.ota_dir = ota_dir
        self.slots = threading.BoundedSemaphore(8)
        self.turn = threading.BoundedSemaphore(1)
        super().__init__(address, Handler)
    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            request.close(); return
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise
    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()
    def handle_error(self, request, address):
        pass


def main():
    from engine import Engine
    token = open('/run/kim-secrets/token', encoding='ascii').read().strip()
    if len(token) < 32 or not all(33 <= ord(c) <= 126 for c in token):
        raise RuntimeError('invalid device token')
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain('/run/kim-secrets/server.crt', '/run/kim-secrets/server.key')
    engine = Engine()
    with Server(('0.0.0.0', 18869), context, token, engine,
                ota_dir=os.environ.get('KIM_OTA_DIR') or None) as server:
        server.serve_forever()

if __name__ == '__main__':
    main()
