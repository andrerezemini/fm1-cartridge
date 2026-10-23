/* SPDX-License-Identifier: GPL-3.0-only
 * Exercise the actual core-0 mix insertion against the existing host DSP and
 * core-1 job double. Build twice (-DT_RX=0/1): BASELINE hashes must match.
 * The return-off and gain-zero cases must be bit-exact baseline. Return-on must
 * be audible and identical for core-1 splits 0, 15 and automatic (-1).
 * cc -O2 -w -Ibuild/gen -Ifirmware/src -Ifirmware/hal tests/uac_rx_mix_test.c -lm -pthread */
#include <stdint.h>
#include <assert.h>
#ifndef T_RX
#define T_RX 1
#endif
#define FELUCCA_UAC_RX T_RX
#define UAC_RX_BARRIER() __asm__ volatile("" ::: "memory")
#define UAC_RX_LOCK() 0u
#define UAC_RX_UNLOCK(s) ((void)(s))
#include "../firmware/src/uac_rx.c"
#define main hostsim_main
#include "hostsim.c"
#undef main

typedef struct { uint64_t hash; uint32_t jobs, returned; } rx_result;
#if T_RX
static uint32_t rx_sequence;
static void rx_feed(uint32_t frames)
{
    uint8_t p[180];
    uint32_t i;
    for(i=0;i<frames;i++,rx_sequence++) {
        int32_t s=(int32_t)(rx_sequence%128u)*128-8192, t=-s/2;
        p[4*i]=(uint8_t)s; p[4*i+1]=(uint8_t)((uint32_t)s>>8);
        p[4*i+2]=(uint8_t)t; p[4*i+3]=(uint8_t)((uint32_t)t>>8);
    }
    uac_rx_push(p,frames*4);
}
#endif
static rx_result rx_render(int mode, int32_t split)
{
    rx_result result={0xcbf29ce484222325ull,0,0};
    uint32_t b,i,k;
    cpu1_host_mode(); cpu1_units=split;
    host_tracks_init();
    host_preset(&trk[0],0,1); host_preset(&trk[1],ENGI_FM6,2); host_preset(&trk[2],7,3);
    for(k=0;k<NPART;k++) {
        uint8_t note=(uint8_t)(48+5*k);
        trk[k].p[P_REV]=25; trk[k].p[P_DLY]=20;
        put_step(&trk[k],0,1,&note,ST_NOTE,0);
    }
    { uint8_t note=36; put_step(TDRUM,0,1,&note,ST_NOTE,0); }
    transport_req=1;
#if T_RX
    uac_rx_stream(mode!=0);
    uac_rx_gain=(uint8_t)(mode==1?0:128);
    if(mode) for(i=0;i<9;i++) rx_feed(44);
#else
    (void)mode;
#endif
    for(b=0;b<800;b++) {
        int32_t out[2*CTL];
#if T_RX
        if(mode) rx_feed(32);
#endif
        mix_block(out,CTL);
        for(i=0;i<2*CTL;i++) for(k=0;k<4;k++) {
            result.hash^=((uint32_t)out[i]>>(8*k))&255u;
            result.hash*=0x100000001B3ull;
        }
    }
    result.jobs=cpu1_host_jobs();
#if T_RX
    result.returned=uar_r;
    if(mode) assert(uar.go && !uar.underruns && !uar.overruns && uar_r>25000u);
#endif
    return result;
}
static rx_result rx_child(int mode,int32_t split)
{
    int fd[2],status=0;
    pid_t pid;
    size_t got=0;
    ssize_t n;
    rx_result result={0};
    assert(!pipe(fd)); fflush(stdout);
    pid=fork(); assert(pid>=0);
    if(!pid) {
        rx_result child=rx_render(mode,split);
        close(fd[0]);
        if(write(fd[1],&child,sizeof child)!=(ssize_t)sizeof child) _exit(2);
        _exit(0);
    }
    close(fd[1]);
    while(got<sizeof result && (n=read(fd[0],(char *)&result+got,sizeof result-got))>0) got+=(size_t)n;
    close(fd[0]); waitpid(pid,&status,0);
    assert(got==sizeof result && WIFEXITED(status) && WEXITSTATUS(status)==0);
    return result;
}
int main(void)
{
    static const int32_t splits[]={0,15,-1};
    rx_result base=rx_child(0,0);
    uint32_t i;
    printf("BASELINE %016llx\n",(unsigned long long)base.hash);
    for(i=0;i<3;i++) {
        rx_result off=rx_child(0,splits[i]);
        assert(off.hash==base.hash);
        if(splits[i]) assert(off.jobs>0);
#if T_RX
        {
            rx_result mute=rx_child(1,splits[i]);
            rx_result on=rx_child(2,splits[i]);
            rx_result on_single=rx_child(2,0);
            assert(mute.hash==base.hash && on.hash!=base.hash && on.hash==on_single.hash);
            assert(on.returned==on_single.returned); /* one consumption per block, independent of core-1 work */
            printf("split%3d: off/mute exact, audible return once, consumed %u, jobs %u\n",splits[i],on.returned,on.jobs);
        }
#endif
    }
    puts("uac_rx mixer: native checks passed (hardware untested)");
    return 0;
}
