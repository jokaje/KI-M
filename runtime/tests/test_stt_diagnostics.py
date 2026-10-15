import asyncio, logging, unittest
from unittest.mock import patch
import engine

class Diagnostics(unittest.TestCase):
    def test_pcm_summary_is_numeric_and_correct(self):
        self.assertEqual(engine.pcm_summary(b'\0\0'*1600),
                         {'samples':1600,'rms':0,'peak':0,'nonzero':0,'clipped':0})
        self.assertEqual(engine.pcm_summary(b'\xff\x7f\x00\x80'),
                         {'samples':2,'rms':32767,'peak':32768,'nonzero':2,'clipped':2})

    def test_error_details_are_content_free(self):
        p=engine.FailClosedProcessor()
        try:
            raise ValueError('PRIVATE_TRANSCRIPT_TOKEN_URL')
        except ValueError as e:
            with self.assertLogs('kim.voice',level='WARNING') as logs:
                with patch.object(engine.FrameProcessor,'push_error',return_value=None):
                    asyncio.run(p.push_error('PRIVATE_TRANSCRIPT_TOKEN_URL',e))
        text=' '.join(logs.output)
        self.assertIn('error_type=ValueError',text)
        self.assertNotIn('PRIVATE_TRANSCRIPT_TOKEN_URL',text)
        self.assertIn('processor=FailClosedProcessor',text)

    def test_tts_worker_failure_is_also_content_free(self):
        async def scenario():
            p=engine.GermanVoice(None,engine.Metrics(),'http://unused')
            p._input_summary=engine.pcm_summary(b'\0\0'*1600)
            with patch.object(p,'_synthesize',side_effect=ValueError('PRIVATE_SYNTHESIS_TEXT')):
                with self.assertLogs('kim.voice',level='WARNING') as logs:
                    task=asyncio.create_task(p._work())
                    try:
                        await p.enqueue('PRIVATE_SENTENCE')
                        await asyncio.wait_for(p.sentences.join(),2)
                    finally:
                        task.cancel()
                        await asyncio.gather(task,return_exceptions=True)
            result=' '.join(logs.output)
            self.assertIn('processor=GermanVoice',result)
            self.assertIn('error_type=ValueError',result)
            self.assertIn("'samples': 1600",result)
            self.assertNotIn('PRIVATE_',result)
        asyncio.run(scenario())

    def test_empty_recognition_is_distinguishable(self):
        p=engine.FailClosedProcessor()
        with self.assertLogs('kim.voice',level='WARNING') as logs:
            with patch.object(engine.FrameProcessor,'push_error',return_value=None):
                asyncio.run(p.push_error('ignored',ValueError('no speech recognized')))
        self.assertIn('reason=no_speech',' '.join(logs.output))

if __name__=='__main__':unittest.main()
