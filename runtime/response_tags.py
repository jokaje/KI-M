"""Bounded incremental model-prefix parser; control markup never reaches speech."""

class ResponseTags:
    ALLOWED = frozenset(('idle', 'neutral', 'happy'))

    def __init__(self):
        self.pending = ''
        self.stage = 'prefix'
        self.emotion = 'idle'
        self.think_bytes = 0

    def feed(self, text):
        if not isinstance(text, str):
            raise ValueError('invalid model delta')
        if self.stage == 'text':
            return self._text(text)
        self.pending += text
        while True:
            if self.stage == 'think':
                closing = self.pending.lower().find('</think>')
                if closing < 0:
                    self.think_bytes += len(self.pending)
                    if self.think_bytes > 2048:
                        raise ValueError('thinking bound')
                    # Retain only a possible closing-tag suffix.
                    self.pending = self.pending[-7:]
                    return ''
                self.pending = self.pending[closing + 8:]
                self.stage = 'prefix'
            stripped = self.pending.lstrip()
            if len(self.pending) - len(stripped) > 16:
                raise ValueError('prefix whitespace bound')
            self.pending = stripped
            lower = stripped.lower()
            if not stripped:
                return ''
            if self.stage == 'prefix' and '<think>'.startswith(lower):
                if len(stripped) < 7:
                    return ''
                self.pending = stripped[7:]
                self.stage = 'think'
                continue
            if self.stage == 'prefix' and lower.startswith('<think>'):
                self.pending = stripped[7:]
                self.stage = 'think'
                continue
            if self.stage == 'prefix' and (lower.startswith('[state:') or lower.startswith('[emotion:') or
                                           '[state:'.startswith(lower) or '[emotion:'.startswith(lower)):
                if len(stripped) <= 32 and ']' not in stripped:
                    return ''
                if ']' not in stripped or stripped.index(']') > 32:
                    raise ValueError('emotion tag bound')
                tag, self.pending = stripped.split(']', 1)
                prefix, sep, value = tag[1:].partition(':')
                if prefix.lower() not in ('state', 'emotion') or not sep or value.lower() not in self.ALLOWED:
                    raise ValueError('unsupported emotion')
                self.emotion = value.lower()
                self.stage = 'after_tag'
                continue
            if stripped.startswith('[') or stripped.startswith('<'):
                raise ValueError('unsupported model markup')
            self.stage = 'text'
            result, self.pending = self.pending, ''
            return self._text(result)

    def _text(self, text):
        # No late control frame is allowed after speech publication.
        if '[' in text or ']' in text or '<' in text or '>' in text:
            raise ValueError('late model markup')
        return text

    def finish(self):
        if self.stage in ('think', 'prefix') and self.pending:
            raise ValueError('incomplete model prefix')
        if self.stage == 'after_tag' and self.pending:
            raise ValueError('incomplete model tag')
        return ''
