"""Engine regressions; mocks never send production memory writes."""
import asyncio
import json
import unittest
from unittest.mock import patch

from pipecat.frames.frames import LLMTextFrame
from engine import Context, Metrics, OllamaSentences


class Response:
    def __init__(self, body=None): self.body = body or {}
    def raise_for_status(self): pass
    def json(self): return self.body
    async def aiter_bytes(self):
        async for line in self.aiter_lines():
            yield (line + "\n").encode("utf-8")
    async def aiter_lines(self):
        yield json.dumps({'message': {'content': 'Hallo.'}, 'done': False})
        yield json.dumps({'done': True})
    async def __aenter__(self): return self
    async def __aexit__(self, *args): pass


class Client:
    def __init__(self, messages): self.messages = messages
    async def __aenter__(self): return self
    async def __aexit__(self, *args): pass
    async def post(self, *args, **kwargs):
        return Response({'memories': [{'id': '33333333-3333-4333-8333-333333333333', 'content': 'Ein harmloser Testfakt.'}]})
    def stream(self, *args, **kwargs):
        self.messages.extend(kwargs['json']['messages'])
        return Response()


class EngineTests(unittest.IsolatedAsyncioTestCase):
    async def test_recall_is_in_same_ollama_request(self):
        messages = []
        with patch('engine.system_prompt', return_value='Testpersona'):
            context = Context()
        context.push_frame = lambda *args: asyncio.sleep(0)
        llm = OllamaSentences(context, Metrics(), 'http://ollama')
        llm.push_frame = lambda *args: asyncio.sleep(0)
        with patch('engine.system_prompt', return_value='Testpersona'), patch('engine.httpx.AsyncClient', side_effect=lambda **kwargs: Client(messages)), patch.dict('engine.os.environ', {'MIMIR_URL': 'http://memory', 'MIMIR_TOKEN_FILE': '/dummy'}), patch('engine.Path.read_text', return_value='test-token'):
            # The persona is read in Context.__init__, so no private content is logged.
            await context.process_frame(LLMTextFrame('Hallo'), None)
            await llm.process_frame(LLMTextFrame('Hallo'), None)
        self.assertIn('Ein harmloser Testfakt.', messages[0]['content'])
        self.assertEqual(messages[1]['content'], 'Hallo')


if __name__ == '__main__': unittest.main()
