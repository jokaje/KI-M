"""Neutral in-memory German synthetic voice probe; does not log content/audio."""
import http.client
import io
import json
import os
import socket
import ssl
import struct
import time
import wave
from pathlib import Path

import numpy as np

PRIVATE = Path(os.environ.get('KIM_PRIVATE_DIR', Path.home() / '.local/share/ki-m'))
SERVER_NAME = os.environ.get('KIM_TLS_SERVER_NAME', 'ki-m.local')
START = time.monotonic()


def elapsed():
    return round(time.monotonic() - START, 3)


def synth_input():
    connection = http.client.HTTPConnection('127.0.0.1', 8093, timeout=40)
    try:
        connection.request('POST', '/v1/audio/speech', json.dumps({
            'model': 'qwen3-tts-0.6b', 'voice': 'serena', 'language': 'German',
            'response_format': 'wav', 'input': 'Hallo KI-M. Wie geht es dir?'
        }), {'Content-Type': 'application/json'})
        response = connection.getresponse()
        assert response.status == 200
        data = response.read(500000)
        with wave.open(io.BytesIO(data)) as wav:
            assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (1, 2, 24000)
            audio = np.frombuffer(wav.readframes(wav.getnframes()), dtype='<i2').astype(np.float32)
        count = len(audio) * 2 // 3
        pcm = np.interp(np.arange(count) * 1.5, np.arange(len(audio)), audio).astype('<i2').tobytes()
        assert 3200 <= len(pcm) <= 256000
        return pcm
    finally:
        connection.close()


def main():
    pcm = synth_input()
    print({'input_ready_s': elapsed(), 'input_pcm_bytes': len(pcm)})
    token = (PRIVATE / 'token').read_text().strip()
    ctx = ssl.create_default_context(cafile=str(PRIVATE / 'server.crt'))
    with socket.create_connection(('127.0.0.1', 18869), timeout=150) as raw:
        with ctx.wrap_socket(raw, server_hostname=SERVER_NAME) as sock:
            sock.settimeout(150)
            request = (f'POST /v1/turn-stream HTTP/1.1\r\nHost: {SERVER_NAME}\r\n'.encode('ascii')
                       + b'Authorization: Bearer ' + token.encode() + b'\r\n'
                       + b'Content-Type: application/vnd.kim.pcm;rate=16000;channels=1;format=s16le\r\n'
                       + b'Content-Length: ' + str(len(pcm)).encode() + b'\r\n\r\n' + pcm)
            sent_at = elapsed()
            sock.sendall(request)
            stream = sock.makefile('rb')
            headers = bytearray()
            while not headers.endswith(b'\r\n\r\n'):
                headers.extend(stream.read(1))
                assert len(headers) < 4096
            assert headers.startswith(b'HTTP/1.1 200 OK'), headers[:32]
            assert b'Transfer-Encoding' not in headers and b'Content-Length' not in headers
            count = total = nonzero = 0
            first_audio = None
            while True:
                kind = stream.read(1)
                assert kind, 'missing terminal frame'
                length = struct.unpack('<I', stream.read(4))[0]
                assert length <= 256000
                audio = stream.read(length)
                assert len(audio) == length
                if kind == b'A':
                    count += 1; total += length
                    nonzero += np.count_nonzero(np.frombuffer(audio, dtype='<i2'))
                    first_audio = first_audio or elapsed()
                else:
                    assert kind == b'E', 'backend returned failure frame'
                    assert count > 0 and nonzero > 0
                    assert stream.read(1) == b''
                    break
            print({'request_sent_s': sent_at, 'first_audio_s': first_audio,
                   'full_s': elapsed(), 'audio_chunks': count,
                   'pcm_bytes': total, 'nonzero_samples': nonzero})

if __name__ == '__main__':
    main()
