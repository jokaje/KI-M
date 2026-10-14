"""Two-turn, scoped consent gate for KI-M's private memory adapter.

Only user STT text enters this controller. No model text or ordinary transcript is saved.
The Engine owns one controller for the serialized, single-device server.
"""
import re
import time
import uuid
from dataclasses import dataclass

import httpx


@dataclass
class Pending:
    action: str
    fact: str
    expires: float
    memory_id: str = ''
    event_id: str = ''


class MemoryControl:
    def __init__(self, lifetime=60):
        self.pending = None
        self.lifetime = lifetime

    async def handle(self, text, selected, client, base_url, token, cancelled=lambda: False):
        """Return a deterministic spoken sentence or None for ordinary conversation.

        The caller must invoke this once for a completed, non-cancelled STT turn,
        on the serialized engine. `selected` is this turn's bounded recall list.
        """
        now = time.monotonic()
        if cancelled():
            self.pending = None
            return None
        if self.pending and self.pending.expires <= now:
            self.pending = None
        text = text.strip()
        pending = self.pending
        if pending:
            prefix = 'Ja, speichere genau: ' if pending.action == 'remember' else 'Ja, vergiss genau: '
            if text == prefix + pending.fact:
                self.pending = None  # never retry automatically after an uncertain write
                if not base_url or not token or cancelled():
                    return 'Der Erinnerungsspeicher ist nicht verfügbar.'
                try:
                    if pending.action == 'remember':
                        response = await client.post(base_url + '/remember', json={
                            'fact': pending.fact, 'source_event_id': pending.event_id,
                            'consent_basis': 'explicit_user_instruction',
                        }, headers={'Authorization': 'Bearer ' + token})
                        response.raise_for_status()
                        recorded = response.json()['memory']
                        if (recorded.get('content') != pending.fact or
                                recorded.get('source') != 'ki-m-consented' or
                                recorded.get('source_event_id') != pending.event_id or
                                recorded.get('consent_basis') != 'explicit_user_instruction' or
                                not _uuid(recorded.get('id', ''))):
                            raise ValueError('memory write mismatch')
                        # Exact-ID readback avoids lexical recall ambiguity.
                        check = await client.get(base_url + '/memory/' + recorded['id'],
                                                 headers={'Authorization': 'Bearer ' + token})
                        check.raise_for_status()
                        stored = check.json().get('memory', {})
                        if (stored.get('id') != recorded['id'] or stored.get('content') != pending.fact
                                or stored.get('source_event_id') != pending.event_id
                                or stored.get('consent_basis') != 'explicit_user_instruction'
                                or stored.get('source') != 'ki-m-consented'):
                            raise ValueError('memory readback mismatch')
                        if cancelled():
                            return 'Die Änderung konnte ich nicht bestätigen.'
                        return 'Ich habe die bestätigte Erinnerung gespeichert.'
                    response = await client.delete(base_url + '/forget/' + pending.memory_id,
                                                   headers={'Authorization': 'Bearer ' + token})
                    response.raise_for_status()
                    if response.json().get('deleted') is not True:
                        raise ValueError('memory deletion unverified')
                    check = await client.get(base_url + '/memory/' + pending.memory_id,
                                             headers={'Authorization': 'Bearer ' + token})
                    if (check.status_code != 404 or check.json().get('detail') !=
                            {'code': 'memory_not_found', 'id': pending.memory_id}):
                        raise ValueError('memory deletion readback mismatch')
                    if cancelled():
                        return 'Die Änderung konnte ich nicht bestätigen.'
                    return 'Ich habe diese bestätigte Erinnerung vergessen.'
                except (httpx.HTTPError, KeyError, ValueError, TypeError):
                    return 'Die Änderung konnte ich nicht bestätigen.'
            # A plain yes, vague reference, or unrecognized command NEVER writes.
            if text.lower() in ('nein', 'nein.', 'abbrechen', 'abbrechen.'):
                self.pending = None
                return 'In Ordnung, keine Änderung.'
            if text.lower() in ('ja', 'ja.', 'okay', 'ok', 'bitte'):
                return 'Bitte bestätige mit dem genauen vorgeschlagenen Satz oder sage Abbrechen.'
            self.pending = None  # unrelated next turn invalidates authorization
        match = re.fullmatch(r'Merke dir: (.{1,120})', text, re.I | re.S)
        if match:
            fact = match.group(1).strip()
            if not fact or '\n' in fact or '\r' in fact:
                return 'Bitte nenne einen einzelnen kurzen Fakt.'
            self.pending = Pending('remember', fact, now + self.lifetime, event_id=str(uuid.uuid4()))
            return 'Vorgeschlagener Fakt: „' + fact + '“. Bestätige mit: Ja, speichere genau: ' + fact
        match = re.fullmatch(r'Vergiss genau: (.{1,120})', text, re.I | re.S)
        if match:
            fact = match.group(1).strip()
            exact = [m for m in selected if m.get('content') == fact and _uuid(m.get('id', ''))]
            if len(exact) != 1:
                return 'Ich konnte keine eindeutig ausgewählte Erinnerung mit genau diesem Inhalt finden.'
            self.pending = Pending('forget', fact, now + self.lifetime, memory_id=exact[0]['id'])
            return 'Soll ich genau diese Erinnerung vergessen: ' + fact + '? Bestätige mit: Ja, vergiss genau: ' + fact
        return None


def _uuid(value):
    try:
        return str(uuid.UUID(value)) == value
    except (ValueError, TypeError, AttributeError):
        return False
