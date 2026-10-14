"""Transport owns engine._lock; native STT must use a distinct model lock."""
import json
from test_engine_failures import engine_for, run_stream


def test_transport_turn_lock_does_not_block_whisper():
    engine = engine_for()
    lines = [json.dumps({'message': {'content': 'Hallo. '}}), json.dumps({'done': True})]
    with engine._lock:
        assert run_stream(engine, lines=lines) == [b'A', b'E']
