"""Narrow private KI-M HTTP facade over pinned Mimir primitives."""
import hashlib
import hmac
import json
import os
import threading
from contextlib import asynccontextmanager
from pathlib import Path
from uuid import UUID

from fastapi import FastAPI, Header, HTTPException, Request
from pydantic import BaseModel, ConfigDict, Field

from vividmimir import Mimir
from vividmimir.constants import _DEDUP_THRESHOLD
from vividmimir.helpers import _content_words, _overlap_ratio
import vividmimir.write_mixin as write_mixin

CHARACTER = 'kim'
SOURCE = 'ki-m-consented'
CONSENT = 'explicit_user_instruction'
DATA = Path(os.environ.get('MIMIR_DATA_DIR', '/data'))
LOCK = threading.RLock()
ENGINE = None
TOKEN_DIGEST = None

# Disable upstream background consolidation: it creates attic, gists and graph
# copies outside the narrowly managed fact lifecycle. No LLM/embed providers.
write_mixin.AUTO_CONSOLIDATION_INTERVAL = 1_000_000_000

class Recall(BaseModel):
    model_config = ConfigDict(extra='forbid', strict=True)
    query: str = Field(min_length=1, max_length=512)
    limit: int = Field(default=3, ge=1, le=5)

class Remember(BaseModel):
    model_config = ConfigDict(extra='forbid', strict=True)
    fact: str = Field(min_length=1, max_length=500)
    source_event_id: str = Field(pattern=r'^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$')
    consent_basis: str = Field(pattern='^explicit_user_instruction$')

def event_id(mem):
    if mem.source != SOURCE or not mem.why_saved.startswith('event:'):
        raise RuntimeError('Unexpected foreign memory in KI-M volume')
    value = mem.why_saved.removeprefix('event:')
    parsed = UUID(value)
    if str(parsed) != value or parsed.version != 4:
        raise RuntimeError('Invalid memory provenance')
    return value

def present(mem):
    event = event_id(mem)
    return {'id': event, 'content': mem.content, 'source': SOURCE,
            'source_event_id': event, 'recorded_at': mem.timestamp,
            'consent_basis': CONSENT}

def load_engine():
    global ENGINE
    DATA.mkdir(mode=0o700, parents=True, exist_ok=True)
    if DATA.is_symlink() or not os.access(DATA, os.W_OK):
        raise RuntimeError('Memory volume not writable')
    # Upstream silently converts malformed JSON to empty data; reject instead.
    path = DATA / 'reflections.json'
    if path.exists():
        rows = json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(rows, list):
            raise RuntimeError('Invalid memory file')
    allowed = {'reflections.json', 'social', 'lessons.json', 'reminders.json',
               'facts.json', 'meta.json', 'tasks.json', 'actions.json',
               'solutions.json', 'artifacts.json'}
    if any(p.name not in allowed for p in DATA.iterdir()):
        raise RuntimeError('Unexpected files in KI-M volume')
    ENGINE = Mimir(data_dir=DATA, chemistry=False, visual=False, llm_fn=None)
    if ENGINE._embed is not None:
        raise RuntimeError('Unexpected embedding engine enabled')
    for mem in ENGINE._reflections:
        event_id(mem)
        if mem._snapshot_id or mem._visual_hash:
            raise RuntimeError('Unexpected memory attachment')
    if (ENGINE._attic or ENGINE._social or ENGINE._facts or ENGINE._lessons
            or ENGINE._project_tasks or ENGINE._project_actions
            or ENGINE._solutions or ENGINE._artifacts):
        raise RuntimeError('Unexpected auxiliary memory in KI-M volume')
    if any(r.content not in {m.content for m in ENGINE._reflections}
           for r in ENGINE._reminders):
        raise RuntimeError('Orphan reminder in KI-M volume')
    if len({event_id(m) for m in ENGINE._reflections}) != len(ENGINE._reflections):
        raise RuntimeError('Duplicate event IDs')
    return ENGINE

@asynccontextmanager
async def lifespan(app):
    global TOKEN_DIGEST
    secret = Path(os.environ.get('MIMIR_TOKEN_FILE', '/run/secrets/kim_mimir_token'))
    if secret.is_symlink():
        raise RuntimeError('Token file must not be a symlink')
    raw = secret.read_bytes().strip()
    if len(raw) < 32 or len(raw) > 256 or any(c < 33 or c > 126 for c in raw):
        raise RuntimeError('Missing or weak token')
    TOKEN_DIGEST = hashlib.sha256(b'ki-m-memory-auth-v1:' + raw).digest()
    load_engine()
    yield

app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None, lifespan=lifespan)

@app.middleware('http')
async def gate(request: Request, call_next):
    from fastapi.responses import JSONResponse
    header = request.headers.get('authorization', '')
    candidate = header[7:] if header.startswith('Bearer ') else ''
    digest = hashlib.sha256(b'ki-m-memory-auth-v1:' + candidate.encode('utf-8', 'replace')).digest()
    if TOKEN_DIGEST is None or not hmac.compare_digest(digest, TOKEN_DIGEST):
        return JSONResponse({'detail': 'Unauthorized'}, status_code=401)
    try:
        declared = int(request.headers.get('content-length', '0'))
    except ValueError:
        return JSONResponse({'detail': 'Invalid length'}, status_code=400)
    if declared < 0 or declared > 4096:
        return JSONResponse({'detail': 'Request too large'}, status_code=413)
    if request.method in ('POST', 'PUT', 'PATCH'):
        body = bytearray()
        async for chunk in request.stream():
            body.extend(chunk)
            if len(body) > 4096:
                return JSONResponse({'detail': 'Request too large'}, status_code=413)
        request._body = bytes(body)
    return await call_next(request)

@app.get('/health')
def health():
    return {'status': 'ok', 'character': CHARACTER, 'engine': 'mimir'}

@app.post('/recall')
def recall(body: Recall):
    with LOCK:
        try:
            result = ENGINE.recall(body.query, limit=body.limit)
            # Never expose foreign source, even if a malformed volume bypasses startup.
            return {'memories': [present(m) for m in result if m.source == SOURCE][:body.limit]}
        except Exception as exc:
            raise HTTPException(503, 'Memory unavailable') from exc

@app.post('/remember')
def remember(body: Remember):
    from fastapi.responses import JSONResponse
    if body.fact != body.fact.strip() or not body.fact.strip():
        raise HTTPException(422, 'Invalid fact or event ID')
    key = str(body.source_event_id)
    with LOCK:
        try:
            for mem in ENGINE._reflections:
                if event_id(mem) == key:
                    if mem.content != body.fact:
                        raise HTTPException(409, 'Event already recorded')
                    return JSONResponse({'memory': present(mem)}, status_code=200)
                if _overlap_ratio(_content_words(body.fact), mem.content_words) >= _DEDUP_THRESHOLD:
                    raise HTTPException(409, 'Similar fact exists')
            mem = ENGINE.remember(content=body.fact, emotion='neutral', importance=5,
                                  source=SOURCE, why_saved='event:' + key)
            ENGINE.save()
            return JSONResponse({'memory': present(mem)}, status_code=201)
        except HTTPException:
            raise
        except Exception as exc:
            raise HTTPException(503, 'Memory unavailable') from exc

@app.get('/memory/{memory_id}')
def read_memory(memory_id: UUID):
    """Authenticated exact-ID readback, reloaded from the existing private store."""
    with LOCK:
        try:
            engine = load_engine()
            targets = [m for m in engine._reflections if event_id(m) == str(memory_id)]
            if len(targets) != 1:
                raise HTTPException(404, {'code': 'memory_not_found', 'id': str(memory_id)})
            return {'memory': present(targets[0])}
        except HTTPException:
            raise
        except Exception as exc:
            raise HTTPException(503, 'Memory unavailable') from exc


@app.delete('/forget/{memory_id}')
def forget(memory_id: UUID):
    with LOCK:
        try:
            key = str(memory_id)
            targets = [m for m in ENGINE._reflections if event_id(m) == key]
            if len(targets) != 1:
                raise HTTPException(404, 'Not found')
            target = targets[0]
            ENGINE._reflections.remove(target)
            ENGINE._attic = [m for m in ENGINE._attic if event_id(m) != key]
            ENGINE._reminders = [r for r in ENGINE._reminders if r.content != target.content]
            ENGINE._inferred_edges.clear()
            ENGINE._yggdrasil.clear()
            ENGINE._priming_buffer.clear()
            if ENGINE._embed is not None and target._embed_uid:
                ENGINE._embed.remove(target._embed_uid)
            ENGINE._rebuild_index()
            ENGINE.save()
            # Verify disk and fresh engine read; fail closed rather than claim deletion.
            load_engine()
            if any(event_id(m) == key for m in ENGINE._reflections + ENGINE._attic):
                raise RuntimeError('Deletion not durable')
            return {'deleted': True}
        except HTTPException:
            raise
        except Exception as exc:
            raise HTTPException(503, 'Memory unavailable') from exc
