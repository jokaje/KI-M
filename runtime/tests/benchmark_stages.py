"""Instrument original/current real engine; synthetic PCM in process memory only.
Run in an isolated inference-test container, not a new backend service.
"""
import importlib.util
import json
import logging
import os
import sys
import time
from pathlib import Path

import httpx

sys.path.insert(0, '/bench/current/tests')
from benchmark_tls import synthetic_pcm

# Reuse the existing Qwen endpoint from inside Docker, preserving request schema.
import benchmark_tls
original_http = benchmark_tls.http.client.HTTPConnection
class DockerHTTPConnection(original_http):
    def __init__(self, host, port=None, **kw):
        super().__init__('host.docker.internal' if host == '127.0.0.1' and port == 8093 else host, port, **kw)
benchmark_tls.http.client.HTTPConnection = DockerHTTPConnection
pcm = synthetic_pcm()
benchmark_tls.http.client.HTTPConnection = original_http
report = {'method': 'original and changed source, same in-memory synthetic PCM, real STT/Mimir/Ollama/Qwen; no device playback',
          'synthetic_input_generation_excluded': True, 'input_pcm_bytes': len(pcm), 'runs': []}
logging.getLogger('kim.voice').setLevel(logging.CRITICAL)
original_post = httpx.AsyncClient.post
for label, directory in [('before', '/bench/original'), ('after', '/bench/current')]:
    sys.path.insert(0, directory)
    for name in ['voice', 'character', 'memory_control']:
        sys.modules.pop(name, None)
    spec = importlib.util.spec_from_file_location('bench_engine_' + label, directory + '/engine.py')
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    captured = []
    base_metrics = module.Metrics
    class InstrumentedMetrics(base_metrics):
        def __init__(self):
            super().__init__()
            captured.append(self)
    module.Metrics = InstrumentedMetrics
    async def instrumented_post(self, url, *args, **kwargs):
        try:
            return await original_post(self, url, *args, **kwargs)
        finally:
            if str(url).endswith('/recall') and captured:
                captured[-1].mark('recall')
    httpx.AsyncClient.post = instrumented_post
    engine = module.Engine()
    for run in range(3):
        begun = time.monotonic()
        frames = []
        first = None
        for kind, data in engine.stream(pcm, lambda: False):
            frames.append((kind, len(data)))
            if kind == b'A' and first is None:
                first = time.monotonic() - begun
        completed = time.monotonic() - begun
        assert frames[-1][0] == b'E' and any(k == b'A' for k, _ in frames)
        marks = captured[-1].marks.copy()
        marks['response_complete'] = round(completed, 3)
        row = {'label': label, 'run': run + 1, 'cumulative_s': marks,
               'first_audio_consumed_s': round(first, 3),
               'audio_packets': sum(k == b'A' for k, _ in frames),
               'pcm_bytes': sum(n for k, n in frames if k == b'A'), 'terminal': 'E'}
        report['runs'].append(row)
        print('BENCHMARK_JSON ' + json.dumps(row), flush=True)
    del engine
    sys.path.remove(directory)
httpx.AsyncClient.post = original_post
print('BENCHMARK_REPORT ' + json.dumps(report), flush=True)
