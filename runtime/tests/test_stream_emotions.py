import pytest

from response_tags import ResponseTags


def test_split_tag_and_utf8_chunks_never_reach_tts():
    parser = ResponseTags()
    out = []
    for chunk in ('[ST', 'ATE:ha', 'ppy] H', 'allo ', 'Welt.'):
        out.append(parser.feed(chunk))
    assert ''.join(out) == 'Hallo Welt.'
    assert parser.emotion == 'happy'
    assert parser.finish() == ''


def test_unknown_and_oversized_tags_fail_closed():
    for text in ('[STATE:sleep] Hallo', '[STATE:' + 'a' * 90, '[STATE:happy', '[EMOTION:offline]'):
        parser = ResponseTags()
        with pytest.raises(ValueError):
            parser.feed(text)
            parser.finish()


def test_plain_reply_and_late_tag_rejected():
    parser = ResponseTags()
    assert parser.feed('Hallo ') == 'Hallo '
    with pytest.raises(ValueError):
        parser.feed('[STATE:happy]')


def test_think_prefix_then_tag():
    parser = ResponseTags()
    assert parser.feed('<think></think>[STATE:happy] Ja.') == 'Ja.'
    assert parser.emotion == 'happy'
