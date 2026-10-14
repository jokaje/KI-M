"""Failures through the actual Pipecat scheduler; no production services or memory writes."""
import asyncio
import json
import threading
import time
from types import SimpleNamespace
from unittest.mock import patch

import pytest

from engine import Engine, GermanVoice, OllamaSentences
from memory_control import MemoryControl
from pipecat.frames.frames import OutputAudioRawFrame


class Whisper:
    def transcribe(self, *args, **kwargs):
        return iter([SimpleNamespace(text='Hallo.', no_speech_prob=0)]), None


class Client:
    def __init__(self, lines):
        self.lines = lines
    async def __aenter__(self): return self
    async def __aexit__(self, *args): pass
    def stream(self, *args, **kwargs): return self
    def raise_for_status(self): pass
    async def aiter_bytes(self):
        async for line in self.aiter_lines():
            yield (line + "\n").encode("utf-8")
    async def aiter_lines(self):
        for line in self.lines:
            yield line
            await asyncio.sleep(0)


def engine_for(model=None):
    engine = Engine.__new__(Engine)
    engine.model = model or Whisper()
    engine.ollama = 'http://ollama'
    engine.tts = 'http://tts'
    engine.memory = MemoryControl()
    engine._lock = threading.Lock()
    return engine


def run_stream(engine, *, lines=(), synth=None, closed=lambda: False):
    if synth is None:
        async def synth(self, text):
            await self.push_frame(OutputAudioRawFrame(b'\x01\x00', 16000, 1))
    result = []
    errors = []
    def consume():
        try:
            result.extend(engine.stream(b'\x00\x00' * 1600, closed))
        except BaseException as error:
            errors.append(error)
    with patch('engine.system_prompt', return_value='Testpersona'), patch.dict('engine.os.environ', {'MIMIR_URL': ''}), patch('engine.httpx.AsyncClient', side_effect=lambda **kw: Client(lines)), patch.object(GermanVoice, '_synthesize', synth):
        worker = threading.Thread(target=consume, daemon=True)
        worker.start()
        worker.join(4)
    assert not worker.is_alive(), 'pipeline stalled'
    assert not errors
    return [kind for kind, _ in result]


def test_stt_exception_is_terminal_error():
    class BrokenWhisper:
        def transcribe(self, *args, **kwargs): raise RuntimeError('private transcription')
    assert run_stream(engine_for(BrokenWhisper())) == [b'X']


def test_malformed_ollama_after_audio_is_error_not_success():
    lines = [json.dumps({'message': {'content': 'Hallo. '}}), '{broken']
    assert run_stream(engine_for(), lines=lines) == [b'A', b'X']


def test_tts_failure_is_terminal_error():
    async def broken(self, text): raise RuntimeError('private synthesis')
    lines = [json.dumps({'message': {'content': 'Hallo. '}}), json.dumps({'done': True})]
    assert run_stream(engine_for(), lines=lines, synth=broken) == [b'X']


def test_sentence_queue_failure_is_terminal_error_after_audio():
    original = GermanVoice.enqueue
    calls = 0
    async def broken(self, text):
        nonlocal calls
        calls += 1
        if calls == 2:
            raise RuntimeError('private queue state')
        await original(self, text)
    lines = [json.dumps({'message': {'content': 'Hallo. Noch eins. '}}), json.dumps({'done': True})]
    with patch.object(GermanVoice, 'enqueue', broken):
        assert run_stream(engine_for(), lines=lines) == [b'A', b'X']


def test_abandoned_whisper_remains_serialized_across_turns():
    entered = threading.Event()
    release = threading.Event()
    active = 0
    maximum = 0
    guard = threading.Lock()
    class SlowWhisper:
        def transcribe(self, *args, **kwargs):
            nonlocal active, maximum
            with guard:
                active += 1
                maximum = max(maximum, active)
            entered.set()
            release.wait(5)
            with guard: active -= 1
            return iter([SimpleNamespace(text='Hallo.', no_speech_prob=0)]), None
    engine = engine_for(SlowWhisper())
    closed = threading.Event()
    second_closed = threading.Event()
    first = []
    def consume(): first.extend(engine.stream(b'\x00\x00' * 1600, closed.is_set))
    def consume_second(): first.extend(engine.stream(b'\x00\x00' * 1600, second_closed.is_set))
    with patch('engine.system_prompt', return_value='Testpersona'), patch.dict('engine.os.environ', {'MIMIR_URL': ''}), patch('engine.httpx.AsyncClient', side_effect=lambda **kw: Client(())):
        a = threading.Thread(target=consume, daemon=True)
        a.start()
        assert entered.wait(2)
        closed.set()
        a.join(4)
        assert not a.is_alive(), 'cancel stalled in Whisper'
        second = threading.Thread(target=consume_second, daemon=True)
        second.start()
        time.sleep(.15)
        assert maximum == 1
        release.set()
        second_closed.set()
        second.join(4)
        assert not second.is_alive()
    assert maximum == 1


def test_memory_action_is_chunked_within_tts_limits():
    class Memory:
        pending = None
        async def handle(self, *args, **kwargs): return 'Wort ' * 92
    engine = engine_for()
    engine.memory = Memory()
    spoken = []
    async def synth(self, text):
        spoken.append(text)
        await self.push_frame(OutputAudioRawFrame(b'\x01\x00', 16000, 1))
    kinds = run_stream(engine, synth=synth)
    assert kinds[-1] == b'E'
    assert len(spoken) > 1
    assert len(spoken) <= 16 and sum(map(len, spoken)) <= 500
    assert all(len(s) <= 80 for s in spoken)
    assert ''.join(spoken).replace(' ', '') == ('Wort' * 92)


def test_recall_whole_operation_has_deadline_and_bad_shape_is_ignored():
    class SlowClient(Client):
        async def post(self, *args, **kwargs):
            await asyncio.sleep(2)
    class BadClient(Client):
        async def post(self, *args, **kwargs):
            return SimpleNamespace(raise_for_status=lambda: None, json=lambda: {'memories': 'not a list'})
    # A slow memory endpoint must not hold the model request beyond the total recall budget.
    async def check(client_type):
        captured = []
        class CaptureClient(Client):
            def stream(self, *args, **kwargs):
                captured.extend(kwargs['json']['messages'])
                return Client([json.dumps({'message': {'content': 'Hallo.'}, 'done': True})])
        from engine import Context, Metrics
        with patch('engine.system_prompt', return_value='Testpersona'):
            context = Context()
        llm = OllamaSentences(context, Metrics(), 'http://ollama')
        llm.push_frame = lambda *args: asyncio.sleep(0)
        with patch.dict('engine.os.environ', {'MIMIR_URL': 'http://memory'}), patch('engine.Path.read_text', return_value='token'), patch('engine.httpx.AsyncClient', side_effect=[client_type(()), CaptureClient(())]):
            start = time.monotonic()
            await llm.process_frame(__import__('pipecat.frames.frames', fromlist=['LLMTextFrame']).LLMTextFrame('Hallo'), None)
            assert time.monotonic() - start < .8
        assert '[Erinnerungen' not in captured[0]['content']
    asyncio.run(check(SlowClient))
    asyncio.run(check(BadClient))


def test_tts_rejects_oversize_while_streaming():
    class Oversized(Client):
        def stream(self, *args, **kwargs): return self
        async def aiter_bytes(self):
            yield b'x' * 384045
            raise AssertionError('read past bound')
    async def check():
        voice = GermanVoice(None, __import__('engine').Metrics(), 'http://tts')
        with patch('engine.httpx.AsyncClient', side_effect=lambda **kw: Oversized(())):
            with pytest.raises(ValueError, match='TTS size'):
                await voice._synthesize('Hallo.')
    asyncio.run(check())
