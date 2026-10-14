import ctypes
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / 'firmware/main/kim_ota_policy.h'


def library():
    source = '''#include "kim_ota_policy.h"
    int parse(const char *s, size_t n, kim_ota_manifest_t *m) { return kim_ota_parse(s,n,m); }
    int newer(const char *a,const char *b) { return kim_ota_version_compare(a,b); }
    int origin(const char *a,const char *b) { return kim_ota_same_origin(a,b); }
    int image(const char *s) { return kim_ota_image_name(s); }
    '''
    directory = tempfile.TemporaryDirectory()
    c = pathlib.Path(directory.name) / 'test.c'
    c.write_text(source)
    so = pathlib.Path(directory.name) / 'test.so'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-I', str(HEADER.parent), str(c), '-o', str(so)], check=True)
    lib = ctypes.CDLL(str(so))
    lib._temp = directory
    return lib


class Manifest(ctypes.Structure):
    _fields_ = [('version', ctypes.c_char * 32), ('image', ctypes.c_char * 96),
                ('sha256', ctypes.c_ubyte * 32), ('size', ctypes.c_size_t),
                ('signed_len', ctypes.c_size_t), ('signature', ctypes.c_ubyte * 80),
                ('signature_len', ctypes.c_size_t)]


def test_signed_manifest_strict_and_bounded():
    lib = library()
    lib.parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(Manifest)]
    s = (b'KIM-OTA-v1\nversion=0.3.7\nmodel=ki-m-box3\nsize=4096\nsha256=' + b'12'*32 +
         b'\nimage=kim-0.3.7.bin\nsignature=3006020101020101\n')
    m = Manifest()
    assert lib.parse(s, len(s), ctypes.byref(m)) == 1
    assert m.version == b'0.3.7' and m.size == 4096 and m.signed_len == s.index(b'signature=')
    canonical = s.replace(b'kim-0.3.7.bin', b'12'*32 + b'.bin')
    assert lib.parse(canonical, len(canonical), ctypes.byref(Manifest())) == 1
    for invalid in (s.replace(b'ki-m-box3', b'other-box'), s.replace(b'4096', b'0004096'),
                    s.replace(b'kim-0.3.7.bin', b'../kim-0.3.7.bin'),
                    s + b'extra', s.replace(b'signature=', b'Signature='), s * 20,
                    s.replace(b'0.3.7', b'0.3.7-rc1'), s.replace(b'12'*32, b'zz'*32)):
        assert lib.parse(invalid, len(invalid), ctypes.byref(Manifest())) == 0


def test_semver_and_origin_rejections():
    lib = library()
    lib.newer.argtypes = lib.origin.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    assert lib.newer(b'0.3.10', b'0.3.9') == 1
    assert lib.newer(b'0.3.6', b'0.3.6') == 0
    assert lib.newer(b'0.3.5', b'0.3.6') == -1
    assert lib.newer(b'0.3.x', b'0.3.6') == -2
    assert lib.newer(b'4294967296.0.0', b'0.3.6') == -2
    assert lib.newer(b'01.3.7', b'0.3.6') == -2
    assert lib.origin(b'https://example.org/ota/manifest', b'https://example.org/firmware/') == 1
    for url in (b'http://example.org/', b'https://example.org.evil/', b'https://example.org:444/',
                b'https://user@example.org/', b'https://example.org\\@evil/'):
        assert lib.origin(url, b'https://example.org/ota/') == 0
