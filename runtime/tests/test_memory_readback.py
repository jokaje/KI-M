"""In-memory exact-ID readback and in-flight cancellation regressions."""
import asyncio
import httpx
from memory_control import MemoryControl

ID = '33333333-3333-4333-8333-333333333333'
FACT = 'Ein Testfakt.'

class Client:
    def __init__(self, retained=False, cancelled=None):
        self.calls = []
        self.retained = retained
        self.cancelled = cancelled
    async def delete(self, url, **kwargs):
        self.calls.append(('DELETE', url))
        if self.cancelled is not None:
            self.cancelled[0] = True
        return httpx.Response(200, json={'deleted': True}, request=httpx.Request('DELETE', url))
    async def get(self, url, **kwargs):
        self.calls.append(('GET', url))
        body = {'memory': {'id': ID, 'content': FACT}} if self.retained else {'detail': {'code': 'memory_not_found', 'id': ID}}
        return httpx.Response(200 if self.retained else 404, json=body, request=httpx.Request('GET', url))


def test_retained_exact_id_cannot_be_confirmed_deleted():
    async def run():
        c = MemoryControl()
        client = Client(retained=True)
        await c.handle('Vergiss genau: ' + FACT, [{'id': ID, 'content': FACT}], client, 'http://memory', 'test')
        answer = await c.handle('Ja, vergiss genau: ' + FACT, [], client, 'http://memory', 'test')
        assert 'nicht bestätigen' in answer
        assert client.calls == [('DELETE', 'http://memory/forget/' + ID), ('GET', 'http://memory/memory/' + ID)]
    asyncio.run(run())


def test_missing_readback_route_cannot_confirm_deletion():
    class MissingRoute(Client):
        async def get(self, url, **kwargs):
            return httpx.Response(404, json={'detail': 'Not Found'}, request=httpx.Request('GET', url))
    async def run():
        c = MemoryControl()
        client = MissingRoute()
        await c.handle('Vergiss genau: ' + FACT, [{'id': ID, 'content': FACT}], client, 'http://memory', 'test')
        answer = await c.handle('Ja, vergiss genau: ' + FACT, [], client, 'http://memory', 'test')
        assert 'nicht bestätigen' in answer
    asyncio.run(run())


def test_disconnect_during_commit_returns_uncertain_not_success():
    async def run():
        c = MemoryControl()
        cancelled = [False]
        client = Client(cancelled=cancelled)
        await c.handle('Vergiss genau: ' + FACT, [{'id': ID, 'content': FACT}], client, 'http://memory', 'test')
        answer = await c.handle('Ja, vergiss genau: ' + FACT, [], client, 'http://memory', 'test', cancelled=lambda: cancelled[0])
        assert 'nicht bestätigen' in answer
        assert c.pending is None
    asyncio.run(run())
