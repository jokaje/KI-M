"""Real Mimir synthetic lifecycle in isolated tmpfs, no production volume."""
import app
import uuid

key = str(uuid.uuid4())
app.load_engine()
body = app.Remember(fact='Isolierter Readback Testfakt Nummer sieben', source_event_id=key, consent_basis=app.CONSENT)
app.remember(body)
app.load_engine()
assert app.read_memory(uuid.UUID(key))['memory']['content'] == body.fact
assert app.forget(uuid.UUID(key))['deleted']
app.load_engine()
try:
    app.read_memory(uuid.UUID(key))
except app.HTTPException as error:
    assert error.status_code == 404
else:
    raise AssertionError('deleted ID still present')
print('REAL_ISOLATED_MIMIR remember -> disk reload -> exact read -> forget -> disk reload -> 404: PASS')
