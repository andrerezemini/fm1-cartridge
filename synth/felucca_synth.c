/* SPDX-License-Identifier: GPL-3.0-only
 * Portable adapter to the actual Felucca DSP (hugelton/Felucca 1.6.1), not an emulation of its
 * sound. Upstream sources retain their copyright and license notices. The include order is the
 * one of tests/hostsim.c upstream: the DSP, without the USB device, the UI, storage or the HAL. */
#include "felucca_synth.h"
#include <string.h>

#define FELUCCA_CPU2 0           /* one core renders every part (the AC7911's second core is not here) */
#define FELUCCA_FM4 0            /* DIGITAL retired upstream: its sounds play as FM6 */
#define FELUCCA_SLICE 1

/* The upstream linker sections describe the AC7911 memory map. Preserve the alignment/noinline
 * attributes, but let the platform linker place this port (felucca.lf on the ESP32). */
#define section(name)
#define bootguard felucca_unused_bootguard
#include "../third_party/felucca/generated/felucca_tables.h"
#ifdef memset
#undef memset
#endif
#ifdef memcpy
#undef memcpy
#endif
#define memset felucca_upstream_memset
#define memcpy felucca_upstream_memcpy
#define memcmp felucca_upstream_memcmp
#include "../third_party/felucca/firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp

/* All DSP calls are serialized by the caller. No physical keyboard, buttons, interrupt control or
 * USB device driver: the keys stay up, the USB-MIDI queue is fed by felucca_synth_midi. */
static struct { volatile uint32_t notes, buttons; } fm1_in;
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
#include "../third_party/felucca/firmware/src/core.h"
#include "../third_party/felucca/firmware/src/engines.c"
#include "../third_party/felucca/firmware/src/params.c"
#include "../third_party/felucca/firmware/src/mod.c"
#include "../third_party/felucca/firmware/src/voice.c"
#include "../third_party/felucca/firmware/src/slicer.c"
#include "../third_party/felucca/firmware/src/fx.c"
/* the queue upstream usb.c / midi_uart.c fill and seq.c events_block drains (source 1: USB) */
#define MQ 64u
static uint32_t midi_in_q[MQ], midi_in_ms[MQ];
static uint8_t midi_in_source[MQ];
static volatile uint32_t mi_w, mi_r;
static volatile uint8_t midi_in_overflow;    /* set by the port when its queue overflows: events_block releases every note */
static void midi_out_event(uint32_t packet) { (void)packet; }   /* the FM-1 is the host: no MIDI out */
#include "../third_party/felucca/firmware/src/song_chain.c"
#include "../third_party/felucca/firmware/src/seq.c"
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
    memset(dly_buf, 0, sizeof dly_buf);
    memset(cho_buf, 0, sizeof cho_buf);
    memset(rev_comb, 0, sizeof rev_comb);
    memset(&rev_u, 0, sizeof rev_u);
    memset(&fx, 0, sizeof fx);
    memset(&hl, 0, sizeof hl);
    memset(ins, 0, sizeof ins);
    memset(sl, 0, sizeof sl);
    memset(sl_buf, 0, sizeof sl_buf);
    memset(lce, 0, sizeof lce);
    memset(fl_buf, 0, sizeof fl_buf);
    memset(&pf, 0, sizeof pf);
    lim_env = LIM_T;
    midi_beat_samples = 0;
    dc_l = dc_r = dce_l = dce_r = 0;      /* the master's DC blocker (fx.c master_out) */
    beat_pos = beat_n = 0;
}

static void felucca_panic(void)
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
        t->arp_note = t->arp_ch[0] = t->arp_ch[1] = t->arp_ch[2] = 0;
        t->xf_on = t->xf = 0;
        t->seq_n = 0;
        t->mw = t->at = t->ex_off = 0;
    }
    memset(midi_ch, 0, sizeof midi_ch);
    memset(midi_sel_on, 0, sizeof midi_sel_on);
    memset(midi_owners, 0, sizeof midi_owners);
    memset(midi_bend_target, 0, sizeof midi_bend_target);
    memset(&midi_clock, 0, sizeof midi_clock);
    transport_req = panic_req = 0;
    midi_in_overflow = 0;
    song.playing = song.rec = 0;
    clear_effects();
}

/* preset pi of upstream engine e into part 1, as ui.c apply_preset_to and set_engine_of: the sound
 * and its sends; not the steps, the arp or the mix of the part (hostsim host_preset) */
static void load_preset(uint32_t e, uint32_t pi)
{
    track_t *t = &trk[0];
    const engine_t *eng = ENGINES[e];
    const preset_t *p;
    static const uint8_t FX_DEF[4] = {0, 24, 28, 36};
    uint32_t i;
    if (e == ENGI_SAMPLE && pi == SMP_SET_PERC) { e = ENGI_DRUM; pi = 0; eng = ENGINES[e]; }   /* (retired PERC: the kit) */
    trk_all_off(t);
    memset(t->v, 0, sizeof t->v);
    for (i = 0; i < P_E0; ++i)
        t->p[i] = TP[i].def;
    t->user = 0;
    t->eng_req = (uint8_t)e;
    if (!eng->npresets) {
        t->preset = 0;
        for (i = 0; i < 8u; ++i)
            t->p[P_E0 + i] = eng->edit[i].def;
    } else {
        pi %= eng->npresets;
        p = &eng->presets[pi];
        t->preset = (uint8_t)pi;
        for (i = 0; i < 8u; ++i)
            t->p[P_E0 + i] = p->e[i];
        t->p[P_ATK] = p->env[0]; t->p[P_DEC] = p->env[1];
        t->p[P_SUS] = p->env[2]; t->p[P_REL] = p->env[3];
        t->p[P_ED_FLT] = p->fenv;
        t->p[P_VOICE] = p->mono ? V_LEGATO : V_POLY;
        for (i = 0; i < 4u; ++i)
            t->p[P_DIST + i] = (int16_t)(p->fx[i] ? p->fx[i] - 1 : FX_DEF[i]);
    }
    fm6_track_loaded(t);
    t->engine = t->eng_req;
    t->xf_on = t->xf = 0;
    eng_mem_claim(t);
}

static int felucca_init(uint32_t sample_rate)
{
    uint32_t i, k;
    if (sample_rate != FS)
        return -1;
    felucca_panic();
    memset(&song, 0, sizeof song);
    memset(trk, 0, sizeof trk);
    memset(&fm1_in, 0, sizeof fm1_in);
    memset(eng_mem, 0, sizeof eng_mem);
    memset(eng_mem_owner, 0, sizeof eng_mem_owner);
    memset(&mod, 0, sizeof mod);
    memset(esync_k, 0, sizeof esync_k);
    memset(spread_buf, 0, sizeof spread_buf);
    memset(kb_note, 0, sizeof kb_note);
    rng_state = 0x1234567u;
    mod_seed = 0x2545F491u;
    vage = 0;
    rendered_frames = fm1_ms = 0;
    for (i = 0; i < G_COUNT; ++i)
        song.g[i] = GP[i].def;
    song.g[G_ROUTE] = ROUTE_SEL;   /* every channel to the selected track (part 1); the port filters the channel */
    song.g[G_CLOCK] = 1;           /* USB clock: the FM-1's; falls back to the internal BPM without pulses */
    song.sel = 0;
    song.master_q12 = 2048;        /* the same initial master gain as the device */
    for (k = 0; k < NTRK; ++k) {
        for (i = 0; i < P_E0; ++i)
            trk[k].p[i] = TP[i].def;
        steps_rest(&trk[k]);
    }
    fm6_init();
    ready = 1;
    load_preset(0, 0);
    felucca_panic();
    return 0;
}

static int felucca_midi(uint8_t status, uint8_t data1, uint8_t data2)
{
    uint32_t cin;
    if (!ready || status < 0x80 || data1 > 127 || data2 > 127)
        return -1;
    if ((status & 0xF0) == 0xC0)                   /* program change: the UI's business upstream */
        return -1;
    if ((status & 0xF0) == 0xB0 && data1 == 120) { felucca_panic(); return 0; }
    if (status >= 0xF0) {
        if (status == 0xFF) { felucca_panic(); return 0; }
        if (status != 0xF8 && status != 0xFA && status != 0xFB && status != 0xFC)
            return -1;
        cin = 0x0F;
    } else {
        if (port_midi_ch && (status & 0x0F) != (uint32_t)(port_midi_ch - 1))
            return 0;                              /* another track of the FM-1: not this instrument */
        cin = status >> 4;
    }
    if (mi_w - mi_r >= MQ) {
        midi_in_overflow = 1;
        felucca_panic();
        return -2;
    }
    if ((status & 0xF0) == 0x90 && data2)
        silenced = 0;
    midi_in_source[mi_w % MQ] = 1;
    midi_in_ms[mi_w % MQ] = fm1_ms;
    midi_in_q[mi_w++ % MQ] = cin | ((uint32_t)status << 8) | ((uint32_t)data1 << 16) | ((uint32_t)data2 << 24);
    return 0;
}

static void felucca_render(int16_t *stereo, size_t frames)
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
            fm6_poll();
            mix_block(block, CTL);
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

/* the engines as the FM-1 picks them: ENGINE_ORDER (DIGITAL, retired, is not offered) */
static uint32_t felucca_engine_count(void) { return NENG_SHOWN; }
static uint32_t engine_of(uint32_t index) { return index < NENG_SHOWN ? ENGINE_ORDER[index] : NENGINES; }
static int felucca_set_preset(uint32_t engine, uint32_t preset)
{
    uint32_t e = engine_of(engine);
    if (!ready || e >= NENGINES || preset >= ENGINES[e]->npresets)
        return -1;
    felucca_panic();
    load_preset(e, preset);
    return 0;
}
static uint32_t felucca_preset_count(uint32_t engine)
{ uint32_t e = engine_of(engine); return e < NENGINES ? ENGINES[e]->npresets : 0; }
static const char *felucca_engine_name(uint32_t engine)
{ uint32_t e = engine_of(engine); return e < NENGINES ? ENGINES[e]->name : NULL; }
static const char *felucca_preset_name(uint32_t engine, uint32_t preset)
{
    uint32_t e = engine_of(engine);
    return e < NENGINES && preset < ENGINES[e]->npresets ? ENGINES[e]->presets[preset].name : NULL;
}
static uint32_t felucca_current_engine(void) { return eng_rank(trk[0].eng_req); }
static uint32_t felucca_current_preset(void) { return trk[0].preset; }

/* Remote catalogue, beside the real descriptors: the part's sound, modulation and the shared FX. */
typedef struct { uint16_t first, last; const char *group; } remote_group_t;
#define RG(a, b, s) {a, b, s}
#define GG(a, b, s) {P_COUNT + a, P_COUNT + b, s}
static const remote_group_t remote_groups[] = {
    RG(P_E0, P_E7, "ENG"), RG(P_ATK, P_REL, "ENV"), RG(P_ESYNC, P_ESYNC, "ENV"),
    RG(P_ED_FLT, P_ED_SHP, "ENV"), RG(P_LEVEL, P_LEVEL, "AMP"), RG(P_FTYPE, P_FTYPE, "FILTER"),
    RG(P_LRATE, P_LD_AMP, "LFO"), RG(P_LSYNC, P_LPOL, "LFO"),
    RG(P_DIST, P_REV, "FX"), RG(P_ITYPE, P_IMIX, "INSERT"),
    RG(P_VOICE, P_DETUNE, "VOICE"), RG(P_SPRD, P_SPRD, "VOICE"),
    RG(P_AMODE, P_AORDER, "ARP"), RG(P_CHRD, P_VOIC, "CHORD"), RG(P_ROOT, P_TRANS, "SCALE"),
    RG(P_M1SRC, P_M4AMT, "MATRIX"), RG(P_LN0, P_LN7, "DRUM"), RG(P_SLCR, P_SLDEPTH, "SLICER"),
    GG(G_BPM, G_SWING, "CLOCK"), GG(G_TUNE, G_TUNE, "MASTER"),
    GG(G_DTIME, G_DMIX, "DELAY"), GG(G_RTYPE, G_RTYPE, "REVERB"), GG(G_RSIZE, G_RDAMP, "REVERB"),
    GG(G_CRATE, G_CDEPTH, "CHORUS")
};
#undef RG
#undef GG

static int felucca_set_midi_channel(uint32_t channel)
{
    if (channel > 16u) return -1;
    port_midi_ch = (int16_t)channel;
    return 0;
}
static uint32_t felucca_midi_channel(void) { return (uint32_t)port_midi_ch; }

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
            if (!d->label || !d->label[0] || d->label[0] == '-' || d->max <= d->min) continue;
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
static uint32_t felucca_param_count(void)
{
    uint32_t n = 0; int16_t *v; const char *g;
    if (ready) remote_desc(UINT32_MAX, &v, &n, &g);
    return n;
}
static int felucca_param_get(uint32_t index, fm1_param_t *out)
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
static int felucca_param_set(uint32_t index, int32_t value)
{
    uint32_t id; int16_t *v; const char *group;
    const param_desc_t *d = ready ? remote_desc(index, &v, &id, &group) : NULL;
    if (!d || value < d->min || value > d->max) return -1;
    *v = (int16_t)value;
    return 0;
}

static const fm1_synth_api_t felucca_api = {
    .name = "FELUCCA", .revision = "63f88ec18b71c6ed4371d03d6f04dd93f4193ad3",
    .init = felucca_init, .midi = felucca_midi, .render = felucca_render, .panic = felucca_panic,
    .set_preset = felucca_set_preset, .engine_count = felucca_engine_count,
    .preset_count = felucca_preset_count, .engine_name = felucca_engine_name,
    .preset_name = felucca_preset_name, .current_engine = felucca_current_engine,
    .current_preset = felucca_current_preset, .param_count = felucca_param_count,
    .param_get = felucca_param_get, .param_set = felucca_param_set,
    .set_midi_channel = felucca_set_midi_channel, .midi_channel = felucca_midi_channel,
};
const fm1_synth_api_t *felucca_synth_api(void) { return &felucca_api; }
