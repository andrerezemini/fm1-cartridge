/* SPDX-License-Identifier: GPL-3.0-only
 * Portable adapter to the actual Clipper DSP, not an emulation of its sound.
 * Upstream sources retain their copyright and license notices. */
#include "clipper_synth.h"
#include <string.h>

#define CLIPPER_SYNTH 1
#define FELUCCA_SLICE 0
#define FELUCCA_ARRANGER 0

/* The upstream linker sections describe the AC7911 memory map. Preserve its
 * alignment/noinline attributes, but let the platform linker place this port. */
#define section(name)
#define bootguard clipper_unused_bootguard
#include "../third_party/clipper/generated/felucca_tables.h"
#ifdef memset
#undef memset
#endif
#ifdef memcpy
#undef memcpy
#endif
#define memset clipper_upstream_memset
#define memcpy clipper_upstream_memcpy
#define memcmp clipper_upstream_memcmp
#include "../third_party/clipper/firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp

/* All DSP calls are serialized by the caller. No hardware interrupt control,
 * physical keyboard, USB device driver or JieLi secondary core is used. */
static struct { uint32_t notes, buttons; } fm1_in;
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
#include "../third_party/clipper/firmware/src/core.h"
#include "../third_party/clipper/firmware/src/cpu1.h"
static int cpu1_run(cpu1_job_t fn, uint32_t arg) { (void)fn; (void)arg; return 0; }
static int cpu1_wait(void) { return 0; }
static int cpu1_on(void) { return 0; }
static uint32_t cpu1_ticks(void) { return 0; }
#include "../third_party/clipper/firmware/src/engines.c"
#include "../third_party/clipper/firmware/src/drums.c"
#include "../third_party/clipper/firmware/src/params.c"
#include "../third_party/clipper/firmware/src/voice.c"
#include "../third_party/clipper/firmware/src/slicer.c"
#include "../third_party/clipper/firmware/src/fx.c"

/* Match the queue consumed by upstream events_block(), without compiling its
 * USB device HAL. Clipper's outgoing MIDI is unused in this host cartridge. */
#define MQ 64u
static uint32_t midi_in_q[MQ], mi_w, mi_r;
static void midi_out_event(uint32_t packet) { (void)packet; }
#include "../third_party/clipper/firmware/src/seq.c"
/* UI-only descriptor lookup is intentionally unavailable in this headless port. */
static const param_desc_t *syn_desc(uint32_t id, int16_t **value)
{ (void)id; *value = NULL; return NULL; }
#undef section
#undef bootguard

/* WAVE's band-limited copies, built in bounded steps. Upstream's wt_service builds four frames a
 * main-loop pass on the FM-1, each frame seven inverse FFTs (1024 points down to 16); called from
 * every block of this real-time render it would build a whole table in one or two renders, tens of
 * ms against a 1.45 ms budget (64 frames). The same copies are made here bit for bit (upstream's load, transform
 * and rounding; the twiddles from a table instead of wt_sin30), butterfly by butterfly, at most
 * PORT_WT_BUDGET a 32-frame block. The WAVE voices stay silent until the table is complete, as
 * upstream's do while it builds (about 120 ms of audio here), and fade in after it (port_wt_hold). */
#define PORT_WT_BUDGET 1024u
static int32_t port_wt_cos[512], port_wt_sin[512];      /* wt_sin30(k + 256), wt_sin30(k): k < 512 */
static struct { uint8_t frame, level, stage; uint32_t len, i, j; } wtb;
static void port_wt_init(void)
{
    for (uint32_t k = 0; k < 512u; ++k) { port_wt_cos[k] = wt_sin30(k + 256u); port_wt_sin[k] = wt_sin30(k); }
}
/* one step of the current (frame, level): load the spectrum, bit-reverse, butterflies (resumable),
 * write the copy; returns the work done in butterflies (the other stages count a quarter point each) */
static uint32_t port_wt_step(uint32_t table, uint32_t budget)
{
    uint32_t l = wtb.level, N = 1024u >> l, H = 128u >> l, h0 = (H * 3u) / 5u, n, h, done = 0;
    int16_t *d = &wt_mip[wtb.frame][WT_OFF[l]];
    const int16_t (*sp)[2] = wt_spec(table, wtb.frame);
    if (!sp) {                                           /* a missing user table builds silence */
        for (n = 0; n < N; n++)
            d[n] = 0;
        wtb.stage = 4;
        return N / 4u;
    }
    switch (wtb.stage) {
    case 0:                                              /* the harmonics, tapered (wt_build_frame) */
        for (n = 0; n < N; n++)
            wt_fr[n] = wt_fi[n] = 0;
        for (h = 0; h < H; h++) {
            int32_t tap = h + 1u <= h0 ? 32767 : (32767 + SINE[(256u + (h + 1u - h0) * 512u / (H + 1u - h0)) & 1023u]) >> 1;
            int32_t a = (int32_t)(((int64_t)sp[h][0] * tap) >> 8), b = (int32_t)(((int64_t)sp[h][1] * tap) >> 8);
            wt_fr[h + 1u] = a, wt_fi[h + 1u] = -b;
            wt_fr[N - h - 1u] = a, wt_fi[N - h - 1u] = b;
        }
        wtb.stage = 1;
        return N / 4u;
    case 1: {                                            /* bit reversal (wt_ifft) */
        uint32_t i, k, bits = 0;
        while ((1u << bits) < N)
            bits++;
        for (i = 0; i < N; i++) {
            uint32_t r = 0, x = i;
            for (k = 0; k < bits; k++, x >>= 1)
                r = (r << 1) | (x & 1u);
            if (r > i) {
                int32_t t = wt_fr[i];
                wt_fr[i] = wt_fr[r], wt_fr[r] = t;
                t = wt_fi[i];
                wt_fi[i] = wt_fi[r], wt_fi[r] = t;
            }
        }
        wtb.stage = 2; wtb.len = 2; wtb.i = wtb.j = 0;
        return N / 4u;
    }
    case 2:                                              /* butterflies, e^(+i 2 pi j / len) (wt_ifft) */
        while (wtb.len <= N && done < budget) {
            uint32_t half = wtb.len >> 1, step = 1024u / wtb.len, j = wtb.j;
            int32_t wc = port_wt_cos[j * step], ws = port_wt_sin[j * step];
            int32_t *ar = &wt_fr[wtb.i + j], *ai = &wt_fi[wtb.i + j], *br = &wt_fr[wtb.i + j + half], *bi = &wt_fi[wtb.i + j + half];
            int32_t tr = (int32_t)(((int64_t)*br * wc - (int64_t)*bi * ws + (1 << 29)) >> 30);
            int32_t ti = (int32_t)(((int64_t)*br * ws + (int64_t)*bi * wc + (1 << 29)) >> 30);
            *br = *ar - tr, *bi = *ai - ti;
            *ar += tr, *ai += ti;
            done++;
            if (++wtb.j == half) {
                wtb.j = 0;
                if ((wtb.i += wtb.len) >= N) { wtb.i = 0; wtb.len <<= 1; }
            }
        }
        if (wtb.len > N)
            wtb.stage = 3;
        return done;
    default:                                             /* the copy, rounded from Q8 */
        for (n = 0; n < N; n++)
            d[n] = (int16_t)clamp((wt_fr[n] + 128) >> 8, -32767, 32767);
        wtb.stage = 4;
        return N / 4u;
    }
}
/* the table being built, on by at most budget butterflies */
static void port_wt_build(uint32_t budget)
{
    uint32_t done = 0;
    while (wt_building >= 0 && done < budget) {
        done += port_wt_step((uint32_t)wt_building, budget - done);
        if (wtb.stage == 4) {
            wtb.stage = 0;
            if (++wtb.level == WT_LEVELS) {
                wtb.level = 0;
                if (++wtb.frame == WT_NF) {
                    wt_have = wt_building;
                    wt_building = -1;
                }
            }
        }
    }
}
/* in place of upstream's wt_service (eng_wave.c), once a 32-frame block */
static void port_wt_service(uint32_t budget)
{
    int32_t want = wt_wanted();
    if (want < 0)
        return;
    if (want != wt_have && want != wt_building) {        /* a new table: start over */
        wt_have = -1;
        wt_building = (int16_t)want;
        memset(&wtb, 0, sizeof wtb);
    }
    port_wt_build(budget);
}
/* A WAVE part waits for its table held silent by upstream's duck (fx.c part_duck), which gives up after
 * 68 blocks (on the FM-1 a table lands in a few ms); the build here takes longer, so the hold is renewed
 * while it runs, and the duck's own fade-in brings the voices in once the table has landed (not a step
 * from silence to their level) */
static void port_wt_hold(void)
{
    uint32_t k;
    if (wt_building < 0)
        return;
    for (k = 0; k < NPART; ++k)
        if (ENGINES[trk[k].engine % NENGINES] == &ENG_WAVE && trk[k].dk_phase != 1u)
            trk[k].dk_phase = 2u, trk[k].dk_hold = 0;
}
/* Native check of the builder against upstream's: table k complete from port_wt_service, then each
 * frame again with wt_build_frame; 0 when every copy is the same (tests/test_clipper.c). */
int clipper_synth_wavetable_check(uint32_t table)
{
    static int16_t frame[WT_PTS];
    uint32_t f, blocks = 0;
    if (table >= WT_NFACTORY)
        return -1;
    wt_have = wt_building = -1;
    memset(&wtb, 0, sizeof wtb);
    wt_building = (int16_t)table;
    while (wt_have != (int16_t)table) {
        if (blocks++ > 100000u || wt_building != (int16_t)table)
            return -1;
        port_wt_build(PORT_WT_BUDGET);
    }
    for (f = 0; f < WT_NF; ++f) {
        memcpy(frame, wt_mip[f], sizeof frame);
        wt_build_frame(table, f);
        if (memcmp(frame, wt_mip[f], sizeof frame))
            return -1;
    }
    wt_have = -1;
    return (int)blocks;
}

static int ready, silenced = 1;
/* The port's own setting, as upstream GLO > MIDI > IN CH (seq.c midi_in_ch): the channel the
 * instrument answers, 0 OMNI or 1..16. First entry of the remote catalogue ("MIDI CH"), so the
 * FM-1's other tracks (their own channels) do not play the cartridge unless asked to. */
static int16_t port_midi_ch;
static const char *const PORT_N_MCH[] = {"OMNI", "1", "2", "3", "4", "5", "6", "7", "8",
                                         "9", "10", "11", "12", "13", "14", "15", "16"};
static const param_desc_t PORT_MIDI_CH_DESC = {"CH", F_ENUM, 0, 16, 0, PORT_N_MCH, 0};
#define PORT_MIDI_CH_ID (P_COUNT + G_COUNT)
static int16_t pcm[2u * CTL];
static uint32_t pcm_pos = CTL;
static uint64_t rendered_frames;

static void clear_effects(void)
{
    memset(dly_buf, 0, sizeof dly_buf);
    memset(cho_buf, 0, sizeof cho_buf);
    memset(rev_line, 0, sizeof rev_line);
    memset(rev_ap, 0, sizeof rev_ap);
    memset(&fx, 0, sizeof fx);
    plate_clear();
    memset(ps_buf, 0, sizeof ps_buf);
    memset(&ps, 0, sizeof ps);
    memset(ins, 0, sizeof ins);
    memset(jc, 0, sizeof jc);
    memset(tflt, 0, sizeof tflt);
    memset(&djf, 0, sizeof djf);
    memset(&bus, 0, sizeof bus);
    bus.gc = 16384;
    memset(&lim_dac, 0, sizeof lim_dac);
    memset(&lim_usb, 0, sizeof lim_usb);
    lim_dac.env = lim_usb.env = LIM_T;
    lim_dac.g = lim_usb.g = 32768;
    lc_l1 = lc_l2 = lc_r1 = lc_r2 = dc_l = dc_r = dce_l = dce_r = 0;
    memset(lce, 0, sizeof lce);
    memset(&uo, 0, sizeof uo);
    master_cur = -1;
}

void clipper_synth_panic(void)
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
        t->nheld = t->arp_phys = t->nmono = t->xp_n = 0;
        t->arp_note = t->arp_ch[0] = t->arp_ch[1] = t->arp_ch[2] = 0;
        t->xf_on = t->xf = 0;
    }
    memset(stq, 0, sizeof stq);
    memset(kh_set, 0, sizeof kh_set);
    memset(mi_pen, 0, sizeof mi_pen);
    memset(ly_route, 0, sizeof ly_route);
    memset(ly_note, 0, sizeof ly_note);
    memset(&mclk, 0, sizeof mclk);
    memset(eng_arena_own, 0, sizeof eng_arena_own);
    part_ctl_reset();
    transport_req = panic_req = arp_keep_req = 0;
    song.playing = song.rec = 0;
    clear_effects();
}

/* a factory sound into track 1. fade: what sounds fades out over XF_BLOCKS with its own values and the
 * engine follows after it (voice.c sound_fade_begin, engine_block; events_block releases the track:
 * panic_req), as the upstream UI loads a preset; else the track starts from nothing (init) */
static void load_preset(uint32_t engine, uint32_t preset, int fade)
{
    track_t *t = &trk[0];
    const preset_t *p = &ENGINES[engine]->presets[preset];
    static const uint8_t fx_default[4] = {0, 24, 28, 36};
    uint32_t i;
    if (fade) {
        sound_fade_begin(t);
        panic_req |= 1u;
    } else {
        memset(t, 0, sizeof *t);
        steps_clear(t);
        t->engine = (uint8_t)engine;
    }
    for (i = 0; i < P_E0; ++i)
        t->p[i] = TP[i].def;
    t->eng_req = (uint8_t)engine;
    t->preset = (uint8_t)preset;
    for (i = 0; i < 8; ++i)
        t->p[P_E0 + i] = p->e[i];
    t->p[P_ATK] = p->env[0]; t->p[P_DEC] = p->env[1];
    t->p[P_SUS] = p->env[2]; t->p[P_REL] = p->env[3];
    t->p[P_ED_FLT] = p->fenv;
    t->p[P_ED_FX] = preset_trim(engine, preset);
    t->p[P_VOICE] = p->mono ? V_LEGATO : V_POLY;
    for (i = 0; i < 4; ++i) {
        t->p[P_DIST + i] = (int16_t)(p->fx[i] ? p->fx[i] - 1 : fx_default[i]);
        t->p[P_AMODE + i] = (int16_t)(p->arp[i] ? p->arp[i] - 1 : TP[P_AMODE + i].def);
    }
    /* Clipper 1.0 ui.c also loads factory sounds with delay/reverb dry. */
    t->p[P_DLY] = t->p[P_REV] = 0;
    preset_extras(t->p, p);
    mseq_init(t);
    fm6_track_loaded(t);
    arp_snd[0] = (uint8_t)t->p[P_AMODE];
}

int clipper_synth_init(uint32_t sample_rate)
{
    uint32_t i, k;
    if (sample_rate != FS)
        return -1;
    clipper_synth_panic();
    memset(&song, 0, sizeof song);
    memset(trk, 0, sizeof trk);
    memset(&fm1_in, 0, sizeof fm1_in);
    memset(part_ctl, 0, sizeof part_ctl);
    memset(eng_arena_own, 0, sizeof eng_arena_own);
    memset(wt_tab_eff, 0, sizeof wt_tab_eff);
    wt_have = wt_building = -1;
    wt_next = 0;
    memset(&wtb, 0, sizeof wtb);
    port_wt_init();
    rng_state = 0x1234567u;
    vage = 0;
    clk_beat = clk_pos = 0;
    rendered_frames = fm1_ms = 0;
    midi_bank = midi_bend = midi_breath = midi_pc_req = 0;
    midi_out_ch = 0;
    midi_in_ch = (uint8_t)port_midi_ch;
    midi_rpn[0] = midi_rpn[1] = 127;
    cpu1_units = 0;
    for (i = 0; i < G_COUNT; ++i)
        song.g[i] = GP[i].def;
    song.g[G_SYNC] = 1; /* USB clock; falls back to internal BPM without pulses. */
    song.master_q12 = 2048; /* Same initial master gain as the device. */
    for (k = 0; k < NTRK; ++k) {
        for (i = 0; i < P_E0; ++i)
            trk[k].p[i] = TP[i].def;
        steps_clear(&trk[k]);
    }
    fm6_init();
    ready = 1;
    load_preset(ENGI_HYBRID, 0, 0);
    clipper_synth_panic();
    return 0;
}

int clipper_synth_midi(uint8_t status, uint8_t data1, uint8_t data2)
{
    uint32_t cin;
    if (!ready || status < 0x80 || data1 > 127 || data2 > 127)
        return -1;
    /* Their actions live in the upstream UI, which this DSP adapter omits. */
    if ((status & 0xF0) == 0xC0 || ((status & 0xF0) == 0xB0 && data1 == 20))
        return -1;
    if ((status & 0xF0) == 0xB0 && data1 == 120) {
        clipper_synth_panic();
        return 0;
    }
    if (status >= 0xF0) {
        if (status == 0xFF) { clipper_synth_panic(); return 0; }
        if (status != 0xF8 && status != 0xFA && status != 0xFB && status != 0xFC)
            return -1;
        cin = 0x0F;
    } else {
        cin = status >> 4;
    }
    if (mi_w - mi_r >= MQ) {
        clipper_synth_panic();
        return -2;
    }
    if ((status & 0xF0) == 0x90 && data2)
        silenced = 0;
    midi_in_q[mi_w++ % MQ] = cin | ((uint32_t)status << 8) |
        ((uint32_t)data1 << 16) | ((uint32_t)data2 << 24);
    return 0;
}

void clipper_synth_render(int16_t *stereo, size_t frames)
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
            port_wt_service(PORT_WT_BUDGET);
            port_wt_hold();
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

int clipper_synth_set_preset(uint32_t engine, uint32_t preset)
{
    if (!ready || engine >= NENGINES || preset >= ENGINES[engine]->npresets)
        return -1;
    load_preset(engine, preset, 1);
    return 0;
}

/* events_block lets go before it drains the queue: the note-ons still queued would start after the
 * release (a MIDI CH change in the same pass), so they go with it */
void clipper_synth_release(void)
{
    uint32_t r, w;
    if (!ready)
        return;
    for (r = w = mi_r; r != mi_w; ++r) {
        uint32_t q = midi_in_q[r % MQ];
        if (((q >> 8) & 0xF0u) == 0x90u && q >> 24)
            continue;
        midi_in_q[w++ % MQ] = q;
    }
    mi_w = w;
    panic_req |= (uint8_t)((1u << NTRK) - 1u);   /* events_block: every track lets go, KBD HOLD and the arp too */
}
uint32_t clipper_synth_engine_count(void) { return NENGINES; }
uint32_t clipper_synth_preset_count(uint32_t engine)
{ return engine < NENGINES ? ENGINES[engine]->npresets : 0; }
const char *clipper_synth_engine_name(uint32_t engine)
{ return engine < NENGINES ? ENGINES[engine]->name : NULL; }
const char *clipper_synth_preset_name(uint32_t engine, uint32_t preset)
{ return engine < NENGINES && preset < ENGINES[engine]->npresets ? ENGINES[engine]->presets[preset].name : NULL; }
const char *clipper_synth_upstream_revision(void)
{ return "e69067211c4690f34e494b5b896e4585163dcc2b"; }

/* Remote catalogue is owned here, beside the real descriptors. No engine or
 * preset table is duplicated in the FM-1's very small flash slot. */
typedef struct { uint16_t first, last; const char *group; } remote_group_t;
#define RG(a, b, s) {a, b, s}
#define GG(a, b, s) {P_COUNT + a, P_COUNT + b, s}
static const remote_group_t remote_groups[] = {
    RG(P_E0, P_E7, "ENG"), RG(P_MAC1, P_MAC4, "MACRO"),
    RG(P_ATK, P_REL, "ENV"), RG(P_ED_FLT, P_ED_FX, "ENV"),
    RG(P_LEVEL, P_LEVEL, "AMP"), RG(P_TFLT, P_TFLT, "TRACK"),
    RG(P_FMODEL, P_FMODEL, "FILTER"), RG(P_VELF, P_VELF, "MOD"),
    RG(P_MWDST, P_ATDST, "MOD"), RG(P_LRATE, P_LD_AMP, "LFO1"),
    RG(P_L2RATE, P_L2MODE, "LFO2"), RG(P_L3RATE, P_L3MODE, "LFO3"),
    RG(P_E2ATK, P_E2REL, "ENV2"), RG(P_E2LOOP, P_E2LOOP, "ENV2"),
    RG(P_DIST, P_REV, "FX"), RG(P_ITYPE, P_IMIX, "INSERT"),
    RG(P_VOICE, P_PAN, "VOICE"), RG(P_GLMODE, P_DETUNE, "VOICE"),
    RG(P_VINT, P_VINT, "VOICE"), RG(P_SPREAD, P_SPREAD, "VOICE"),
    RG(P_AMODE, P_AORDER, "ARP"), RG(P_SPICE, P_DICE, "ARP"), RG(P_SLOOP, P_SLOOP, "ARP"),
    RG(P_M1SRC, P_M6AMT, "MATRIX"), RG(P_M7SRC, P_M12AMT, "MATRIX"),
    GG(G_BPM, G_BPM, "CLOCK"), GG(G_TUNE, G_TUNE, "MASTER"),
    GG(G_DTIME, G_DMIX, "DELAY"), GG(G_DMODE, G_DMODE, "DELAY"),
    GG(G_RTYPE, G_RTYPE, "REVERB"), GG(G_RSIZE, G_RDAMP, "REVERB"), GG(G_RPRE, G_RPRE, "REVERB"),
    GG(G_CRATE, G_CDEPTH, "CHORUS"), GG(G_EQLOW, G_EQLOW, "EQ"),
    GG(G_EQMID, G_EQMID, "EQ"), GG(G_EQHIGH, G_EQHIGH, "EQ"),
    GG(G_COMP, G_COMP, "MASTER"), GG(G_FILT, G_FILT, "MASTER")
};
#undef RG
#undef GG

int clipper_synth_set_midi_channel(uint32_t channel)
{
    if (channel > 16u) return -1;
    port_midi_ch = (int16_t)channel;
    midi_in_ch = (uint8_t)channel;
    return 0;
}
uint32_t clipper_synth_midi_channel(void) { return (uint32_t)port_midi_ch; }

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
uint32_t clipper_synth_current_engine(void) { return trk[0].eng_req; }
uint32_t clipper_synth_current_preset(void) { return trk[0].preset; }
uint32_t clipper_synth_param_count(void)
{
    uint32_t n = 0; int16_t *v; const char *g;
    if (ready) remote_desc(UINT32_MAX, &v, &n, &g);
    return n;
}
int clipper_synth_param_get(uint32_t index, clipper_param_t *out)
{
    uint32_t id; int16_t *v; const char *group, *unit;
    const param_desc_t *d = ready ? remote_desc(index, &v, &id, &group) : NULL;
    if (!d || !out) return -1;
    memset(out, 0, sizeof *out);
    out->min = d->min; out->max = d->max; out->value = (int16_t)clamp(*v, d->min, d->max);
    str_cpy(out->name, group, sizeof out->name);
    uint32_t field, n = str_len(out->name);
    int slot = id < P_COUNT ? mx_slot_of(id, &field) : -1;
    if (slot >= 0) { fmt_int(out->name + n, slot + 1); n = str_len(out->name); }
    out->name[n++] = ' '; out->name[n] = 0;
    str_cpy(out->name + n, d->label, sizeof out->name - n);
    /* FMX's fixed-size slot-name table has no NULL sentinel, although its
     * upstream descriptor uses F_INT (whose formatter scans for one). Treat
     * that table as the direct enum it actually is, without editing vendor. */
    param_desc_t display = *d;
    if (display.names == N_FMX_PATCH || display.names == N_FMX_ALG) display.fmt = F_ENUM;
    param_format(&display, out->value, out->text, &unit);
    /* The device's tiny columns abbreviate enums. Our full-width row needn't. */
    if (display.fmt == F_ENUM && d->names)
        str_cpy(out->text, d->names[out->value - d->min], sizeof out->text);
    str_cpy(out->unit, unit, sizeof out->unit);
    return 0;
}
int clipper_synth_param_set(uint32_t index, int32_t value)
{
    uint32_t id; int16_t *v; const char *group;
    const param_desc_t *d = ready ? remote_desc(index, &v, &id, &group) : NULL;
    if (!d || value < d->min || value > d->max) return -1;
    *v = (int16_t)value;
    if (v == &port_midi_ch) midi_in_ch = (uint8_t)value;
    return 0;
}

static const fm1_synth_api_t clipper_api = {
    .name = "CLIPPER", .revision = "e69067211c4690f34e494b5b896e4585163dcc2b",
    .init = clipper_synth_init, .midi = clipper_synth_midi, .render = clipper_synth_render,
    .panic = clipper_synth_panic, .release = clipper_synth_release, .set_preset = clipper_synth_set_preset,
    .engine_count = clipper_synth_engine_count, .preset_count = clipper_synth_preset_count,
    .engine_name = clipper_synth_engine_name, .preset_name = clipper_synth_preset_name,
    .current_engine = clipper_synth_current_engine, .current_preset = clipper_synth_current_preset,
    .param_count = clipper_synth_param_count, .param_get = clipper_synth_param_get,
    .param_set = clipper_synth_param_set, .set_midi_channel = clipper_synth_set_midi_channel,
    .midi_channel = clipper_synth_midi_channel,
};
const fm1_synth_api_t *clipper_synth_api(void) { return &clipper_api; }
