/* SPDX-License-Identifier: GPL-3.0-only */
#include "fm1/audio_return.h"
#include "fm1/usb_midi.h"
#ifndef FM1_SIM_SYNTH_API
#define FM1_SIM_SYNTH_API clipper_synth_api
#define FM1_SIM_SYNTH_HEADER "clipper_synth.h"
#endif
#include FM1_SIM_SYNTH_HEADER
static const fm1_synth_api_t *S;
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void u16(FILE *f, uint16_t n) { fputc(n & 255, f); fputc(n >> 8, f); }
static void u32(FILE *f, uint32_t n) { u16(f,(uint16_t)n); u16(f,(uint16_t)(n>>16)); }
static void wav_header(FILE *f, uint32_t frames)
{
    rewind(f);
    fwrite("RIFF",1,4,f); u32(f,36+frames*4); fwrite("WAVEfmt ",1,8,f);
    u32(f,16); u16(f,1); u16(f,2); u32(f,44100); u32(f,44100*4);
    u16(f,4); u16(f,16); fwrite("data",1,4,f); u32(f,frames*4);
}
static bool number(const char *text, double lo, double hi, double *out)
{
    char *end;
    errno=0; double value=strtod(text,&end);
    if (errno || end==text || *end || !isfinite(value) || value<lo || value>hi) return false;
    *out=value; return true;
}
static unsigned midi_events;
static bool synth_enabled=true;
static bool midi(uint8_t status, uint8_t a, uint8_t b)
{
    uint8_t packet[4]={(uint8_t)(status>=0xf8 ? 15 : status>>4),status,a,b};
    fm1_midi_event_t event;
    if (!fm1_midi_decode(packet,&event)) return false;
    if (synth_enabled && S->midi(event.status,event.data1,event.data2)) return false;
    midi_events++; return true;
}
static void usage(const char *name)
{
    fprintf(stderr,"Usage: %s [--wav file.wav] [--report file.json] [--seconds 1..600]\n"
        "       [--ppm -2000..2000] [--faults] [--no-adaptive] [--tone]\n"
        "       [--engine n] [--preset n]\n",name);
}

int main(int argc, char **argv)
{
    const char *wav_path="cartridge.wav", *report_path="cartridge.json";
    double seconds=10, ppm=400, engine=0, preset=0;
    bool faults=false, adaptive=true, tone=false;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--faults")) faults=true;
        else if (!strcmp(argv[i],"--no-adaptive")) adaptive=false;
        else if (!strcmp(argv[i],"--tone")) tone=true;
        else if (!strcmp(argv[i],"--help")) { usage(argv[0]); return 0; }
        else {
            const char *key=argv[i];
            if (++i==argc) { usage(argv[0]); return 2; }
            bool ok=true;
            if (!strcmp(key,"--wav")) wav_path=argv[i];
            else if (!strcmp(key,"--report")) report_path=argv[i];
            else if (!strcmp(key,"--seconds")) ok=number(argv[i],1,600,&seconds);
            else if (!strcmp(key,"--ppm")) ok=number(argv[i],-2000,2000,&ppm);
            else if (!strcmp(key,"--engine")) ok=number(argv[i],0,255,&engine) && floor(engine)==engine;
            else if (!strcmp(key,"--preset")) ok=number(argv[i],0,255,&preset) && floor(preset)==preset;
            else ok=false;
            if (!ok) { usage(argv[0]); return 2; }
        }
    }
    if (!strcmp(wav_path,report_path)) { fputs("WAV and JSON paths must differ\n",stderr); return 2; }
    S=FM1_SIM_SYNTH_API();
    if (S->init(44100) || S->set_preset((uint32_t)engine,(uint32_t)preset)) {
        fprintf(stderr,"Unsupported %s preset\n",S->name); return 2;
    }
    FILE *wav=fopen(wav_path,"wb");
    if (!wav) { perror(wav_path); return 1; }
    FILE *report=fopen(report_path,"w");
    if (!report) { perror(report_path); fclose(wav); return 1; }
    wav_header(wav,0);
    synth_enabled=!tone; /* Tone mode still parses MIDI but has no synth consumer. */
    fm1_audio_return_t rx;
    fm1_return_init(&rx,adaptive);
    uint32_t packet_phase=0, total_frames=0, drops=0, reconnects=0;
    double consumer_phase=0, sine_phase=0, clock_phase=0;
    int16_t source[100], output[100];
    uint8_t packet[200];
    const uint8_t notes[]={60,64,67,72,67,64,62,55};
    uint8_t held=0;
    unsigned duration=(unsigned)(seconds*1000);
    for (unsigned ms=0;ms<duration;ms++) {
        bool disconnected=faults && ms>=3000 && ms<3300;
        if (faults && ms==3000) {
            S->panic(); fm1_return_reset(&rx); held=0;
        }
        if (faults && ms==3300) { fm1_return_reset(&rx); reconnects++; }
        size_t source_frames=fm1_packet_frames(44100,&packet_phase);
        if (!disconnected) {
            if (ms%500==0 || (faults && ms==3300)) {
                if (held && !midi(0x80,held,0)) goto midi_error;
                held=notes[(ms/500)%8];
                if (!midi(0x90,held,96)) goto midi_error;
            }
            if (ms%500==400 && held) {
                if (!midi(0x80,held,0)) goto midi_error;
                held=0;
            }
            clock_phase+=48; /* 120 BPM, 24 MIDI clocks/quarter note. */
            if (clock_phase>=1000) {
                clock_phase-=1000;
                if (!midi(0xf8,0,0)) goto midi_error;
            }
            if (tone) {
                for (size_t i=0;i<source_frames;i++) {
                    source[2*i]=source[2*i+1]=(int16_t)(sin(sine_phase)*3276.0);
                    sine_phase+=6.283185307179586*440.0/44100.0;
                    if (sine_phase>=6.283185307179586) sine_phase-=6.283185307179586;
                }
            } else S->render(source,source_frames);
            for (size_t i=0;i<source_frames*2;i++) {
                uint16_t n=(uint16_t)source[i];
                packet[2*i]=(uint8_t)n; packet[2*i+1]=(uint8_t)(n>>8);
            }
            bool lose=faults && ((ms>=6000 && ms<6100) || ms%997==996);
            if (lose) drops++;
            else (void)fm1_return_push(&rx,packet,source_frames*4);
            if (faults && ms==7000) (void)fm1_return_push(&rx,packet,3);
        }
        consumer_phase+=44.1*(1.0+ppm/1000000.0);
        size_t count=(size_t)consumer_phase;
        consumer_phase-=count;
        fm1_return_render(&rx,output,count);
        for (size_t i=0;i<count*2;i++) u16(wav,(uint16_t)output[i]);
        total_frames+=(uint32_t)count;
    }
    S->panic();
    wav_header(wav,total_frames);
    const fm1_return_stats_t *s=&rx.stats;
    fprintf(report,
        "{\n  \"schema\": 1,\n  \"simulation_only\": true,\n"
        "  \"synth\": \"%s\",\n  \"upstream_revision\": \"%s\",\n"
        "  \"source\": \"%s\",\n  \"engine\": %u,\n  \"preset\": %u,\n"
        "  \"sample_rate\": 44100,\n  \"seconds\": %.3f,\n"
        "  \"consumer_clock_ppm\": %.3f,\n  \"adaptive\": %s,\n"
        "  \"wav_frames\": %u,\n  \"midi_events\": %u,\n"
        "  \"received_frames\": %llu,\n  \"rendered_frames\": %llu,\n"
        "  \"silent_frames\": %llu,\n  \"underruns\": %u,\n"
        "  \"overruns\": %u,\n  \"malformed_packets\": %u,\n"
        "  \"injected_packet_losses\": %u,\n  \"reconnects\": %u,\n"
        "  \"fill_min\": %u,\n  \"fill_max\": %u,\n  \"final_ratio\": %.9f\n}\n",
        S->name,S->revision,tone ? "tone" : "synth",
        (unsigned)engine,(unsigned)preset,duration/1000.0,ppm,adaptive?"true":"false",
        total_frames,midi_events,(unsigned long long)s->received_frames,
        (unsigned long long)s->rendered_frames,(unsigned long long)s->silent_frames,
        s->underruns,s->overruns,s->malformed_packets,drops,reconnects,
        s->fill_min==FM1_RETURN_CAPACITY ? 0 : s->fill_min,s->fill_max,rx.ratio);
    bool failed=ferror(wav)||ferror(report);
    if (fclose(wav)) failed=true;
    if (fclose(report)) failed=true;
    if (failed) { fputs("Failed writing output\n",stderr); return 1; }
    printf("%s -> %s, %s; underruns=%u overruns=%u losses=%u (simulation only)\n",
           tone?"tone":S->engine_name((uint32_t)engine),wav_path,report_path,
           s->underruns,s->overruns,drops);
    return 0;
midi_error:
    fputs("Unexpected MIDI decoding/queue error\n",stderr);
    fclose(wav); fclose(report); return 1;
}
