#define KIM_VOICE_WORKER_TEST
#include "../../firmware/main/voice.c"
#include <assert.h>
#include <stdio.h>
int64_t test_now;
kim_trial_t kim_ota_trial;
atomic_bool test_cancel;
int test_writes, test_fail_write;
void (*test_during_write)(void);
int mock_status=200, mock_alloc_fail, mock_create_fail, mock_write_delay_us, mock_body_mode;
int mock_reset_exit;
atomic_int mock_closed, mock_codec_writes, mock_http_fallback, mock_restarts;
atomic_bool mock_mute;
uint8_t mock_body[8192];
int mock_body_size;
void *worker_entry(void *arg) { stream_playback_task(arg); return NULL; }
static void reset(void) {
    test_cancel=false; test_writes=0; test_fail_write=0; mock_status=200;
    mock_alloc_fail=mock_create_fail=mock_write_delay_us=mock_body_mode=0;
    mock_body_size=0; atomic_store(&kim_is_online,true);
    atomic_store(&kim_voice_state,KIM_THINKING);
    kim_playback_reset(&playback);
    atomic_store(&mock_closed,0); atomic_store(&mock_codec_writes,0);
    test_during_write=NULL; speaker=(void*)1;
}
static void slow_write(void) { if(mock_write_delay_us) usleep(mock_write_delay_us); atomic_fetch_add(&mock_codec_writes,1); }
static void blocked_write(void) { atomic_fetch_add(&mock_codec_writes,1); usleep(300000); }
static void wedged_write(void) { atomic_fetch_add(&mock_codec_writes,1); usleep(4000000); }
static void response(int frames, bool terminal) {
    /* wire A, little-endian length, PCM, then E/X */
    int offset=0;
    for(int f=0;f<frames;f++) {
        mock_body[offset++]='A';mock_body[offset++]=128;mock_body[offset++]=2;
        mock_body[offset++]=0;mock_body[offset++]=0;
        for(int i=0;i<640;i++)mock_body[offset++]=(i&1)?1:1;
    }
    if(terminal) { mock_body[offset++]='E'; for(int i=0;i<4;i++)mock_body[offset++]=0; }
    mock_body_size=offset;
}
int main(void) {
    reset();
    for(int status_i=0;status_i<3;status_i++) {
        mock_status=(int[]){404,405,501}[status_i];
        stream_result_t r=send_stream(0);
        assert(r==STREAM_UNSUPPORTED);
        if(r==STREAM_UNSUPPORTED && !cancelled() && kim_is_online)atomic_fetch_add(&mock_http_fallback,1);
        assert(atomic_load(&mock_closed)==status_i+1);
    }
    assert(atomic_load(&mock_http_fallback)==3);
    for(int status_i=0;status_i<3;status_i++) {
        reset(); mock_status=(int[]){404,405,501}[status_i];
        memset(mock_body,0,684);memcpy(mock_body,"RIFF",4);
        mock_body[4]=676&255;mock_body[5]=676>>8;
        memcpy(mock_body+8,"WAVEfmt ",8);mock_body[16]=16;
        mock_body[20]=1;mock_body[22]=1;
        mock_body[24]=0x80;mock_body[25]=0x3e;
        mock_body[28]=0;mock_body[29]=0x7d;
        mock_body[32]=2;mock_body[34]=16;
        memcpy(mock_body+36,"data",4);
        mock_body[40]=128;mock_body[41]=2;
        memset(mock_body+44,1,640);mock_body_size=684;
        send_response(0);
        assert(atomic_load(&mock_closed)==2);
        assert(test_writes==1);
    }
    reset(); mock_alloc_fail=1; assert(send_stream(0)==STREAM_FAILED); assert(atomic_load(&mock_closed)==0);
    reset(); response(3,true); mock_create_fail=1; assert(send_stream(0)==STREAM_FAILED); assert(atomic_load(&mock_closed)==1);
    reset();response(3,true);test_during_write=slow_write;assert(send_stream(0)==STREAM_OK);assert(atomic_load(&mock_codec_writes)==3);assert(atomic_load(&mock_closed)==1);
    reset();response(3,false);assert(send_stream(0)==STREAM_FAILED);assert(atomic_load(&mock_closed)==1);
    reset();response(3,true);test_fail_write=1;assert(send_stream(0)==STREAM_FAILED);assert(atomic_load(&mock_closed)==1);
    reset();response(3,true);mock_write_delay_us=300000;test_during_write=slow_write;
    test_cancel=true; assert(send_stream(0)==STREAM_FAILED);assert(atomic_load(&mock_closed)==1);
    reset();test_during_write=blocked_write;
    stream_player_t p;
    assert(stream_player_start(&p));
    stream_player=&p;
    static uint8_t pcm[32768];memset(pcm,1,sizeof(pcm));
    stream_deadline=esp_timer_get_time()+1000000;
    assert(stream_player_push(&p,pcm,1920));
    int64_t start=esp_timer_get_time();
    while(!atomic_load(&mock_codec_writes) && esp_timer_get_time()-start<500000) vTaskDelay(1);
    assert(atomic_load(&mock_codec_writes)==1);
    assert(kim_voice_display_state()==KIM_THINKING);
    assert(kim_voice_display_mouth()==0);
    /* Physical mute during a blocked codec write: wait for writer to exit,
     * never delete it, release its mutex, or close codec while it writes. */
    test_cancel=true;
    assert(!stream_player_finish(&p,false));
    assert(esp_timer_get_time()-start>=250000);
    assert(esp_timer_get_time()-start<2500000);
    stream_player=NULL;
    reset();test_during_write=blocked_write;
    assert(stream_player_start(&p));stream_player=&p;
    stream_deadline=esp_timer_get_time()+30000;
    assert(!stream_player_push(&p,pcm,sizeof(pcm)));
    assert(!stream_player_finish(&p,false));
    stream_player=NULL;
    pid_t child=fork();assert(child>=0);
    if(child==0) {
        reset();mock_reset_exit=1;test_during_write=wedged_write;
        stream_player_t wedged;
        assert(stream_player_start(&wedged));
        stream_deadline=esp_timer_get_time()+1000000;
        assert(stream_player_push(&wedged,pcm,1920));
        int64_t began=esp_timer_get_time();
        while(!atomic_load(&mock_codec_writes) && esp_timer_get_time()-began<500000)vTaskDelay(1);
        assert(atomic_load(&mock_codec_writes)==1);
        (void)stream_player_finish(&wedged,false);
        _exit(1); /* must restart, not return with writer still holding codec */
    }
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==42);
    puts("worker-path PASS");return 0;
}
