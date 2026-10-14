"""Bound NDJSON bytes before newline assembly and parsing."""
import asyncio
import pytest
import engine

class Response:
    def __init__(self, chunks): self.chunks = chunks; self.read = 0
    async def aiter_bytes(self, chunk_size=4096):
        for chunk in self.chunks:
            self.read += 1
            yield chunk


def test_overlong_unterminated_line_is_rejected_during_read():
    async def run():
        response = Response([b'x' * 4096] * 100)
        with pytest.raises(ValueError, match='model line bound'):
            async for _ in engine.bounded_model_lines(response):
                pass
        assert response.read == 5
    asyncio.run(run())


def test_utf8_split_across_chunks_and_final_line():
    async def run():
        response = Response([b'{"text":"Gr\xc3', b'\xbc\xc3\x9fe"}\r\n', b'{"done":true}'])
        rows = [line async for line in engine.bounded_model_lines(response)]
        assert rows == ['{"text":"Grüße"}', '{"done":true}']
    asyncio.run(run())


def test_total_stream_bytes_are_bounded_even_with_short_lines():
    async def run():
        response = Response([b'{}\n' * 1000] * 100)
        with pytest.raises(ValueError, match='model stream bound'):
            async for _ in engine.bounded_model_lines(response):
                pass
        assert response.read < 100
    asyncio.run(run())
