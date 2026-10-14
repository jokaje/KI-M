#include "kim_ble_protocol.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "cJSON.h"
void kim_ble_wipe(kim_ble_request_t *r) { if (r) { volatile unsigned char *p=(volatile unsigned char *)r; for(size_t i=0;i<sizeof *r;i++) p[i]=0; } }
static bool number(const cJSON *x, int min, int max) {
    return cJSON_IsNumber(x) && isfinite(x->valuedouble) && x->valuedouble >= min &&
           x->valuedouble <= max && floor(x->valuedouble)==x->valuedouble;
}
static bool ascii(const char *s, size_t min, size_t max) {
    if (!s) return false;
    size_t n=strlen(s); if(n<min || n>max) return false;
    for(size_t i=0;i<n;i++) if((unsigned char)s[i]<0x20 || (unsigned char)s[i]>0x7e) return false;
    return true;
}
kim_ble_parse_result_t kim_ble_parse(const void *data, size_t len, kim_ble_request_t *out) {
    if (!out) return KIM_BLE_INVALID;
    kim_ble_wipe(out);
    if (!data || !len || len>KIM_BLE_MAX_REQUEST || memchr(data,0,len)) return KIM_BLE_INVALID;
    /* Do not permit Unicode escapes: decoded NUL and aliasing would bypass
     * the byte-oriented SSID/password contract. */
    for (size_t i=0;i+1<len;i++) if (((const char *)data)[i]=='\\' &&
        ((const char *)data)[i+1]=='u') return KIM_BLE_INVALID;
    char raw[KIM_BLE_MAX_REQUEST+1]; memcpy(raw,data,len); raw[len]=0;
    const char *end=NULL;
    cJSON *root=cJSON_ParseWithLengthOpts(raw,len+1,&end,1);
    memset(raw,0,sizeof raw);
    if (!root || !cJSON_IsObject(root) || end != raw+len) goto invalid;
    unsigned seen=0;
    for(const cJSON *x=root->child;x;x=x->next) {
        static const char *const keys[]={"v","id","op","ssid","password","volume","brightness","sleep_s"};
        unsigned i=0; while(i<8 && strcmp(x->string,keys[i])) i++;
        if(i==8 || (seen&(1U<<i))) goto invalid;
        seen|=1U<<i;
    }
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(root,"v"), *id=cJSON_GetObjectItemCaseSensitive(root,"id"),
                *op=cJSON_GetObjectItemCaseSensitive(root,"op");
    if (!number(v,1,1) || !number(id,1,2147483647) || !cJSON_IsString(op) ||
        !ascii(op->valuestring,1,15)) goto invalid;
    out->id=(uint32_t)id->valuedouble;
    strcpy(out->op,op->valuestring);
    unsigned required=7;
    if (!strcmp(out->op,"save") || !strcmp(out->op,"forget")) {
        const cJSON *ssid=cJSON_GetObjectItemCaseSensitive(root,"ssid");
        if(!cJSON_IsString(ssid) || !ascii(ssid->valuestring,1,32)) goto invalid;
        strcpy(out->ssid,ssid->valuestring); required|=1U<<3;
        if(!strcmp(out->op,"save")) {
            const cJSON *pass=cJSON_GetObjectItemCaseSensitive(root,"password");
            if(!cJSON_IsString(pass) || !ascii(pass->valuestring,8,63)) goto invalid;
            strcpy(out->password,pass->valuestring); required|=1U<<4;
        }
    } else if (!strcmp(out->op,"settings_set")) {
        unsigned count=0;
        const cJSON *x=cJSON_GetObjectItemCaseSensitive(root,"volume");
        if(x) { if(!number(x,10,80)) goto invalid; out->volume=x->valueint; required|=1U<<5; count++; }
        x=cJSON_GetObjectItemCaseSensitive(root,"brightness");
        if(x) { if(!number(x,10,100)) goto invalid; out->brightness=x->valueint; required|=1U<<6; count++; }
        x=cJSON_GetObjectItemCaseSensitive(root,"sleep_s");
        if(x) { if(!number(x,0,1800) || (x->valueint!=0 && x->valueint<30)) goto invalid; out->sleep_s=x->valueint; required|=1U<<7; count++; }
        if(count!=1) goto invalid;
    } else if(strcmp(out->op,"scan") && strcmp(out->op,"networks") && strcmp(out->op,"settings_get")) goto invalid;
    if(seen!=required) goto invalid;
    out->fields=seen;
    cJSON_Delete(root);
    return KIM_BLE_VALID;
invalid:
    cJSON_Delete(root); kim_ble_wipe(out); return KIM_BLE_INVALID;
}
