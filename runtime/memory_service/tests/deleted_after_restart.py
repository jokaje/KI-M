"""Check deletion survives a second engine reload after container restart."""
import json
import os
import urllib.request

key = os.environ['MEMORY_PERSIST_EVENT']
token = open('/run/secrets/kim_mimir_token').read().strip()
request = urllib.request.Request('http://127.0.0.1:8080/recall',
    data=b'{"query":"synthetic cobalt lantern","limit":5}',
    headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'}, method='POST')
with urllib.request.urlopen(request, timeout=10) as response:
    assert response.status == 200
    assert all(m['id'] != key for m in json.load(response)['memories'])
print('Deleted memory absent after second restart: PASS')
