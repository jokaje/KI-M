"""In-memory transport contract tests: never call real STT/TTS or storage."""
import io
import socket
import struct
import threading
import wave
from types import SimpleNamespace

import pytest

from transport import Handler, TYPE, frame


class Engine:
    def __init__(self, events):
        self._lock = threading.Lock()
        self.events = events
        self.closed = False
        self.calls = 0

    def stream(self, pcm, socket_closed):
        self.calls += 1
        try:
            yield from self.events
        finally:
            self.closed = True


def request(events, path='/v1/turn', body=b'\0\0' * 1600, headers=b'', engine=None):
    engine = engine or Engine(events)
    client, server = socket.socketpair()
    client.settimeout(2)
    holder = SimpleNamespace(token='t' * 32, engine=engine, turn=threading.Lock())
    handler = object.__new__(Handler)
    handler.server = holder
    req = (b'POST ' + path.encode() + b' HTTP/1.1\r\nAuthorization: Bearer ' + holder.token.encode() +
           b'\r\nContent-Type: ' + TYPE.encode() + b'\r\nContent-Length: ' + str(len(body)).encode() +
           b'\r\n' + headers + b'\r\n' + body)
    def serve():
        with server:
            handler.serve(server)
    thread = threading.Thread(target=serve)
    thread.start()
    try:
        client.sendall(req)
        chunks = []
        while True:
            try:
                part = client.recv(65536)
            except ConnectionResetError:  # rejected request leaves unread body on the server
                break
            if not part:
                break
            chunks.append(part)
    finally:
        client.close()
        thread.join(timeout=3)
    assert not thread.is_alive()
    return b''.join(chunks), engine


def wav_data(reply):
    header, data = reply.split(b'\r\n\r\n', 1)
    with wave.open(io.BytesIO(data), 'rb') as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) == (1, 2, 16000, 'NONE')
        assert wav.getnframes() * 2 == len(data) - 44
    assert struct.unpack_from('<I', data, 4)[0] == len(data) - 8
    assert struct.unpack_from('<I', data, 40)[0] == len(data) - 44
    assert int(dict(line.split(b': ', 1) for line in header.split(b'\r\n')[1:])[b'Content-Length']) == len(data)
    return header, data[44:]


def test_legacy_complete_wav_and_safe_caption():
    reply, engine = request([(b'A', b'\x01\x00' * 1600), (b'A', b'\x02\x00' * 1600), (b'E', b'')])
    header, pcm = wav_data(reply)
    assert reply.startswith(b'HTTP/1.1 200 OK\r\n')
    assert b'Content-Type: audio/wav\r\n' in header
    assert b'X-KIM-Text: ' in header
    assert pcm == b'\x01\x00' * 1600 + b'\x02\x00' * 1600
    assert engine.closed


@pytest.mark.parametrize('events', [[], [(b'X', b'')], [(b'A', b'\x01\x00')], [(b'E', b'')],
    [(b'A', b'odd'), (b'E', b'')], [(b'A', b'\x01\x00'), (b'X', b'')],
    [(b'A', b'\x01\x00')] * 17 + [(b'E', b'')],
    [(b'A', b'\x01\x00' * 128000)] * 6 + [(b'E', b'')]])
def test_legacy_fails_closed(events):
    reply, engine = request(events)
    assert not reply.startswith(b'HTTP/1.1 200 OK')
    assert engine.closed


def test_stream_preserves_raw_frames_and_closes_generator():
    events = [(b'A', b'\x01\x00' * 1600), (b'E', b'')]
    reply, engine = request(events, path='/v1/turn-stream')
    assert reply.split(b'\r\n\r\n', 1)[1] == b''.join(frame(*event) for event in events)
    assert engine.closed


def test_emotion_capability_is_explicit_and_legacy_wire_unchanged():
    events = [(b'M', b'\x01'), (b'A', b'\x01\x00' * 1600), (b'E', b'')]
    old, _ = request(events, path='/v1/turn-stream')
    new, _ = request(events, path='/v1/turn-stream', headers=b'X-KIM-Capabilities: emotion-v1\r\n')
    legacy_wav, _ = request(events)
    assert old.split(b'\r\n\r\n', 1)[1] == b''.join(frame(*item) for item in events[1:])
    assert new.split(b'\r\n\r\n', 1)[1] == b''.join(frame(*item) for item in events)
    assert wav_data(legacy_wav)[1] == events[1][1]


def test_stream_missing_e_reports_x_and_closes_generator():
    reply, engine = request([(b'A', b'\x01\x00')], path='/v1/turn-stream')
    assert reply.endswith(frame(b'X'))
    assert engine.closed


def test_generator_closed_on_send_error_and_lock_released():
    engine = Engine([(b'A', b'\x01\x00'), (b'E', b'')])
    handler = object.__new__(Handler)
    handler.server = SimpleNamespace(engine=engine)
    client, server = socket.socketpair()
    class FailedSend:
        def __init__(self):
            self.calls = 0
        def settimeout(self, seconds):
            server.settimeout(seconds)
        def sendall(self, data):
            self.calls += 1
            if self.calls == 2:
                raise BrokenPipeError('peer closed')
            server.sendall(data)
        def fileno(self):
            return server.fileno()
        def recv(self, *args):
            return server.recv(*args)
    try:
        handler.turn_reply(FailedSend(), b'\0\0' * 1600, '/v1/turn-stream')
    finally:
        client.close()
        server.close()
    assert engine.calls == 1
    assert engine.closed
    assert not engine._lock.locked()


def test_generator_closed_when_worker_raises():
    class BrokenEngine(Engine):
        def stream(self, pcm, socket_closed):
            self.calls += 1
            try:
                yield b'A', b'\x01\x00'
                raise RuntimeError('inference failed')
            finally:
                self.closed = True
    reply, engine = request([], engine=BrokenEngine([]))
    assert b'502 Bad Gateway' in reply
    assert engine.closed


def test_reject_input_before_engine():
    reply, engine = request([], body=b'\0' * 3198)
    assert b'422 Unprocessable Entity' in reply
    assert engine.calls == 0
