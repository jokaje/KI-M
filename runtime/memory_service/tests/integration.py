"""Exercise real HTTP service and persisted Mimir storage; synthetic facts only."""
import json
import os
import urllib.error
import urllib.request
import uuid

BASE = os.environ.get('MEMORY_TEST_URL', 'http://127.0.0.1:8080')
TOKEN = open(os.environ['MEMORY_TEST_TOKEN_FILE'], encoding='ascii').read().strip()

def call(method, path, data=None, token=TOKEN):
    body = json.dumps(data).encode() if data is not None else None
    req = urllib.request.Request(BASE + path, data=body, method=method,
        headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as exc:
        return exc.code, json.load(exc)

if __name__ == '__main__':
    event = str(uuid.uuid4())
    fact = 'Synthetic cobalt lantern belongs to KI-M test ' + event
    request = {'fact': fact, 'source_event_id': event, 'consent_basis': 'explicit_user_instruction'}
    assert call('GET', '/health', token='incorrect')[0] == 401
    assert call('GET', '/health', token='')[0] == 401
    assert call('POST', '/remember', {**request, 'char_id': 'sammy'})[0] == 422
    assert call('POST', '/remember', {**request, 'consent_basis': 'implicit'})[0] == 422
    assert call('POST', '/remember', {**request, 'source_event_id': 'not-a-uuid'})[0] == 422
    assert call('POST', '/recall', {'query': 'q' * 4200})[0] == 413
    assert call('GET', '/api/memory/export')[0] == 404
    status, response = call('POST', '/remember', request)
    assert status == 201, (status, response)
    memory = response['memory']
    assert memory['content'] == fact and memory['id'] == event, response
    assert call('POST', '/remember', request)[0] == 200
    assert call('POST', '/remember', {**request, 'fact': 'different'})[0] == 409
    status, response = call('POST', '/recall', {'query': 'synthetic cobalt lantern', 'limit': 5})
    assert status == 200 and any(m['id'] == event and m['content'] == fact for m in response['memories']), response
    assert call('DELETE', '/forget/' + str(uuid.uuid4()))[0] == 404
    assert call('DELETE', '/forget/' + event, token='wrong')[0] == 401
    if os.environ.get('MEMORY_TEST_PHASE') == 'before_restart':
        print(json.dumps({'event': event, 'fact': fact}))
    else:
        assert call('DELETE', '/forget/' + event)[0] == 200
        assert call('DELETE', '/forget/' + event)[0] == 404
        status, response = call('POST', '/recall', {'query': 'synthetic cobalt lantern', 'limit': 5})
        assert status == 200 and all(m['id'] != event for m in response['memories']), response
        print('HTTP auth, scope, write, idempotency, recall, forget: PASS')
