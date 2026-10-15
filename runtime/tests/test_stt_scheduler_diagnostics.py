"""Content-free diagnostics exercised through the actual Pipecat scheduler."""
from types import SimpleNamespace
from unittest.mock import patch
from test_engine_failures import engine_for,run_stream


def test_empty_stt_logs_numeric_input_and_original_error(caplog):
    class EmptyWhisper:
        def transcribe(self,*args,**kwargs):return iter([]),None
    with caplog.at_level('WARNING',logger='kim.voice'):
        assert run_stream(engine_for(EmptyWhisper()))==[b'X']
    assert 'processor=SpeechToText error_type=ValueError' in caplog.text
    assert 'reason=no_speech' in caplog.text
    assert "'samples': 1600" in caplog.text
    assert "'rms': 0" in caplog.text


def test_failed_tts_logs_input_summary_and_private_error_type(caplog):
    import json
    async def broken(self,text):raise LookupError('PRIVATE_TRANSCRIPT_OR_SECRET')
    lines=[json.dumps({'message':{'content':'PRIVATE_SENTENCE. '}}),json.dumps({'done':True})]
    with caplog.at_level('WARNING',logger='kim.voice'):
        assert run_stream(engine_for(),lines=lines,synth=broken)==[b'X']
    assert 'processor=GermanVoice error_type=LookupError' in caplog.text
    assert "'samples': 1600" in caplog.text
    assert 'PRIVATE_' not in caplog.text
