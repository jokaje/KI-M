#include <assert.h>
#include "barge_gate.h"
int main(void) {
    kim_barge_gate_t g;
    kim_barge_gate_reset(&g);
    for(int i=0;i<17;i++) assert(!kim_barge_gate_feed(&g,true,10,false,true));
    assert(kim_barge_gate_feed(&g,true,10,false,true));
    assert(!kim_barge_gate_feed(&g,true,10,true,true));
    for(int i=0;i<30;i++) assert(!kim_barge_gate_feed(&g,true,10,false,false));
    kim_barge_gate_reset(&g);
    for(int i=0;i<10;i++) assert(!kim_barge_gate_feed(&g,true,10,false,true));
    for(int i=0;i<8;i++) assert(!kim_barge_gate_feed(&g,false,10,false,true));
    for(int i=0;i<17;i++) assert(!kim_barge_gate_feed(&g,true,10,false,true));
    assert(kim_barge_gate_feed(&g,true,10,false,true));
    return 0;
}
