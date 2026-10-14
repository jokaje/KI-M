"""Bounded sentence-chunk speech conversion; never retain transcripts or PCM."""
import io
import re
import wave

import numpy as np

MAX_WAV = 384044


def strip_thinking(text):
    return re.sub(r'^\s*<think>.*?</think>\s*', '', text, flags=re.S | re.I)


class SentenceSplitter:
    # Conservative fixed abbreviations; avoid splitting a title from its name.
    ABBREVIATIONS = frozenset(('dr.', 'prof.', 'hr.', 'fr.', 'bzw.', 'ca.', 'z.b.',
                               'd.h.', 'u.a.', 'usw.', 'vgl.', 'nr.', 'str.'))
    CLOSERS = '“”„\"\'»«)]}'

    def __init__(self):
        self.pending = ''
        self.started = False
        self.produced = 0

    def feed(self, token):
        self.pending += token
        if not self.started:
            trimmed = self.pending.lstrip()
            if '<think>'.startswith(trimmed.lower()) and len(trimmed) < 7:
                return []
            if trimmed.lower().startswith('<think>'):
                if '</think>' not in trimmed.lower():
                    if len(self.pending) > 2048:
                        raise ValueError('thinking bound')
                    return []
                self.pending = strip_thinking(self.pending)
            self.started = True
        if '<think' in self.pending.lower() or '</think>' in self.pending.lower():
            raise ValueError('unexpected thinking output')
        if len(self.pending) + self.produced > 500:
            raise ValueError('reply too long')
        sentences = []
        while True:
            boundary = None
            for i, char in enumerate(self.pending):
                if char not in '.!?':
                    continue
                if char == '.':
                    if i and i + 1 < len(self.pending) and self.pending[i-1].isdigit() and self.pending[i+1].isdigit():
                        continue
                    word = self.pending[:i+1].lower().split()[-1]
                    if word in self.ABBREVIATIONS or any(self.pending[:i+1].lower().endswith(abbr) for abbr in ('z.b.', 'd.h.', 'u.a.')):
                        continue
                    # A dot between letters can be the first half of z.B. across deltas.
                    if i + 1 < len(self.pending) and self.pending[i+1].isalpha() and i and self.pending[i-1].isalpha():
                        continue
                end = i + 1
                while end < len(self.pending) and self.pending[end] in self.CLOSERS:
                    end += 1
                # Wait for lookahead: a token can end at punctuation or before a closing quote.
                if end == len(self.pending):
                    continue
                if self.pending[end].isspace():
                    boundary = end
                    break
            if boundary is None:
                if len(self.pending) > 240:
                    raise ValueError('sentence too long')
                break
            sentence = self.pending[:boundary].strip()
            if len(sentence) > 240:
                raise ValueError('sentence too long')
            self.pending = self.pending[boundary:].lstrip()
            if sentence:
                self.produced += len(sentence)
                sentences.append(sentence)
        return sentences

    def finish(self):
        if not self.started and self.pending.lstrip() and (self.pending.lstrip().lower().startswith('<think>') or
                                 '<think>'.startswith(self.pending.lstrip().lower())):
            raise ValueError('unclosed thinking')
        sentence = self.pending.strip()
        self.pending = ''
        if '<think' in sentence.lower() or '</think>' in sentence.lower():
            raise ValueError('unexpected thinking output')
        if len(sentence) > 240 or self.produced + len(sentence) > 500:
            raise ValueError('reply too long')
        return [sentence] if sentence else []


def decode_tts_wav(data):
    if not 46 <= len(data) <= MAX_WAV:
        raise ValueError('TTS size')
    try:
        with wave.open(io.BytesIO(data), 'rb') as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) != (1, 2, 24000, 'NONE'):
                raise ValueError('TTS format')
            count = wav.getnframes()
            if not 0 < count <= 192000 or len(data) != 44 + count * 2:
                raise ValueError('TTS length')
            pcm = wav.readframes(count)
    except wave.Error as exc:
        raise ValueError('invalid TTS wave') from exc
    audio = np.frombuffer(pcm, dtype='<i2').astype(np.float32)
    output_count = count * 2 // 3
    output = np.interp(np.arange(output_count) * 1.5, np.arange(count), audio)
    return np.clip(output, -32768, 32767).astype('<i2').tobytes()
