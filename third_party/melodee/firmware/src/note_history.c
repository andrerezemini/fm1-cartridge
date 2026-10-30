/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* Sparse snapshots retain logical slot identity, including holes. The bitmap
 * follows the packed records so expansion can run backwards in place. Dense
 * snapshots use the original full array when a bitmap would cost more RAM. */
#define NOTE_HISTORY_MAP (RECORD_MAX / 8u)
_Static_assert(RECORD_MAX%8u==0,"history bitmap covers every event");
static int note_history_present(const recorded_note_t *r)
{
    const uint8_t *p=(const uint8_t *)r;uint32_t bits=0;
    for(uint32_t j=0;j<sizeof *r;j++)bits|=p[j];
    return bits!=0;
}
static uint32_t note_history_dense_count(uint32_t count)
{return count*sizeof(recorded_note_t)+NOTE_HISTORY_MAP>=sizeof recording?RECORD_MAX:count;}
static uint32_t note_history_bytes(uint32_t count)
{return count==RECORD_MAX?sizeof recording:count*sizeof(recorded_note_t)+NOTE_HISTORY_MAP;}
static int note_history_select(uint32_t i,int owner)
{return owner<0?note_history_present(&recording[i]):recording[i].vel&&recorded_owner(&recording[i])==(uint32_t)owner;}
static uint32_t note_history_count(int owner)
{
    uint32_t n=0;for(uint32_t i=0;i<RECORD_MAX;i++)n+=note_history_select(i,owner);
    return note_history_dense_count(n);
}
static void note_history_capture(recorded_note_t *notes,uint32_t count,int owner)
{
    memset(notes,0,note_history_bytes(count));
    uint8_t *map=(uint8_t *)notes+count*sizeof *notes;uint32_t n=0;
    for(uint32_t i=0;i<RECORD_MAX;i++)if(note_history_select(i,owner)){
        notes[count==RECORD_MAX?i:n++]=owner<0?recording[i]:recording_snapshot(i);
        if(count!=RECORD_MAX)map[i/8u]|=(uint8_t)(1u<<(i%8u));
    }
}
static void note_history_expand(recorded_note_t *notes,uint32_t count)
{
    if(count==RECORD_MAX)return;
    uint8_t map[NOTE_HISTORY_MAP];memcpy(map,(uint8_t *)notes+count*sizeof *notes,sizeof map);
    for(uint32_t i=RECORD_MAX;i--;){
        if(map[i/8u]&(1u<<(i%8u)))notes[i]=notes[--count];
        else memset(&notes[i],0,sizeof notes[i]);
    }
}
static uint32_t note_history_pack(recorded_note_t *notes)
{
    uint32_t count=0;for(uint32_t i=0;i<RECORD_MAX;i++)count+=note_history_present(&notes[i]);
    count=note_history_dense_count(count);if(count==RECORD_MAX)return count;
    uint8_t map[NOTE_HISTORY_MAP]={0};uint32_t n=0;
    for(uint32_t i=0;i<RECORD_MAX;i++)if(note_history_present(&notes[i])){
        map[i/8u]|=(uint8_t)(1u<<(i%8u));notes[n++]=notes[i];
    }
    memcpy((uint8_t *)notes+count*sizeof *notes,map,sizeof map);return count;
}
