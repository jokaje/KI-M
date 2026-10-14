#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/main/kim_ble_protocol.h"
static void good(const char *s, const char *op) { kim_ble_request_t r; int v=kim_ble_parse(s,strlen(s),&r); if(v!=KIM_BLE_VALID) fprintf(stderr,"failed op %s\n",op); assert(v==KIM_BLE_VALID); assert(!strcmp(r.op,op)); kim_ble_wipe(&r); }
static void bad(const char *s) { kim_ble_request_t r; assert(kim_ble_parse(s,strlen(s),&r)!=KIM_BLE_VALID); }
int main(void) {
 good("{\"v\":1,\"id\":7,\"op\":\"scan\"}","scan");
 good("{\"v\":1,\"id\":1,\"op\":\"save\",\"ssid\":\"Synthetic\",\"password\":\"12345678\"}","save");
 good("{\"v\":1,\"id\":8,\"op\":\"forget\",\"ssid\":\"Synthetic\"}","forget");
 good("{\"v\":1,\"id\":9,\"op\":\"settings_set\",\"volume\":10}","settings_set");
 bad("{\"v\":1,\"id\":1,\"op\":\"save\",\"ssid\":\"x\",\"password\":\"1234567\"}");
 bad("{\"v\":1,\"id\":1,\"op\":\"scan\",\"unknown\":true}");
 bad("{\"v\":1,\"id\":1,\"op\":\"scan\",\"id\":2}");
 bad("{\"v\":1,\"id\":1.5,\"op\":\"scan\"}");
 bad("{\"v\":1,\"id\":0,\"op\":\"scan\"}");
 bad("{\"v\":1,\"id\":1,\"op\":\"settings_set\",\"volume\":9}");
 bad("{\"v\":1,\"id\":1,\"op\":\"settings_set\",\"sleep_s\":1}");
 bad("{\"v\":1,\"id\":1,\"op\":\"settings_set\",\"brightness\":101}");
 bad("{\"v\":1,\"id\":1,\"op\":\"save\",\"ssid\":\"a\\u0000b\",\"password\":\"12345678\"}");
 bad("{\"v\":1,\"id\":1,\"op\":\"forget\",\"ssid\":\"\u00e9\"}");
 char over[600]; memset(over,' ',sizeof over); assert(kim_ble_parse(over,sizeof over,(kim_ble_request_t[1]){{0}})!=KIM_BLE_VALID);
 const char nul[]="{\"v\":1}\0{}"; assert(kim_ble_parse(nul,sizeof nul-1,(kim_ble_request_t[1]){{0}})!=KIM_BLE_VALID);
 puts("BLE protocol: 4 valid, 12 rejected");
}
