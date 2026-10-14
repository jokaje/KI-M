#include <assert.h>
#include <stdint.h>
#include "avatar_player.h"
#include "generated/sprites.h"

/* Inject successful/failed decoder completion through production handoff. */
static void complete(kim_avatar_player_t *p, unsigned index, unsigned epoch) {
    unsigned i,e;
    assert(kim_avatar_begin_decode(p,KIM_AVATAR_BYTES,&i,&e));
    assert(i==index && e==epoch);
    kim_avatar_finish_decode(p,i,e,true);
}
int main(void) {
    assert(kim_avatar_select_clip(KIM_OFFLINE,0,0)==6);
    assert(kim_avatar_select_clip(KIM_SLEEP,0,0)==6);
    assert(kim_avatar_select_clip(KIM_IDLE,0,0)==0);
    assert(kim_avatar_select_clip(KIM_LISTENING,0,0)==1);
    assert(kim_avatar_select_clip(KIM_THINKING,0,0)==2);
    assert(kim_avatar_select_clip(KIM_SPEAKING,0,0)==3);
    assert(kim_avatar_select_clip(KIM_IDLE,1,0)==4);
    assert(kim_avatar_select_clip(KIM_IDLE,2,0)==5);
    assert(kim_avatar_select_clip(KIM_NEUTRAL,0,0)==0);
    assert(kim_avatar_select_clip(KIM_IDLE,0,1)==6);
    assert(kim_avatar_select_clip(KIM_IDLE,0,2)==6);
    assert(kim_avatar_elapsed(6,0,0,80000,0)==4000);
    assert(kim_avatar_elapsed(6,1,80000,80000,0)==4000);
    assert(kim_avatar_elapsed(6,2,1000,80000,0)==5000);
    assert(kim_avatar_elapsed(0,0,0,9000,8000)==1000);
    uint16_t pixels[KIM_AVATAR_SIZE * KIM_AVATAR_SIZE]={0};
    const uint8_t tile[]={255,0,0, 0,255,0, 0,0,255, 255,255,255};
    assert(kim_avatar_copy_rgb_rect(pixels,tile,2,3,3,4));
    assert(pixels[3*KIM_AVATAR_SIZE+2]==0xf800);
    assert(pixels[3*KIM_AVATAR_SIZE+3]==0x07e0);
    assert(pixels[4*KIM_AVATAR_SIZE+2]==0x001f);
    assert(pixels[4*KIM_AVATAR_SIZE+3]==0xffff);
    assert(pixels[3*KIM_AVATAR_SIZE+4]==0);
    assert(!kim_avatar_copy_rgb_rect(pixels,tile,0,0,KIM_AVATAR_SIZE,0));
    assert(!kim_avatar_frame_valid(KIM_AVATAR_FRAME_COUNT,KIM_AVATAR_BYTES));
    assert(!kim_avatar_frame_valid(0,0));
    assert(!kim_avatar_frame_valid(0,KIM_AVATAR_BYTES-1));
    for (unsigned i=0;i<KIM_AVATAR_FRAME_COUNT;i++) assert(kim_avatar_frame_valid(i,KIM_AVATAR_BYTES));
    uint16_t a=1,b=2,fallback=3;
    kim_avatar_player_t p={.front=&a,.back=&b};
    kim_avatar_request(&p,100,100,1);
    assert(atomic_load(&p.desired)==0);
    for (unsigned clip=0;clip<KIM_AVATAR_CLIPS;clip++) {
        kim_avatar_request(&p,clip,UINT32_MAX,clip+1);
        unsigned f=atomic_load(&p.desired);
        assert(f>=kim_avatar_clips[clip].start && f<kim_avatar_clips[clip].start+kim_avatar_clips[clip].count);
    }
    kim_avatar_request(&p,0,0,10);
    assert(kim_avatar_display_data(&p,&fallback)==&fallback);
    assert(!kim_avatar_present(&p));
    unsigned index,epoch;
    for (unsigned retry=0;retry<3;retry++) {
        assert(kim_avatar_begin_decode(&p,KIM_AVATAR_BYTES,&index,&epoch));
        assert(index==0 && epoch==10);
        kim_avatar_finish_decode(&p,index,epoch,false);
        assert(kim_avatar_display_data(&p,&fallback)==&fallback);
    }
    complete(&p,0,10);
    assert(kim_avatar_present(&p));
    assert(p.front==&b && p.back==&a && atomic_load(&p.shown)==0 && atomic_load(&p.phase)==2);
    assert(kim_avatar_display_data(&p,&fallback)==&b);
    kim_avatar_release(&p); /* UI retired former source before worker may reuse it. */
    assert(!kim_avatar_begin_decode(&p,KIM_AVATAR_BYTES,&index,&epoch));

    kim_avatar_request(&p,0,0,11);
    assert(kim_avatar_begin_decode(&p,KIM_AVATAR_BYTES,&index,&epoch));
    kim_avatar_request(&p,1,0,12);
    assert(kim_avatar_display_data(&p,&fallback)==&b);
    kim_avatar_finish_decode(&p,index,epoch,true); /* A completes after B request */
    assert(!kim_avatar_present(&p));
    assert(!atomic_load(&p.phase) && p.front==&b);
    complete(&p,kim_avatar_clips[1].start,12);
    assert(kim_avatar_present(&p));
    assert(kim_avatar_display_data(&p,&fallback)==&a);
    kim_avatar_release(&p);

    kim_avatar_request(&p,0,0,13);
    assert(kim_avatar_begin_decode(&p,KIM_AVATAR_BYTES,&index,&epoch));
    kim_avatar_request(&p,1,0,14);
    kim_avatar_request(&p,0,0,15); /* reenter A, same frame but new epoch */
    kim_avatar_finish_decode(&p,index,epoch,true);
    assert(!kim_avatar_present(&p));
    assert(kim_avatar_display_data(&p,&fallback)==&a);
    complete(&p,0,15);
    assert(kim_avatar_present(&p));
    assert(kim_avatar_display_data(&p,&fallback)==&b);
    kim_avatar_release(&p);

    kim_avatar_request(&p,0,0,16);
    assert(kim_avatar_begin_decode(&p,KIM_AVATAR_BYTES,&index,&epoch));
    kim_avatar_request(&p,0,1000,16); /* frame advances during decode */
    kim_avatar_finish_decode(&p,index,epoch,true);
    assert(kim_avatar_present(&p)); /* completed in same clip/epoch, though desired moved */
    assert(kim_avatar_display_data(&p,&fallback)==p.front);
    kim_avatar_release(&p);
    /* Ten UI polls per second versus six authored frames/second: the last
     * validated front remains visible while the worker is still decoding. */
    kim_avatar_request(&p,0,1200,16);
    assert(kim_avatar_display_data(&p,&fallback)==p.front);
    assert(kim_avatar_begin_decode(&p,KIM_AVATAR_BYTES,&index,&epoch));
    kim_avatar_request(&p,0,1500,16);
    assert(kim_avatar_display_data(&p,&fallback)==p.front);
    kim_avatar_finish_decode(&p,index,epoch,false);
    assert(kim_avatar_display_data(&p,&fallback)==p.front);
    /* A new generation (including reentry into the SAME clip) retains the
     * old front visibly, but cannot present it as a fresh completion. */
    kim_avatar_request(&p,0,0,17);
    assert(kim_avatar_display_data(&p,&fallback)==p.front);
    return 0;
}
