"""Read-only exact-ID endpoint tests: no real memory volume or writes."""
import importlib.util
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch
from uuid import UUID

spec = importlib.util.spec_from_file_location('memory_app', '/bench/memory_service/app.py')
app = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = app
spec.loader.exec_module(app)
ID = '33333333-3333-4333-8333-333333333333'

class ExactReadTests(unittest.TestCase):
    def test_present_record_is_read_by_exact_uuid(self):
        mem = SimpleNamespace(source=app.SOURCE, why_saved='event:' + ID,
                              content='Ein isolierter Testfakt.', timestamp='test')
        fake = SimpleNamespace(_reflections=[mem])
        with patch.object(app, 'load_engine', return_value=fake), patch.object(app, 'ENGINE', fake):
            result = app.read_memory(UUID(ID))
        self.assertEqual(result['memory']['id'], ID)
        self.assertEqual(result['memory']['content'], mem.content)

    def test_exact_missing_record_returns_404(self):
        fake = SimpleNamespace(_reflections=[])
        with patch.object(app, 'load_engine', return_value=fake), patch.object(app, 'ENGINE', fake):
            with self.assertRaises(app.HTTPException) as raised:
                app.read_memory(UUID(ID))
        self.assertEqual(raised.exception.status_code, 404)

    def test_storage_failure_returns_503_not_absence(self):
        with patch.object(app, 'load_engine', side_effect=ValueError('isolated failure')):
            with self.assertRaises(app.HTTPException) as raised:
                app.read_memory(UUID(ID))
        self.assertEqual(raised.exception.status_code, 503)

if __name__ == '__main__':
    unittest.main()
