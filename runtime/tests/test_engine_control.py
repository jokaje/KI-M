"""No network or production writes: consent and bounded worker tests."""
import asyncio
import queue
import threading
import unittest
from unittest.mock import patch

from memory_control import MemoryControl
from engine import AudioSink, GermanVoice, Metrics
from pipecat.frames.frames import EndFrame, LLMTextFrame, StartFrame, OutputAudioRawFrame

ID = '33333333-3333-4333-8333-333333333333'


class Reply:
    def __init__(self, data): self.data = data
    def raise_for_status(self): pass
    def json(self): return self.data


class Client:
    def __init__(self): self.calls = []; self.fact = None; self.deleted = False; self.bad_readback = False
    async def post(self, url, json, headers):
        self.calls.append((url, json))
        if url.endswith('/remember'):
            self.fact = json['fact']
            return Reply({'memory': {'id': ID, 'content': self.fact, 'source': 'ki-m-consented',
                                    'source_event_id': json['source_event_id'],
                                    'consent_basis': json['consent_basis']}})
        return Reply({'memories': [] if self.bad_readback else [{'id': ID, 'content': self.fact,
            'source': 'ki-m-consented', 'source_event_id': self.calls[0][1]['source_event_id'],
            'consent_basis': 'explicit_user_instruction'}]})
    async def get(self, url, headers):
        self.calls.append((url, None))
        if self.deleted:
            response = Reply({'detail': {'code': 'memory_not_found', 'id': ID}})
            response.status_code = 404
            return response
        return Reply({'memory': {} if self.bad_readback else {'id': ID, 'content': self.fact,
            'source': 'ki-m-consented', 'source_event_id': self.calls[0][1]['source_event_id'],
            'consent_basis': 'explicit_user_instruction'}})
    async def delete(self, url, headers):
        self.calls.append((url, None)); self.deleted = True
        return Reply({'deleted': True})


class ConsentTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.control = MemoryControl(); self.client = Client()
    async def say(self, text, selected=()):
        return await self.control.handle(text, selected, self.client, 'http://memory', 'test-token')
    async def test_ordinary_and_ambiguous_yes_never_write(self):
        self.assertIsNone(await self.say('Mir gefällt Blau.'))
        self.assertIsNone(await self.say('Ja'))
        self.assertEqual(self.client.calls, [])
        challenge = await self.say('Merke dir: Das Testwort ist Blau.')
        self.assertIn('Ja, speichere genau: Das Testwort ist Blau.', challenge)
        await self.say('Ja')
        self.assertEqual(self.client.calls, [])
        self.assertIn('gespeichert', await self.say('Ja, speichere genau: Das Testwort ist Blau.'))
        self.assertEqual(self.client.calls[0][1]['fact'], 'Das Testwort ist Blau.')
        self.assertEqual(self.client.calls[0][1]['consent_basis'], 'explicit_user_instruction')
        self.assertEqual(len(self.client.calls), 2)
    async def test_cancel_expiry_mismatch_and_readback(self):
        await self.say('Merke dir: Testfakt.')
        await self.say('Ja, speichere genau: anderer Fakt.')
        self.assertEqual(self.client.calls, [])
        await self.say('Merke dir: Testfakt.')
        await self.say('Abbrechen')
        self.assertEqual(self.client.calls, [])
        await self.say('Merke dir: Testfakt.')
        self.control.pending.expires = 0
        await self.say('Ja, speichere genau: Testfakt.')
        self.assertEqual(self.client.calls, [])
        await self.say('Merke dir: Testfakt.')
        self.client.bad_readback = True
        self.assertIn('nicht bestätigen', await self.say('Ja, speichere genau: Testfakt.'))
        self.assertIsNone(self.control.pending)
    async def test_cancelled_confirmation_never_writes(self):
        await self.say('Merke dir: Testfakt.')
        result = await self.control.handle('Ja, speichere genau: Testfakt.', [], self.client,
                                           'http://memory', 'test-token', cancelled=lambda: True)
        self.assertIsNone(result)
        self.assertIsNone(self.control.pending)
        self.assertEqual(self.client.calls, [])

    async def test_forget_only_selected_exact_uuid_and_confirmation(self):
        selected = [{'id': ID, 'content': 'Ein Testfakt.'}]
        await self.say('Vergiss genau: Ein Testfakt.', [{'id': 'bad', 'content': 'Ein Testfakt.'}])
        self.assertIsNone(self.control.pending)
        await self.say('Vergiss genau: Ein Testfakt.', selected)
        await self.say('Ja')
        self.assertFalse(self.client.deleted)
        self.assertIn('vergessen', await self.say('Ja, vergiss genau: Ein Testfakt.'))
        self.assertEqual(self.client.calls[-2][0], 'http://memory/forget/' + ID)
        self.assertEqual(self.client.calls[-1][0], 'http://memory/memory/' + ID)
        self.assertTrue(self.client.deleted)


class VoiceTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        async def base_noop(*args): pass
        patcher = patch('engine.FrameProcessor.process_frame', new=base_noop)
        patcher.start()
        self.addCleanup(patcher.stop)

    async def test_bounded_queue_order_and_drain(self):
        voice = GermanVoice(None, Metrics(), 'http://tts')
        spoken = []; emitted = []
        async def synth(text):
            spoken.append(text)
            await asyncio.sleep(0.01)
            await voice.push_frame(OutputAudioRawFrame(b'\x01\x00', 16000, 1))
        async def push(frame, direction=None): emitted.append(frame)
        voice._synthesize = synth
        voice.push_frame = push
        await voice.process_frame(StartFrame(), None)
        for text in ['eins.', 'zwei.', 'drei.', 'vier.']:
            await voice.process_frame(LLMTextFrame(text), None)
        self.assertLessEqual(voice.sentences.maxsize, 2)
        await voice.process_frame(EndFrame(), None)
        self.assertEqual(spoken, ['eins.', 'zwei.', 'drei.', 'vier.'])
        self.assertIsInstance(emitted[-1], EndFrame)
        self.assertEqual(sum(isinstance(x, OutputAudioRawFrame) for x in emitted), 4)
    async def test_full_output_does_not_block_on_disconnect(self):
        output = queue.Queue(maxsize=1)
        output.put_nowait((b'A', b'old'))
        stopped = threading.Event()
        sink = AudioSink(output, stopped)
        publisher = asyncio.create_task(sink.process_frame(OutputAudioRawFrame(b'\x01\x00', 16000, 1), None))
        await asyncio.sleep(0.08)
        stopped.set()
        with self.assertRaises(asyncio.CancelledError):
            await asyncio.wait_for(publisher, 1)
        self.assertEqual(output.qsize(), 1)

    async def test_tts_error_propagates_and_worker_cleans_up(self):
        voice = GermanVoice(None, Metrics(), 'http://tts')
        async def fail(text): raise ValueError('tts broken')
        voice._synthesize = fail
        voice.push_frame = lambda *args: asyncio.sleep(0)
        await voice.process_frame(StartFrame(), None)
        await voice.process_frame(LLMTextFrame('Test.'), None)
        await voice.process_frame(EndFrame(), None)
        self.assertIsInstance(voice.failure, ValueError)
        self.assertEqual(str(voice.failure), 'tts broken')
        await voice.cleanup()
        self.assertTrue(voice.worker.done())


if __name__ == '__main__': unittest.main()
