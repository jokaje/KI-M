from types import SimpleNamespace
import socket
from test_transport_regression import Engine
from transport import Handler


def test_failed_legacy_wav_send_never_appends_stream_x():
    engine = Engine([(b'A', b'\x01\x00'), (b'E', b'')])
    handler = object.__new__(Handler)
    handler.server = SimpleNamespace(engine=engine)
    client, server = socket.socketpair()
    class FailedWavSend:
        def __init__(self): self.sent = []
        def settimeout(self, seconds): server.settimeout(seconds)
        def fileno(self): return server.fileno()
        def recv(self, *args): return server.recv(*args)
        def sendall(self, data):
            self.sent.append(data)
            if len(self.sent) == 1:
                raise BrokenPipeError('isolated partial WAV send')
    conn = FailedWavSend()
    try:
        handler.turn_reply(conn, b'\0\0' * 1600, '/v1/turn')
    finally:
        client.close(); server.close()
    assert len(conn.sent) == 1
    assert conn.sent[0].startswith(b'HTTP/1.1 200 OK')
    assert engine.closed
