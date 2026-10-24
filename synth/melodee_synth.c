/* SPDX-License-Identifier: GPL-3.0-only
 * Portable adapter to the actual Melodee DSP (keremimo/melodee 1.4.5), not an emulation of its
 * sound. Upstream sources retain their copyright and license notices. The include order is the
 * one of tests/hostsim.c upstream: the DSP, without the USB device, the UI, storage or the HAL.
 * One core: the AC7911's second core (MELODEE_DUAL_CORE) is not here, upstream's serial path is. */
#include "melodee_synth.h"
#include <string.h>

#define MELODEE_DUAL_CORE 0
#define MELODEE_USB_AUDIO 0
#define MELODEE_CDC 0
#define MELODEE_FLASH 0
#define MELODEE_OTA 0
#define MELODEE_AUDIO_PROFILE 0
/* the audio working-memory arena (resources.c): one part's engine state and its FX; the host's
 * 160 KB is more than the cartridge needs, the ESP32 keeps it in internal RAM */
#ifndef RESOURCE_HOST_WORDS
#define RESOURCE_HOST_WORDS 16384
#endif
#ifndef RESOURCE_HOST_LOW_WORDS
#define RESOURCE_HOST_LOW_WORDS 2048
#endif
#ifndef RESOURCE_HOST_CACHE_WORDS
#define RESOURCE_HOST_CACHE_WORDS 64
#endif

/* The upstream linker sections describe the AC7911 memory map. Preserve the alignment/noinline
 * attributes, but let the platform linker place this port (melodee.lf on the ESP32). */
#define section(name)
#define bootguard melodee_unused_bootguard
#include "../third_party/melodee/generated/melodee_tables.h"
#ifdef memset
#undef memset
#endif
#ifdef memcpy
#undef memcpy
#endif
#define memset melodee_upstream_memset
#define memcpy melodee_upstream_memcpy
#define memcmp melodee_upstream_memcmp
#include "../third_party/melodee/firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp

/* All DSP calls are serialized by the caller. No physical keyboard, buttons, interrupt control or
 * USB device driver: the keys stay up, the USB-MIDI queue is fed by melodee_synth_midi. */
static struct { volatile uint32_t notes, buttons; } fm1_in;
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
#include "../third_party/melodee/firmware/src/core.h"
#include "../third_party/melodee/firmware/src/engines.c"
#include "../third_party/melodee/firmware/src/params.c"
#include "../third_party/melodee/firmware/src/mod.c"
#include "../third_party/melodee/firmware/src/voice.c"
#include "../third_party/melodee/firmware/src/mix.c"
/* the queue upstream usb.c / midi_uart.c fill and seq.c events_block drains (source 1: USB) */
#define MQ 64u
static uint32_t midi_in_q[MQ], midi_in_ms[MQ], midi_in_time[MQ];
static uint8_t midi_in_source[MQ];
static volatile uint32_t mi_w, mi_r;
static volatile uint8_t midi_in_overflow;    /* set by the port when its queue overflows: events_block releases every note */
static void midi_out_event(uint32_t packet) { (void)packet; }   /* the FM-1 is the host: no MIDI out */
/* upstream usb.c's block timeline (the ISR's): events are not timed here, every one plays at its block */
#define MIDI_TIME_NOW() fm1_ms
#define MIDI_TICKS_PER_MS 1u
static uint32_t midi_render_time;
static uint8_t midi_render_timed;
#include "../third_party/melodee/firmware/src/song_chain.c"
#include "../third_party/melodee/firmware/src/seq.c"
/* upstream ui.c's step_clear (the UI is not part of this port): motion.c's pattern_clear uses it */
static void step_clear(step_t *st)
{
    st->n = 0;
    st->time = ST_REST;
    st->flags = 0;
    st->vel = 0;
    st->hit = st->acc = 0;
    st->probability = 0;
}
#undef section
#undef bootguard

static int ready, silenced = 1;
/* The port's own setting: the channel the instrument answers, 0 OMNI or 1..16. Entry 0 of the
 * remote catalogue ("MIDI CH"). Upstream routes MIDI IN SEL (every channel) to the selected
 * track; the filter here keeps the FM-1's other tracks off the cartridge unless asked. */
static int16_t port_midi_ch;
static const char *const PORT_N_MCH[] = {"OMNI", "1", "2", "3", "4", "5", "6", "7", "8",
                                         "9", "10", "11", "12", "13", "14", "15", "16"};
static const param_desc_t PORT_MIDI_CH_DESC = {"CH", F_ENUM, 0, 16, 0, PORT_N_MCH, 0};
#define PORT_MIDI_CH_ID (P_COUNT + G_COUNT)
static int16_t pcm[2u * CTL];
static uint32_t pcm_pos = CTL;
static uint64_t rendered_frames;

static void steps_rest(track_t *t)
{
    for (uint32_t i = 0; i < NSTEP; ++i) { memset(&t->step[i], 0, sizeof t->step[i]); t->step[i].time = ST_REST; }
}

static void clear_effects(void)
{
    memset(fx_state, 0, sizeof fx_state);
    for (uint32_t k = RES_FX0; k < RES_FX0 + FX_STATE_COUNT; ++k) resource_release(k);
    memset(mix_cache, 0, sizeof mix_cache);
    dc_l = dc_r = dce_l = dce_r = 0;      /* the master's DC blocker (mix.c master_out) */
    lim_env = LIM_T;
    beat_pos = beat_n = 0;
    seq_erase_transport = 0;
    pattern_epoch = 0;
    recording_generation = 0;
}

static void melodee_panic(void)
{
    uint32_t i;
    mi_r = mi_w = 0;
    silenced = 1;
    pcm_pos = CTL;
    memset(pcm, 0, sizeof pcm);
    if (!ready)
        return;
    for (i = 0; i < NTRK; ++i) {
        track_t *t = &trk[i];
        trk_all_off(t);
        memset(t->v, 0, sizeof t->v);
        t->nheld = t->arp_phys = t->latched = t->nmono = t->xp_n = 0;
        t->arp_note = 0;
        t->xf_on = t->xf = 0;
        t->seq_n = 0;
        t->mw = t->at = t->ex_off = t->foot = t->breath = t->porta = 0;
        t->bend_raw = 0;
    }
    memset(midi_ch, 0, sizeof midi_ch);
    memset(midi_sel_on, 0, sizeof midi_sel_on);
    memset(midi_owners, 0, sizeof midi_owners);
    memset(mchord, 0, sizeof mchord);
    midi_hint = 0;
    memset(midi_bend_target, 0, sizeof midi_bend_target);
    memset(midi_bend_q8, 0, sizeof midi_bend_q8);
    memset(&midi_clock, 0, sizeof midi_clock);
    transport_req = panic_req = 0;
    midi_in_overflow = 0;
    song.playing = song.rec = 0;
    clear_effects();
}

/* The patch hooks of a preset (ui.c engine_load_of): PROPHET's, CZ-1's and FM6's patch memory. While
 * the old sound of the same engine fades out they would rewrite the patch under it (a click), so
 * they wait for the end of the fade (melodee_render), before the notes held for it start. */
static uint8_t hooks_pending;
/* Upstream's fade renders the old sound with the old engine values (pe_old) and the new common ones;
 * here the common ones of the fading sound too (its envelope, filter, effects), as Clipper's
 * sound_fade_begin does: kept from the fade's start and put back for each of its blocks (melodee_render) */
static int16_t fade_p[P_E0];
static void load_hooks(track_t *t)
{
    hooks_pending = 0;
    if (t->eng_req == ENGI_PROPHET) { p5_ready[0] = 1; p5_preset_loaded(t, t->preset); }
    cz_factory_loaded(t);
    cz_track_accept(t);
    fm6_track_loaded(t);
}

/* preset pi of upstream engine e into part 1, as ui.c engine_load_of and apply_preset_to: the
 * sound; not the steps, the arp or the mix of the part (hostsim host_preset). fade: the voices
 * sounding fade out over XF_BLOCKS on the old sound and the engine follows after them (voice.c
 * engine_block, events_block releases the part: panic_req); else they stop at once (init). DRUM
 * lays its state out by KIT (eng_drum.c drum_bytes: the 909, the SID kit, the lanes of the
 * others), sized from the new values while the fade renders with the old ones: a change of
 * layout under the hits cuts them instead of fading them. */
static uint32_t drum_layout(const track_t *t) { return drum_is909(t) ? 1u : drum_issid(t) ? 2u : 3u; }
static void load_preset(uint32_t e, uint32_t pi, int fade)
{
    track_t *t = &trk[0];
    const engine_t *eng = ENGINES[e];
    uint32_t i, any = 0, drum = t->engine == ENGI_DRUM ? drum_layout(t) : 0;
    if (fade) {
        for (i = 0; i < NVOICE; ++i)
            any |= t->v[i].active;
        if (any && !t->xf_on) {
            t->xf_on = 1; t->xf = XF_BLOCKS;
            memcpy(fade_p, t->p, sizeof fade_p);
        }
        panic_req |= 1u;
    } else {
        trk_all_off(t);
        memset(t->v, 0, sizeof t->v);
    }
    for (i = 0; i < P_E0; ++i)
        t->p[i] = TP[i].def;
    t->user = 0; t->user_native = 0;
    t->eng_req = (uint8_t)e;
    for (i = 0; i < 8u; ++i)
        t->p[P_E0 + i] = eng->edit[i].def;
    memset(&drum_patch[0], 0, sizeof(drum_patch_t));
    if (!eng->npresets) {
        t->preset = 0;
    } else {
        const preset_t *p;
        pi %= eng->npresets;
        p = eng_preset(eng, pi);
        t->preset = (uint8_t)pi;
        for (i = 0; i < 8u; ++i)
            t->p[P_E0 + i] = p->e[i];
        t->p[P_ATK] = p->env[0]; t->p[P_DEC] = p->env[1];
        t->p[P_SUS] = p->env[2]; t->p[P_REL] = p->env[3];
        t->p[P_ED_FLT] = p->fenv;
        t->p[P_VOICE] = p->mono ? V_LEGATO : V_POLY;
    }
    if (fade && drum && drum_layout(t) != drum) {   /* another DRUM layout under the hits: no fade */
        trk_all_off(t);
        memset(t->v, 0, sizeof t->v);
        t->xf_on = t->xf = 0;
    }
    if (fade && t->xf_on && t->engine == e)
        hooks_pending = 1;
    else
        load_hooks(t);
    if (fade)
        return;
    if (t->engine != t->eng_req)
        eng_state_clear(0);                 /* as voice.c engine_block on a switch */
    t->engine = t->eng_req;
    t->xf_on = t->xf = 0;
}

/* events_block lets go before it drains the queue: the note-ons still queued would start after the
 * release (a CC 123 or a MIDI CH change in the same pass), so they go with it */
static void melodee_release(void)
{
    uint32_t r, w;
    if (!ready)
        return;
    for (r = w = mi_r; r != mi_w; ++r) {
        uint32_t q = midi_in_q[r % MQ];
        if (((q >> 8) & 0xF0u) == 0x90u && q >> 24)
            continue;
        midi_in_q[w % MQ] = q;
        midi_in_ms[w % MQ] = midi_in_ms[r % MQ];
        midi_in_time[w % MQ] = midi_in_time[r % MQ];
        midi_in_source[w % MQ] = midi_in_source[r % MQ];
        ++w;
    }
    mi_w = w;
    panic_req |= (uint8_t)((1u << NTRK) - 1u);   /* events_block: every part lets go, the arp and the MIDI notes too */
}

static int melodee_init(uint32_t sample_rate)
{
    uint32_t i, k;
    if (sample_rate != FS)
        return -1;
    melodee_panic();
    memset(&song, 0, sizeof song);
    memset(trk, 0, sizeof trk);
    memset(&fm1_in, 0, sizeof fm1_in);
    memset(resource, 0, sizeof resource);
    rng_state = 0x1234567u;
    mod_seed = 0x2545F491u;
    vage = 0;
    rendered_frames = fm1_ms = 0;
    dsp_tables_init();                 /* the RAM lookup tables (FM6), before anything renders */
    cz_init();
    pattern_init();
    for (i = 0; i < G_COUNT; ++i)
        song.g[i] = GP[i].def;
    song.g[G_ROUTE] = 1;           /* SEL: every channel to the selected track (part 1); the port filters the channel */
    song.g[G_CLOCK] = 1;           /* USB clock: the FM-1's; falls back to the internal BPM without pulses */
    song.sel = 0;
    song.ntrk = 1;                 /* one part in use: the others cost nothing */
    song.master_q12 = 2048;        /* the same initial master gain as the device */
    tuning_a4 = A4_DEFAULT;
    for (k = 0; k < NTRK; ++k) {
        for (i = 0; i < P_E0; ++i)
            trk[k].p[i] = TP[i].def;
        steps_rest(&trk[k]);
    }
    fm6_init();
    ready = 1;
    load_preset(ENGINE_ORDER[0], 0, 0);
    melodee_panic();
    return 0;
}

static int melodee_midi(uint8_t status, uint8_t data1, uint8_t data2)
{
    uint32_t cin;
    if (!ready || status < 0x80 || data1 > 127 || data2 > 127)
        return -1;
    if ((status & 0xF0) == 0xC0)                   /* program change: the UI's business upstream */
        return -1;
    if ((status & 0xF0) == 0xB0 && data1 == 120) { melodee_panic(); return 0; }
    if (status >= 0xF0) {
        if (status == 0xFF) { melodee_panic(); return 0; }
        if (status != 0xF8 && status != 0xFA && status != 0xFB && status != 0xFC)
            return -1;
        cin = 0x0F;
    } else {
        if (port_midi_ch && (status & 0x0F) != (uint32_t)(port_midi_ch - 1))
            return 0;                              /* another track of the FM-1: not this instrument */
        if ((status & 0xF0) == 0xB0 && data1 >= 123) { melodee_release(); return 0; }   /* all notes off (and the modes) */
        cin = status >> 4;
    }
    if (mi_w - mi_r >= MQ) {
        midi_in_overflow = 1;
        melodee_panic();
        return -2;
    }
    if ((status & 0xF0) == 0x90 && data2)
        silenced = 0;
    midi_in_source[mi_w % MQ] = 1;
    midi_in_ms[mi_w % MQ] = fm1_ms;
    midi_in_time[mi_w % MQ] = 0;
    midi_in_q[mi_w++ % MQ] = cin | ((uint32_t)status << 8) | ((uint32_t)data1 << 16) | ((uint32_t)data2 << 24);
    return 0;
}

static void melodee_render(int16_t *stereo, size_t frames)
{
    size_t n, i;
    if (!stereo)
        return;
    if (!ready) {
        memset(stereo, 0, frames * 2u * sizeof *stereo);
        return;
    }
    while (frames) {
        if (pcm_pos == CTL) {
            int32_t block[2u * CTL];
            fm1_ms = (uint32_t)(rendered_frames * 1000u / FS);
            if (hooks_pending && (!trk[0].xf_on || !trk[0].xf))
                load_hooks(&trk[0]);                /* the fade ends in this block: the new patch first */
            if (trk[0].xf_on && trk[0].xf) {        /* a fade block: the old sound's own values */
                int16_t now[P_E0];
                memcpy(now, trk[0].p, sizeof now);
                memcpy(trk[0].p, fade_p, sizeof now);
                mix_block(block, CTL);
                for (i = 0; i < P_E0; ++i)          /* (a value MIDI set in the block stays) */
                    if (trk[0].p[i] == fade_p[i])
                        trk[0].p[i] = now[i];
            } else {
                mix_block(block, CTL);
            }
            for (i = 0; i < 2u * CTL; ++i)
                pcm[i] = silenced ? 0 : (int16_t)clamp(block[i], -32768, 32767);
            pcm_pos = 0;
            rendered_frames += CTL;
        }
        n = CTL - pcm_pos;
        if (n > frames)
            n = frames;
        memcpy(stereo, pcm + 2u * pcm_pos, n * 2u * sizeof *stereo);
        pcm_pos += (uint32_t)n;
        stereo += 2u * n;
        frames -= n;
    }
}

/* the engines as the FM-1 picks them: ENGINE_ORDER (the retired ones are not offered) */
static uint32_t melodee_engine_count(void) { return NENG_SHOWN; }
static uint32_t engine_of(uint32_t index) { return index < NENG_SHOWN ? ENGINE_ORDER[index] : NENGINES; }
static int melodee_set_preset(uint32_t engine, uint32_t preset)
{
    uint32_t e = engine_of(engine);
    if (!ready || e >= NENGINES || preset >= ENGINES[e]->npresets)
        return -1;
    load_preset(e, preset, 1);
    return 0;
}
static uint32_t melodee_preset_count(uint32_t engine)
{ uint32_t e = engine_of(engine); return e < NENGINES ? ENGINES[e]->npresets : 0; }
static const char *melodee_engine_name(uint32_t engine)
{ uint32_t e = engine_of(engine); return e < NENGINES ? ENGINES[e]->name : NULL; }
static const char *melodee_preset_name(uint32_t engine, uint32_t preset)
{
    uint32_t e = engine_of(engine);
    if (e >= NENGINES || preset >= ENGINES[e]->npresets) return NULL;
    return eng_preset(ENGINES[e], preset)->name;   /* (Prophet, CZ-1: made on demand, two held at once) */
}
static uint32_t melodee_current_engine(void) { return eng_rank(trk[0].eng_req); }
static uint32_t melodee_current_preset(void) { return trk[0].preset; }

/* Remote catalogue, beside the real descriptors: the part's sound, modulation, its FX slots and
 * the shared master slots. */
typedef struct { uint16_t first, last; const char *group; } remote_group_t;
#define RG(a, b, s) {a, b, s}
#define GG(a, b, s) {P_COUNT + a, P_COUNT + b, s}
static const remote_group_t remote_groups[] = {
    RG(P_E0, P_E7, "ENG"), RG(P_ATK, P_REL, "ENV"), RG(P_ED_FLT, P_ED_SHP, "ENV"),
    RG(P_LEVEL, P_LEVEL, "AMP"), RG(P_LRATE, P_LD_AMP, "LFO"), RG(P_LSYNC, P_LPOL, "LFO"),
    RG(P_FXTYPE, P_FXCHAR, "FX1"), RG(P_FXTONE, P_FXTONE, "FX1"),
    RG(P_FX2TYPE, P_FX2CHAR, "FX2"), RG(P_FX2TONE, P_FX2TONE, "FX2"),
    RG(P_FX3TYPE, P_FX3CHAR, "FX3"), RG(P_FX3TONE, P_FX3TONE, "FX3"), RG(P_FXSEND, P_FXSEND3, "SEND"),
    RG(P_VOICE, P_MUTE, "VOICE"), RG(P_SPRD, P_SPRD, "VOICE"),
    RG(P_AMODE, P_AORDER, "ARP"), RG(P_CHRD, P_VOIC, "CHORD"), RG(P_STRUM, P_STROKE, "CHORD"),
    RG(P_ROOT, P_TRANS, "SCALE"), RG(P_M1SRC, P_M4AMT, "MATRIX"), RG(P_LN0, P_LN7, "DRUM"),
    GG(G_BPM, G_SWING, "CLOCK"), GG(G_TUNE, G_TUNE, "MASTER"),
    GG(G_MFXTYPE, G_MFXCHAR, "MFX1"), GG(G_MFXTONE, G_MFXTONE, "MFX1"),
    GG(G_MFX2TYPE, G_MFX2CHAR, "MFX2"), GG(G_MFX2TONE, G_MFX2TONE, "MFX2"),
    GG(G_MFX3TYPE, G_MFX3CHAR, "MFX3"), GG(G_MFX3TONE, G_MFX3TONE, "MFX3"),
    GG(G_FXRETURN, G_FXMODE, "MFX")
};
#undef RG
#undef GG

static int melodee_set_midi_channel(uint32_t channel)
{
    if (channel > 16u) return -1;
    port_midi_ch = (int16_t)channel;
    return 0;
}
static uint32_t melodee_midi_channel(void) { return (uint32_t)port_midi_ch; }

static const param_desc_t *remote_desc(uint32_t index, int16_t **value, uint32_t *pid, const char **group)
{
    uint32_t pos = 1;                                /* position 0: the port's MIDI channel */
    if (index == 0) {
        *pid = PORT_MIDI_CH_ID; *group = "MIDI"; *value = &port_midi_ch;
        return &PORT_MIDI_CH_DESC;
    }
    for (uint32_t g = 0; g < NELEM(remote_groups); ++g) {
        for (uint32_t id = remote_groups[g].first; id <= remote_groups[g].last; ++id) {
            const param_desc_t *d = id < P_COUNT ? track_desc(&trk[0], id) : &GP[id - P_COUNT];
            if (!d || !d->label || !d->label[0] || d->label[0] == '-' || d->max <= d->min) continue;
            if (pos++ == index) {
                *pid = id; *group = remote_groups[g].group;
                *value = id < P_COUNT ? &trk[0].p[id] : &song.g[id - P_COUNT];
                return d;
            }
        }
    }
    *pid = pos;
    return NULL;
}
static uint32_t melodee_param_count(void)
{
    uint32_t n = 0; int16_t *v; const char *g;
    if (ready) remote_desc(UINT32_MAX, &v, &n, &g);
    return n;
}
static int melodee_param_get(uint32_t index, fm1_param_t *out)
{
    uint32_t id; int16_t *v; const char *group, *unit;
    const param_desc_t *d = ready ? remote_desc(index, &v, &id, &group) : NULL;
    if (!d || !out) return -1;
    memset(out, 0, sizeof *out);
    out->min = d->min; out->max = d->max; out->value = (int16_t)clamp(*v, d->min, d->max);
    str_cpy(out->name, group, sizeof out->name);
    uint32_t n = str_len(out->name);
    if (id >= P_M1SRC && id <= P_M4AMT) { fmt_int(out->name + n, (int32_t)((id - P_M1SRC) / 3u + 1u)); n = str_len(out->name); }
    out->name[n++] = ' '; out->name[n] = 0;
    str_cpy(out->name + n, d->label, sizeof out->name - n);
    param_format(d, out->value, out->text, &unit);
    /* The device's tiny columns abbreviate enums. Our full-width row needn't. */
    if (d->fmt == F_ENUM && d->names)
        str_cpy(out->text, d->names[out->value - d->min], sizeof out->text);
    str_cpy(out->unit, unit, sizeof out->unit);
    return 0;
}
static int melodee_param_set(uint32_t index, int32_t value)
{
    uint32_t id; int16_t *v; const char *group;
    const param_desc_t *d = ready ? remote_desc(index, &v, &id, &group) : NULL;
    if (!d || value < d->min || value > d->max) return -1;
    *v = (int16_t)value;
    return 0;
}

static const fm1_synth_api_t melodee_api = {
    .name = "MELODEE", .revision = "398ada1e09d864c12f3b778a43e45c7c2a8031b8",
    .init = melodee_init, .midi = melodee_midi, .render = melodee_render, .panic = melodee_panic,
    .release = melodee_release,
    .set_preset = melodee_set_preset, .engine_count = melodee_engine_count,
    .preset_count = melodee_preset_count, .engine_name = melodee_engine_name,
    .preset_name = melodee_preset_name, .current_engine = melodee_current_engine,
    .current_preset = melodee_current_preset, .param_count = melodee_param_count,
    .param_get = melodee_param_get, .param_set = melodee_param_set,
    .set_midi_channel = melodee_set_midi_channel, .midi_channel = melodee_midi_channel,
};
const fm1_synth_api_t *melodee_synth_api(void) { return &melodee_api; }
