import pytest

from voice import SentenceSplitter


def chunks(*parts):
    split = SentenceSplitter()
    result = []
    for part in parts:
        result.extend(split.feed(part))
    return result + split.finish()


@pytest.mark.parametrize('parts,expected', [
    (('Dr. Müller kommt. ',), ['Dr. Müller kommt.']),
    (('Das ist z.B. gut. ',), ['Das ist z.B. gut.']),
    (('Es sind 3.14 Euro. Weiter! ',), ['Es sind 3.14 Euro.', 'Weiter!']),
    (('Es sind 42. Danach geht es weiter. ',), ['Es sind 42.', 'Danach geht es weiter.']),
    (('„Hallo!', '“ Danach. '), ['„Hallo!“', 'Danach.']),
    (('Was?', ' Ja! ',), ['Was?', 'Ja!']),
    (('Hallo.', ' Danach. '), ['Hallo.', 'Danach.']),
    (('<thi', 'nk>secret</think> Guten Tag. '), ['Guten Tag.']),
])
def test_german_sentence_boundaries(parts, expected):
    assert chunks(*parts) == expected


@pytest.mark.parametrize('input', ['A' * 241, 'A' * 240 + '. ', 'A' * 501])
def test_sentence_bounds(input):
    split = SentenceSplitter()
    with pytest.raises(ValueError):
        split.feed(input)


def test_total_reply_bound_and_thinking_safety():
    split = SentenceSplitter()
    with pytest.raises(ValueError):
        split.feed('<think>' + 'secret' * 400)
    split = SentenceSplitter()
    assert split.feed('A' * 230 + '. ') == ['A' * 230 + '.']
    assert split.feed('B' * 230 + '. ') == ['B' * 230 + '.']
    with pytest.raises(ValueError):
        split.feed('C' * 50 + '. ')
    split = SentenceSplitter()
    with pytest.raises(ValueError):
        split.feed('Hallo. <think>no</think> ')
