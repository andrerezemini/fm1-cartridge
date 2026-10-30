/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio)
 * Portions Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * SLOOP insert adaptation by isod89, d30d655 (after Felucca 1.5).
 * Ported from isod89/sloop-fm1 fx.c at 4ec1ac829fd2bdf2c571ea215fb3d62477d01220.
 * Melodee stereo/arena/return integration and bounded arithmetic by Kerem Kilic. */
enum { IT_OFF, IT_SOFT, IT_HARD, IT_FOLD, IT_FUZZ, IT_CRUSH, IT_PHASER, IT_FLANGER, IT_CHORUS, IT_N };
enum { SI_TYPE, SI_A, SI_B, SI_C };
#define IL_LEN 256u                     /* FLANG / CHOR: the line, samples (a power of 2: masked) */
#define PH_ST 4u                        /* PHASR: all-pass stages (two notches, the classic pedal's) */
static const uint8_t INS_HOLD[16] __attribute__((section(".data.fx_sloop"))) = {64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};   /* CRUSH RATE */
/* PHASR: the stages' coefficient over the sweep (Q14), 33 points */
static const int16_t PH_A[33] __attribute__((section(".data.fx_sloop"))) = {
    -16038, -15998, -15955, -15906, -15853, -15793, -15727, -15653, -15572, -15481, -15381, -15270, -15147, -15012,
    -14862, -14696, -14514, -14313, -14092, -13849, -13582, -13290, -12970, -12621, -12240, -11825, -11375, -10886,
    -10356, -9784, -9166, -8500, -7783,
};
typedef struct {
    uint8_t type;                       /* the type running */
    uint8_t cnt;                        /* CRUSH: samples left of the held one */
    uint16_t wp;                        /* FLANG / CHOR: the line's next write */
    uint32_t ph;                        /* the sweep's phase */
    int32_t hp, lp1, lp2, dc, hold;     /* low cut (Q8), the two low-passes, DC blocker (Q8), CRUSH's held sample */
    int32_t px[PH_ST], py[PH_ST], fb;   /* PHASR: each stage's last input and output, the fed back */
    int16_t *line;               /* FLANG / CHOR, at a quarter (headroom) */
} ins_t;
/* Audio-owned state comes from the existing arena. */
static inline uint32_t ins_tri(uint32_t q) { uint32_t t = q >> 16; return t < 32768u ? t << 1 : (65535u - t) << 1; }
/* the one-pole low-pass (topology-preserving) coefficient of cutoff index c, Q16: G = g / (1 + g), g = tan(pi fc / FS) */
static int32_t ins_lpk(int32_t c)
{
    uint32_t g = SVF_G[clamp(c, 0, 127)];
    return (int32_t)((g << 16) / (4096u + g));
}
/* the drives' and CRUSH's shape of v (a voice's full scale 32768; v already through DRIVE) */
static inline int32_t ins_shape(uint32_t ty, int32_t v, uint32_t sh)
{
    switch (ty) {
    case IT_SOFT:
        return softclip(v);
    case IT_HARD:
        return clamp(v, -24000, 24000);
    case IT_FOLD: {
        int32_t u = (int32_t)((uint32_t)(v + 16384) & 0xFFFFu) - 32768;   /* a triangle of period 65536: +-16384 */
        u = 16384 - (u < 0 ? -u : u);
        return u + (u >> 1);                            /* x 1.5 (no divide) */
    }
    case IT_FUZZ:
        return v > 0 ? (v > 20000 ? 20000 : v) : softclip(v) >> 1;
    case IT_CRUSH:                                      /* sh: 16 - BITS; to the nearest step */
        return sh ? ((v + (1 << (sh - 1u))) >> sh) * (1 << sh) : v;
    default:
        return v;
    }
}
static inline uint32_t ins_bits_sh(int32_t a) { return 15u - (uint32_t)(clamp(a, 0, 127) >> 3); }   /* BITS 1..16 */

/* SOFT .. FUZZ: low cut -> DRIVE -> shape -> TONE -> LEVEL -> DC blocker; CRUSH: held sample -> bits -> LPF. The state
 * in locals for the block (b may alias it as far as the compiler knows: no load / store a sample), written back */
static __attribute__((noinline,section(".audio_text"))) void ins_drive(ins_t *s, const int16_t *p, int32_t *b, uint32_t n)
{
    uint32_t i, ty = s->type, sh = ins_bits_sh(p[SI_A]);
    uint32_t hold = INS_HOLD[clamp(p[SI_B], 0, 127) >> 3];
    int32_t a = clamp(p[SI_A], 0, 127), g = 4096 + a * a * 3;   /* DRIVE, Q12: 1x .. 12.8x, gentle at first */
    int32_t lp1 = s->lp1, lp2 = s->lp2, x, y, v;
    if (ty == IT_CRUSH) {
        uint32_t k = (uint32_t)ins_lpk(p[SI_C]), cnt = s->cnt;
        int32_t hd = s->hold;
        for (i = 0; i < n; i++) {
            x = clamp(b[i],-262143,262143);
            if (!cnt) {
                hd = ins_shape(IT_CRUSH, clamp(x, -262144, 262143), sh);
                cnt = hold;
            }
            cnt--;
            y = hd * 16;                                /* (x16: the low cutoffs' rounding leaves no offset) */
            v = mulq16(y - lp1, k);                     /* LPF: two one-pole low-passes */
            y = v + lp1;
            lp1 = y + v;
            v = mulq16(y - lp2, k);
            y = v + lp2;
            lp2 = y + v;
            b[i] = (y + 8) >> 4;
        }
        s->cnt = (uint8_t)cnt;
        s->hold = hd;
    } else {
        uint32_t k = (uint32_t)ins_lpk(p[SI_B]);
        int32_t lvl = LEVEL_Q12[p[SI_C] & 127], hp = s->hp, dc = s->dc;
        for (i = 0; i < n; i++) {
            x = clamp(b[i],-262143,262143);
            hp += ((x * 256) - hp) >> 8;                 /* ~27 Hz low cut (Q8: no offset left by the rounding) */
            v = clamp(x - (hp >> 8), -230000, 230000);
            y = ins_shape(ty, ((v >> 3) * g) >> 9, 0) * 16;   /* (>> 3 first: 32 bits; x16 as CRUSH) */
            v = mulq16(y - lp1, k);                     /* TONE: two one-pole low-passes */
            y = v + lp1;
            lp1 = y + v;
            v = mulq16(y - lp2, k);
            y = v + lp2;
            lp2 = y + v;
            y = (((y + 8) >> 4) * lvl) >> 12;           /* LEVEL */
            dc += ((y * 256) - dc) >> 9;                 /* DC blocker, ~14 Hz (FUZZ's lopsided halves) */
            b[i] = y - (dc >> 8);
        }
        s->hp = hp;
        s->dc = dc;
    }
    s->lp1 = lp1;
    s->lp2 = lp2;
}

/* PHASR, FLANG, CHOR: the sweep at RATE (one LFO per state), DEPTH, FDBK; the state in locals for the block */
static __attribute__((noinline,section(".audio_text"))) void ins_swept(ins_t *s, const int16_t *p, int32_t *b, uint32_t n)
{
    uint32_t i, inc = LFO_INC[clamp(p[SI_A], 0, 127)] / CTL, dep = (uint32_t)clamp(p[SI_B], 0, 127);
    int32_t c = clamp(p[SI_C], -127, 127), dc = s->dc;
    if (s->type == IT_PHASER) {                         /* the coefficients a block at a time (at its middle) */
        uint32_t tr = ins_tri(s->ph + (n >> 1) * inc), q = 32768u + (uint32_t)(((int32_t)tr - 32768) * (int32_t)dep / 127);
        int32_t a = PH_A[q >> 11] + (((PH_A[(q >> 11) + 1u] - PH_A[q >> 11]) * (int32_t)(q & 2047u)) >> 11);
        int32_t fbk = c * 180, fb = s->fb;              /* 0 .. 0.7, Q15 */
        int32_t x0 = s->px[0], x1 = s->px[1], x2 = s->px[2], x3 = s->px[3];
        int32_t y0 = s->py[0], y1 = s->py[1], y2 = s->py[2], y3 = s->py[3];
        _Static_assert(PH_ST == 4u, "ins_swept: four all-pass stages");
        for (i = 0; i < n; i++) {
            int32_t x = clamp(b[i],-262143,262143), u = clamp((x + 4) >> 3, -16383, 16383) + ((fb * fbk + 16384) >> 15), y;
            /* y = a (u - y1) + u1, four times */
            y = clamp(x0 + ((a * (u - y0) + 8192) >> 14), -65535, 65535), x0 = u, y0 = y, u = y;
            y = clamp(x1 + ((a * (u - y1) + 8192) >> 14), -65535, 65535), x1 = u, y1 = y, u = y;
            y = clamp(x2 + ((a * (u - y2) + 8192) >> 14), -65535, 65535), x2 = u, y2 = y, u = y;
            y = clamp(x3 + ((a * (u - y3) + 8192) >> 14), -65535, 65535), x3 = u, y3 = y;
            fb = clamp(y, -32767, 32767);
            y *= 4;                                    /* (y: at an eighth) */
            dc += ((y * 256) - dc) >> 9;                 /* DC blocker, ~14 Hz (a clipped loud input is lopsided) */
            b[i] = (x >> 1) + y - (dc >> 8);   /* (dry + all-passed) / 2 */
        }
        s->px[0] = x0, s->px[1] = x1, s->px[2] = x2, s->px[3] = x3;
        s->py[0] = y0, s->py[1] = y1, s->py[2] = y2, s->py[3] = y3;
        s->fb = fb;
        s->ph += n * inc;
    } else {
        int32_t chorus = s->type == IT_CHORUS, fbk = c * (chorus ? 103 : 219);   /* 0 .. 0.4 / 0 .. 0.85, Q15 */
        uint32_t dq = dep * 516u, ph = s->ph, wp = s->wp; /* DEPTH, Q16 */
        int16_t *line = s->line;
        for (i = 0; i < n; i++, ph += inc) {
            int32_t x = clamp(b[i],-262143,262143), r0, r1, v, fr;
            uint32_t d;                                 /* the delay, Q16 samples */
            if (chorus) {
                d = (uint32_t)((140 << 16) + ((osc_sine(ph) * (int32_t)dep * 50) >> 5));   /* 3.2 +- 2.3 ms */
            } else {
                uint32_t tr = ins_tri(ph), sw = (tr * tr) >> 16;   /* longer near the short end */
                d = (11u << 16) + 209u * (((sw >> 1) * dq) >> 15);  /* 0.25 .. 5 ms */
            }
            r0 = (int32_t)(wp - (d >> 16)) & (int32_t)(IL_LEN - 1u);
            r1 = (r0 - 1) & (int32_t)(IL_LEN - 1u);
            fr = (int32_t)((d >> 1) & 0x7FFFu);
            v = line[r0] + (((line[r1] - line[r0]) * fr) >> 15);
            line[wp] = (int16_t)clamp((x >> 2) + softclip(mulq15(v, fbk)), -32767, 32767);
            wp = (wp + 1u) & (IL_LEN - 1u);
            v *= 2;                                    /* (v: at a quarter) */
            dc += ((v * 256) - dc) >> 9;                 /* DC blocker (the line's clip on a loud lopsided wave) */
            b[i] = (x >> 1) + v - (dc >> 8);   /* (dry + delayed) / 2 */
        }
        s->ph = ph;
        s->wp = (uint16_t)wp;
    }
    s->dc = dc;
}


typedef struct {ins_t ch[2];uint32_t stereo,quiet,asleep;} fx_sloop_insert_t;
static uint32_t fx_sloop_insert_bytes(uint32_t type)
{return sizeof(fx_sloop_insert_t)+(type==FX_FLANGER || type==FX_PEDAL_CHORUS?2u*IL_LEN*sizeof(int16_t):0u);}
static uint32_t fx_sloop_kind(uint32_t t)
{
    static const uint8_t kind[8] __attribute__((section(".data.fx_sloop")))=
        {IT_SOFT,IT_HARD,IT_FOLD,IT_FUZZ,IT_CRUSH,IT_FLANGER,IT_PHASER,IT_CHORUS};
    return kind[t-FX_SOFT];
}
static __attribute__((noinline,section(".audio_text"))) void fx_sloop_insert_run(
    fx_state_t *f,fx_sloop_insert_t *s,uint32_t n,int32_t target,int32_t *l,int32_t *r,int32_t third,uint32_t shared)
{
    uint32_t kind=fx_sloop_kind(f->type);
    if(s->asleep && !f->echo){
        f->mix+=clamp(target-f->mix,-128*(int32_t)n,128*(int32_t)n);
        return;
    }
    if(f->echo){s->asleep=0;s->quiet=0;}
    int16_t p[4]={(int16_t)kind,f->rate,f->character,112};
    if(kind==IT_CRUSH){p[SI_A]=127-f->rate;p[SI_B]=127-f->character;p[SI_C]=96;}
    if(kind>=IT_PHASER)p[SI_C]=(int16_t)(third*127/63);
    /* Mono engines get one processor until a stereo signal arrives. Copy its
     * history only on that transition, preserving all tails and pan changes. */
    uint32_t delay=kind==IT_FLANGER || kind==IT_CHORUS;
    int16_t *lines=(int16_t *)(s+1);
    if(!f->mono && !s->stereo){
        memcpy(&s->ch[1],&s->ch[0],sizeof(ins_t));
        if(delay)memcpy(lines+IL_LEN,lines,IL_LEN*sizeof(int16_t));
        s->stereo=1;
    }
    if(delay){s->ch[0].line=lines;s->ch[1].line=lines+IL_LEN;}
    uint32_t channels=s->stereo?2u:1u;
    for(uint32_t ch=0;ch<channels;ch++){
        ins_t *v=&s->ch[ch];v->type=(uint8_t)kind;
        if(kind>=IT_PHASER)ins_swept(v,p,ch?r:l,n);
        else ins_drive(v,p,ch?r:l,n);
    }
    int32_t mix=f->mix,peak=0;
    for(uint32_t i=0;i<n;i++){
        int32_t wl=l[i],wr=channels==1u?wl:r[i],dl=fx_dry_l[i],dr=fx_dry_r[i];
        /* SLOOP's pedal algorithms include half dry. Parallel returns supply
         * the swept signal alone; our mixer already carries the dry track. */
        if(shared && kind>=IT_PHASER){wl=(wl-(clamp(dl,-262143,262143)>>1))*2;wr=(wr-(clamp(dr,-262143,262143)>>1))*2;}
        int32_t al=wl<0?-wl:wl,ar=wr<0?-wr:wr;
        if(al>peak)peak=al;if(ar>peak)peak=ar;
        if(mix!=target)mix+=clamp(target-mix,-128,128);
        l[i]=dl+fx_scale(wl-dl,(uint32_t)mix*2u);r[i]=dr+fx_scale(wr-dr,(uint32_t)mix*2u);
    }
    f->mix=mix;
    if(!f->echo && peak<=4)s->quiet+=n;else s->quiet=0;
    if(s->quiet>=512u){
        /* A full delay revolution below four output units proves there is
         * no pending audible tap. Retire residual integer DC/tail state. */
        memset(s,0,fx_sloop_insert_bytes(f->type));s->asleep=1;
    }
}
