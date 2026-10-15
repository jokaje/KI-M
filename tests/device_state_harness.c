#include "kim_device_state.h"
#include <assert.h>
#include <string.h>

int main(void) {
    kim_device_state_t state = {0};
    char json[KIM_DEVICE_STATE_JSON_CAP];
    /* Boot unknown: report even if privacy is active. All combinations persist. */
    assert(kim_device_state_sample(&state, 100, true, false));
    assert(kim_device_state_due(&state, 100));
    unsigned epoch = kim_device_state_epoch(&state);
    assert(epoch > 0);
    assert(kim_device_state_format(&state, epoch, json, sizeof(json)));
    assert(!strcmp(json, "{\"version\":1,\"epoch\":1,\"muted\":true,\"menu_open\":false}"));
    kim_device_state_sent(&state, 100, epoch);
    assert(!kim_device_state_due(&state, 4999));
    assert(kim_device_state_due(&state, 5100));
    kim_device_state_sent(&state, 5100, epoch);
    assert(kim_device_state_sample(&state, 5110, true, true));
    assert(kim_device_state_due(&state, 5110));
    assert(!kim_device_state_format(&state, kim_device_state_epoch(&state), json, 10));
    assert(kim_device_state_format(&state, kim_device_state_epoch(&state), json, sizeof(json)));
    assert(!strcmp(json, "{\"version\":1,\"epoch\":2,\"muted\":true,\"menu_open\":true}"));
    /* An old in-flight send cannot acknowledge a newer privacy transition. */
    kim_device_state_sent(&state, 5120, epoch);
    assert(kim_device_state_due(&state, 5120));
    epoch = kim_device_state_epoch(&state);
    kim_device_state_sent(&state, 5130, epoch);
    assert(!kim_device_state_due(&state, 5130));
    assert(kim_device_state_sample(&state, 5140, false, true));
    assert(kim_device_state_due(&state, 5140));
    assert(kim_device_state_sample(&state, 5150, false, false));
    assert(kim_device_state_due(&state, 5150));
    /* Failed sends cannot quiet retries or silently reset physical gates. */
    kim_device_state_failed(&state, 5150);
    assert(kim_device_state_due(&state, 5400));
    assert(!state.muted && !state.menu_open);
    return 0;
}
