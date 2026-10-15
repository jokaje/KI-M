#include <assert.h>
#include <string.h>
#include "kim_character_state.h"
int main(void) {
    kim_character_poll_t p = {0};
    kim_character_state_t s;
#define PARSE(x) kim_character_parse((x), strlen(x), &s)
    assert(PARSE("{\"version\":1,\"recommended_state\":\"sleep\"}") && s == KIM_CHARACTER_SLEEP);
    assert(PARSE(" { \"recommended_state\" : \"happy\" , \"version\" : 1 } ") && s == KIM_CHARACTER_HAPPY);
    assert(!PARSE("{\"version\":1.0,\"recommended_state\":\"sleep\"}"));
    assert(!PARSE("{\"version\":1,\"recommended_state\":\"sleep\\u0000\"}"));
    assert(!PARSE("{\"version\":1,\"recommended_state\":\"idle\",\"recommended_state\":\"sleep\"}"));
    assert(!PARSE("{\"version\":2,\"recommended_state\":\"sleep\"}"));
    assert(!PARSE("{\"version\":1,\"recommended_state\":\"speaking\"}"));
    assert(!PARSE("{\"version\":1,\"recommended_state\":\"sleep\",\"thought\":\"private\"}"));
    assert(!PARSE("{\"version\":1,\"version\":1,\"recommended_state\":\"sleep\"}"));
    assert(!PARSE("{\"version\":1,\"recommended_state\":\"sleep\"}garbage"));
    assert(!kim_character_parse("{\"version\":1,\"recommended_state\":\"sleep\"}", 300, &s));
    kim_character_gate_t gate = {.online=true,.healthy=true,.idle=true,.released=true,.menu_allowed=true,.unmuted=true};
    assert(kim_character_due(&p, 10000, &gate));
    kim_character_poll_started(&p, 10000);
    assert(!kim_character_due(&p, 19999, &gate));
    assert(kim_character_due(&p, 20000, &gate));
    p.generation = 3;
    assert(!kim_character_publish(&p, 2, KIM_CHARACTER_SLEEP, 20000, &gate));
    gate.released = false;
    assert(!kim_character_publish(&p, 3, KIM_CHARACTER_SLEEP, 20000, &gate));
    gate.released = true;
    assert(kim_character_publish(&p, 3, KIM_CHARACTER_SLEEP, 20000, &gate));
    assert(kim_character_visible(&p, 20001, &gate) == KIM_CHARACTER_SLEEP);
    kim_character_interact(&p);
    assert(kim_character_visible(&p, 20002, &gate) == KIM_CHARACTER_IDLE);
    assert(!kim_character_publish(&p, 3, KIM_CHARACTER_SLEEP, 20003, &gate));
    kim_character_offer(&p, 3, KIM_CHARACTER_SLEEP);
    assert(!kim_character_consume(&p, 20004, &gate));
    kim_character_offer(&p, 4, KIM_CHARACTER_SLEEP);
    gate.released = false;
    assert(!kim_character_consume(&p, 20005, &gate));
    gate.released = true;
    kim_character_offer(&p, 4, KIM_CHARACTER_HAPPY);
    kim_character_interact(&p);
    assert(!kim_character_consume(&p, 20006, &gate));
    kim_character_offer(&p, 5, KIM_CHARACTER_SLEEP);
    assert(kim_character_consume(&p, 20007, &gate));
    assert(kim_character_visible(&p, 20008, &gate) == KIM_CHARACTER_SLEEP);
    gate.menu_allowed = false; assert(!kim_character_due(&p, 30000, &gate));
    gate.menu_allowed = true; gate.unmuted = false; assert(!kim_character_due(&p, 30000, &gate));
    gate.unmuted = true; gate.online = false; assert(!kim_character_due(&p, 30000, &gate));
    gate.online = true; gate.healthy = false; assert(!kim_character_due(&p, 30000, &gate));
    gate.healthy = true; gate.idle = false; assert(!kim_character_due(&p, 30000, &gate));
    gate.idle = true; assert(kim_character_due(&p, 30000, &gate));
    kim_character_poll_started(&p, 30000);
    assert(kim_character_publish(&p, p.generation, KIM_CHARACTER_THINKING, 30000, &gate));
    assert(kim_character_publish(&p, p.generation, KIM_CHARACTER_THINKING, 40000, &gate));
    assert(kim_character_publish(&p, p.generation, KIM_CHARACTER_THINKING, 50000, &gate));
    assert(kim_character_visible(&p, 60001, &gate) == KIM_CHARACTER_IDLE);
    assert(kim_character_visible(&p, 50001, &gate) == KIM_CHARACTER_IDLE);
    return 0;
}
