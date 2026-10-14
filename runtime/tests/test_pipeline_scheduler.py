"""Exercise the real Pipecat scheduler, not just direct processor methods."""
import asyncio
import json
import threading
import time
from types import SimpleNamespace
from unittest.mock import patch

from engine import Engine, GermanVoice
from memory_control import MemoryControl
from pipecat.frames.frames import OutputAudioRawFrame


class Whisper:
    def transcribe(self, *args, **kwargs):
        return iter([SimpleNamespace(text='Ein neutraler Testsatz.', no_speech_prob=0)]), None


class Stream:
    def __init__(self, read): self.read = read
    async def __aenter__(self): return self
    async def __aexit__(self, *args): pass
    def raise_for_status(self): pass
    async def aiter_bytes(self):
        async for line in self.aiter_lines():
            yield (line + "\n").encode("utf-8")
    async def aiter_lines(self):
        for index in range(5):
            self.read.append(index)
            yield json.dumps({'message': {'content': f'Satz {index}. '}})
            await asyncio.sleep(0)
        yield json.dumps({'done': True})


class Client:
    def __init__(self, read): self.read = read
    async def __aenter__(self): return self
    async def __aexit__(self, *args): pass
    def stream(self, *args, **kwargs): return Stream(self.read)


def test_real_scheduler_strips_split_emotion_before_tts():
    seen = []
    class TaggedStream(Stream):
        async def aiter_lines(self):
            for delta in ('[ST', 'ATE:ha', 'ppy] Guten Tag. '):
                yield json.dumps({'message': {'content': delta}})
                await asyncio.sleep(0)
            yield json.dumps({'done': True})
    class TaggedClient(Client):
        def stream(self, *args, **kwargs): return TaggedStream(self.read)
    async def synthesis(self, text):
        seen.append(text)
        await self.publish_audio(b'\x01\x00')
    engine = Engine.__new__(Engine)
    engine.model = Whisper()
    engine.ollama, engine.tts = 'http://ollama', 'http://tts'
    engine.memory, engine._lock = MemoryControl(), threading.Lock()
    with patch('engine.system_prompt', return_value='Testpersona'), patch.dict('engine.os.environ', {'MIMIR_URL': ''}), patch('engine.httpx.AsyncClient', side_effect=lambda **kw: TaggedClient([])), patch.object(GermanVoice, '_synthesize', synthesis):
        result = list(engine.stream(b'\x00\x00' * 1600, lambda: False))
    assert [kind for kind, _ in result] == [b'M', b'A', b'E'], (seen, result)
    assert result[0][1] == b'\x01'
    assert seen == ['Guten Tag.']


def test_real_pipeline_applies_sentence_backpressure_and_overlap():
    read = []
    gate = threading.Event()
    entered = threading.Event()
    result = []
    errors = []
    async def synthesis(self, text):
        entered.set()
        while not gate.is_set():
            await asyncio.sleep(.01)
        await self.push_frame(OutputAudioRawFrame(b'\x01\x00', 16000, 1))
    engine = Engine.__new__(Engine)
    engine.model = Whisper()
    engine.ollama = 'http://ollama'
    engine.tts = 'http://tts'
    engine.memory = MemoryControl()
    engine._lock = threading.Lock()
    def consume():
        try:
            result.extend(engine.stream(b'\x00\x00' * 1600, lambda: False))
        except BaseException as e:
            errors.append(type(e).__name__)
    with patch('engine.system_prompt', return_value='Testpersona'), patch.dict('engine.os.environ', {'MIMIR_URL': ''}), patch('engine.httpx.AsyncClient', side_effect=lambda **kw: Client(read)), patch.object(GermanVoice, '_synthesize', synthesis):
        consumer = threading.Thread(target=consume)
        consumer.start()
        try:
            assert entered.wait(2)
            time.sleep(.15)
            # One synthesizing, two queued, fourth awaiting bounded publication.
            # The fifth sentence must not already sit in Pipecat's unbounded queue.
            assert len(read) == 4
        finally:
            gate.set()
            consumer.join(3)
    assert not consumer.is_alive() and not errors
    assert [kind for kind, _ in result] == [b'A'] * 5 + [b'E']
