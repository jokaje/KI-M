import ctypes
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'firmware/main'


class SettingsTest(unittest.TestCase):
    def test_settings_persist_validate_and_network_capacity(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / 'esp_err.h').write_text('typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 258\n#define ESP_ERR_INVALID_STATE 259\n#define ESP_ERR_NO_MEM 257\n#define ESP_ERR_NVS_NOT_FOUND 4354\n#define ESP_ERR_NVS_INVALID_LENGTH 4356\n')
            (p / 'nvs.h').write_text('#include <stddef.h>\n#include <stdint.h>\ntypedef int nvs_handle_t;\n#define NVS_READWRITE 1\nint nvs_open(const char*,int,nvs_handle_t*);\nint nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);\nint nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);\nint nvs_commit(nvs_handle_t);\nvoid nvs_close(nvs_handle_t);\n')
            (p / 'freertos').mkdir()
            (p / 'freertos/FreeRTOS.h').write_text('#define portMAX_DELAY 0xffffffffu\n')
            (p / 'freertos/semphr.h').write_text('#include <pthread.h>\n#include <stdlib.h>\ntypedef pthread_mutex_t *SemaphoreHandle_t;\nstatic inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { SemaphoreHandle_t m=malloc(sizeof(*m)); if(m) pthread_mutex_init(m,0); return m; }\nstatic inline int xSemaphoreTake(SemaphoreHandle_t m,unsigned ticks) { (void)ticks; return pthread_mutex_lock(m)==0; }\nstatic inline int xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(m)==0; }\n')
            (p / 'mock.c').write_text('''#include <string.h>
#include "nvs.h"
static char keys[16][16]; static unsigned char values[16][1024]; static size_t sizes[16]; static int count;
int nvs_open(const char *s,int m,nvs_handle_t *h){*h=1;return 0;}
int nvs_get_blob(nvs_handle_t h,const char *k,void *v,size_t *n){for(int i=0;i<count;i++)if(!strcmp(keys[i],k)){if(*n<sizes[i])return 4356;memcpy(v,values[i],sizes[i]);*n=sizes[i];return 0;}return 4354;}
int nvs_set_blob(nvs_handle_t h,const char *k,const void *v,size_t n){if(n>1024)return 258;int i=0;for(;i<count;i++)if(!strcmp(keys[i],k))break;if(i==count)count++;strcpy(keys[i],k);memcpy(values[i],v,n);sizes[i]=n;return 0;}
int nvs_commit(nvs_handle_t h){return 0;} void nvs_close(nvs_handle_t h){}
''')
            lib = p / 'lib.so'
            subprocess.run(['cc', '-shared', '-fPIC', '-pthread', '-std=c11', '-Wall', '-Werror', '-I', str(p), '-I', str(ROOT), str(ROOT / 'kim_settings.c'), str(p / 'mock.c'), '-o', str(lib)], check=True)
            c = ctypes.CDLL(str(lib))
            c.kim_settings_set_volume.argtypes = [ctypes.c_int]
            c.kim_settings_set_brightness.argtypes = [ctypes.c_int]
            c.kim_settings_set_sleep_s.argtypes = [ctypes.c_int]
            c.kim_settings_network_save.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
            c.kim_settings_network_forget.argtypes = [ctypes.c_char_p]
            self.assertEqual(c.kim_settings_init(), 0)
            self.assertEqual((c.kim_settings_volume(), c.kim_settings_brightness(), c.kim_settings_sleep_s()), (50, 70, 300))
            for setter, bad in ((c.kim_settings_set_volume, 81), (c.kim_settings_set_brightness, 9), (c.kim_settings_set_sleep_s, 29)):
                self.assertNotEqual(setter(bad), 0)
            self.assertEqual(c.kim_settings_set_volume(80), 0)
            self.assertEqual(c.kim_settings_set_brightness(10), 0)
            self.assertEqual(c.kim_settings_set_sleep_s(0), 0)
            self.assertEqual(c.kim_settings_init(), 0)
            self.assertEqual((c.kim_settings_volume(), c.kim_settings_brightness(), c.kim_settings_sleep_s()), (80, 10, 0))
            self.assertNotEqual(c.kim_settings_network_save(b'', b'12345678'), 0)
            self.assertNotEqual(c.kim_settings_network_save(b'a'*33, b'12345678'), 0)
            self.assertNotEqual(c.kim_settings_network_save(b'a', b'short'), 0)
            self.assertNotEqual(c.kim_settings_network_save(b'a', b'x'*64), 0)
            for i in range(8):
                self.assertEqual(c.kim_settings_network_save(f'net{i}'.encode(), b'12345678'), 0)
            self.assertEqual(c.kim_settings_network_count(), 8)
            self.assertNotEqual(c.kim_settings_network_save(b'net8', b'12345678'), 0)
            self.assertEqual(c.kim_settings_network_save(b'net2', b'abcdefgh'), 0)
            self.assertEqual(c.kim_settings_network_count(), 8)
            self.assertEqual(c.kim_settings_network_forget(b'net2'), 0)
            self.assertEqual(c.kim_settings_init(), 0)
            self.assertEqual(c.kim_settings_network_count(), 7)


if __name__ == '__main__':
    unittest.main()
