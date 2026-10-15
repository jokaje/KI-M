"""Content-free real TLS regression benchmark; synthetic audio only in memory."""
import argparse
import datetime
import hashlib
import http.client
import io
import json
import os
import socket
import ssl
import struct
import subprocess
import time
import wave
from pathlib import Path

import numpy as np

PRIVATE = Path(os.environ.get('KIM_PRIVATE_DIR', Path.home() / '.local/share/ki-m'))
SERVER_NAME = os.environ.get('KIM_TLS_SERVER_NAME', 'ki-m.local')


def synthetic_pcm():
    conn = http.client.HTTPConnection('127.0.0.1', 8093, timeout=45)
    try:
        conn.request('POST', '/v1/audio/speech', json.dumps({
            'model': 'qwen3-tts-0.6b', 'voice': 'serena', 'language': 'German',
            'response_format': 'wav', 'input': 'Hallo KI-M. Wie geht es dir?'
        }), {'Content-Type': 'application/json'})
        response = conn.getresponse()
        if response.status != 200:
            raise RuntimeError('synthetic input synthesis failed')
        data = response.read(384045)
        if len(data) > 384044:
            raise ValueError('synthetic input bound')
        with wave.open(io.BytesIO(data), 'rb') as wav:
            assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (1, 2, 24000)
            audio = np.frombuffer(wav.readframes(wav.getnframes()), dtype='<i2').astype(np.float32)
        count = len(audio) * 2 // 3
        pcm = np.interp(np.arange(count) * 1.5, np.arange(len(audio)), audio).astype('<i2').tobytes()
        assert 3200 <= len(pcm) <= 256000
        return pcm
    finally:
        conn.close()


def exact(stream, n):
    data = stream.read(n)
    if len(data) != n:
        raise ValueError('truncated transport')
    return data


def turn(pcm, fallback=False):
    token = (PRIVATE / 'token').read_text().strip()
    context = ssl.create_default_context(cafile=str(PRIVATE / 'server.crt'))
    begin = time.monotonic()
    with socket.create_connection(('127.0.0.1', 18869), timeout=10) as raw:
        with context.wrap_socket(raw, server_hostname=SERVER_NAME) as sock:
            sock.settimeout(155)
            route = '/v1/turn' if fallback else '/v1/turn-stream'
            headers = (f'POST {route} HTTP/1.1\r\nHost: {SERVER_NAME}\r\n'
                       f'Authorization: Bearer {token}\r\n'
                       'Content-Type: application/vnd.kim.pcm;rate=16000;channels=1;format=s16le\r\n'
                       f'Content-Length: {len(pcm)}\r\n\r\n').encode('ascii')
            sent = time.monotonic()
            sock.sendall(headers + pcm)
            with sock.makefile('rb') as stream:
                header = bytearray()
                while not header.endswith(b'\r\n\r\n'):
                    header.extend(exact(stream, 1))
                    if len(header) > 8192:
                        raise ValueError('response header bound')
                assert header.startswith(b'HTTP/1.1 200 OK'), 'response status'
                count = total = nonzero = 0
                first = None
                if fallback:
                    fields = dict(line.split(':', 1) for line in header.decode('ascii').split('\r\n')[1:] if ':' in line)
                    size = int(fields['Content-Length'])
                    assert size <= 1280044
                    data = exact(stream, size)
                    with wave.open(io.BytesIO(data), 'rb') as wav:
                        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (1, 2, 16000)
                        audio = wav.readframes(wav.getnframes())
                    total = len(audio)
                    nonzero = int(np.count_nonzero(np.frombuffer(audio, dtype='<i2')))
                    assert total > 0 and nonzero > 0
                    first = time.monotonic() - sent
                    count = 1
                else:
                    assert b'Transfer-Encoding:' not in header and b'Content-Length:' not in header
                    while True:
                        kind = exact(stream, 1)
                        size = struct.unpack('<I', exact(stream, 4))[0]
                        assert size <= 256000
                        audio = exact(stream, size)
                        if kind == b'A':
                            assert size > 0 and size % 2 == 0
                            count += 1
                            total += size
                            assert count <= 16 and total <= 1280000
                            nonzero += int(np.count_nonzero(np.frombuffer(audio, dtype='<i2')))
                            if first is None:
                                first = time.monotonic() - sent
                        else:
                            assert kind == b'E' and size == 0, 'failure terminal'
                            assert count > 0 and nonzero > 0
                            break
                assert stream.read(1) == b'', 'trailing bytes'
                return {'route': route, 'first_audio_received_s': round(first, 3),
                        'complete_response_s': round(time.monotonic() - sent, 3),
                        'connection_setup_s': round(sent - begin, 3), 'audio_packets': count,
                        'pcm_bytes': total, 'nonzero_samples': nonzero}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--label', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--fallback', action='store_true')
    args = parser.parse_args()
    report = {'label': args.label, 'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'measurement': 'host loopback strict-CA TLS; not physical audible playback',
              'synthetic_input_generation_excluded': True, 'runs': []}
    pcm = synthetic_pcm()
    report['input_pcm_bytes'] = len(pcm)
    report['input_sha256'] = hashlib.sha256(pcm).hexdigest()
    report['container_image'] = subprocess.check_output(['docker', 'inspect', 'kim-voice-voice-1', '--format', '{{.Image}}'], text=True).strip()
    try:
        for index in range(args.runs):
            result = turn(pcm, args.fallback)
            result['run'] = index + 1
            report['runs'].append(result)
            print(json.dumps(result), flush=True)
    except Exception as error:
        report['failed'] = type(error).__name__
        raise
    finally:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
