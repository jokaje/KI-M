"""After restarting container, verify Mimir persisted the synthetic fact; delete it."""
import os
import json
import urllib.request
import urllib.error
from uuid import UUID

BASE = 'http://127.0.0.1:8080'
TOKEN = open(os.environ['MEMORY_TEST_TOKEN_FILE'], encoding='ascii').read().strip()

def call(method, path, data=None):
    body = json.dumps(data).encode() if data is not None else None
    request = urllib.request.Request(BASE + path, data=body, method=method,
        headers={'Authorization': 'Bearer ' + TOKEN, 'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as exc:
        return exc.code, json.load(exc)

if __name__ == '__main__':
    event = os.environ['MEMORY_PERSIST_EVENT']
    UUID(event)
    status, result = call('POST', '/recall', {'query': 'synthetic cobalt lantern', 'limit': 5})
    assert status == 200 and any(m['id'] == event for m in result['memories']), (status, result)
    assert call('DELETE', '/forget/' + event) == (200, {'deleted': True})
    assert call('DELETE', '/forget/' + event)[0] == 404
    status, result = call('POST', '/recall', {'query': 'synthetic cobalt lantern', 'limit': 5})
    assert status == 200 and all(m['id'] != event for m in result['memories']), (status, result)
    print('Mimir durable recall after restart, targeted deletion: PASS')
