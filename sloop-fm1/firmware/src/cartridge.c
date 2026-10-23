/* SPDX-License-Identifier: GPL-3.0-only */
/* Main-loop client of the cartridge. Its metadata stays on the ESP32.
 * One request at a time; bounded retries replay identical sequence/revision.
 * Uses the OTA SysEx rings but a separate 7D 'CF' protocol namespace. */
#if !FELUCCA_OTA || !FELUCCA_UAC_RX
#error "the CARTRIDGE page needs USB playback and the SysEx transport (OTA)"
#endif
#include "cartridge_wire.h"
#ifndef CART_PROBES
#define CART_PROBES 4u              /* discovery queries sent while the page is hidden, each time USB comes up */
#endif
static struct {
    uint8_t q[CART_REQUEST_SIZE], view[CART_REPLY_SIZE];
    uint16_t seq;
    int16_t delta[4];
    uint8_t online, waiting, retries, dirty, active, refresh;
    uint8_t up, probe;              /* USB configured since the last drop; hidden-page probes left (CART_PROBES) */
    uint8_t to_last;                /* PRESETS stepped back into the previous engine: its last preset once it has loaded */
    uint32_t sent_ms, seen_ms, resets;
#if FELUCCA_CARTRIDGE_DIAG
    uint32_t tx_count;
#endif
} cart;

static void cart_offline(void)
{
    if (cart.online || cart.waiting) cart.dirty = 1;
    cart.online = cart.waiting = cart.to_last = 0;
    memset(cart.delta, 0, sizeof cart.delta);
}

/* The FM-1's own charger (fm1_sys.h fm1_charge_*) while the cartridge is the USB host: the ESP32's small cell
 * must not charge the FM-1's battery. CHARGE (menu HOME -> USB, RAM only) AUTO holds CHARGE_EN off while
 * cart.online, ON leaves the charger as the boot found it. P3_CHG_CON0 survives a soft reset, so the state as
 * the boot found it and "we hold it off" live in .noinit: a crash or watchdog reset with the link up still
 * learns the true boot state at the next start and puts the charger back. Not static, as felucca_dbg: a
 * file-local .noinit object would let the compiler fold the magic check away. Power-on: RAM random, magic off.
 * The icon in the header is animation whenever a USB host is configured (ui_draw.c): it never read this bit. */
#define CART_KEEP_MAGIC 0x43484731u                       /* "CHG1" */
struct cart_keep { uint32_t magic; uint8_t boot_en, off; } cart_keep __attribute__((section(".noinit")));
static uint8_t cart_charge_on;                            /* menu CHARGE: 0 AUTO (the default), 1 ON */
static uint32_t cart_charge_writes;                       /* diagnostic: P33 writes this boot */

static void cart_charge_boot(void)                        /* fm1_main, before the interrupts are enabled */
{
    uint32_t en = fm1_charge_en();
    if (cart_keep.magic != CART_KEEP_MAGIC) { cart_keep.magic = CART_KEEP_MAGIC; cart_keep.off = 0; }
    if (cart_keep.off && !en) { en = 1; fm1_charge_set(1); cart_charge_writes++; }   /* a reset found it off by us */
    cart_keep.boot_en = (uint8_t)en; cart_keep.off = 0;
}
static void cart_charge_apply(void)                       /* main loop: after every link / menu change */
{
    uint32_t want = cart_charge_on || !cart.online;      /* 1: as the boot left it */
    if (!cart_keep.boot_en || want == (uint32_t)!cart_keep.off) return;
    fm1_irq_off();
    fm1_charge_set(want);
    cart_keep.off = (uint8_t)!want;
    fm1_irq_on();
    cart_charge_writes++;
}
static void cart_charge_restore(void)                     /* right before an intentional reset: it leaves the
                                                           * interrupts off, the reset follows (fm1_fault: already off) */
{
    fm1_irq_off();
    if (cart_keep.off) { fm1_charge_set(1); cart_keep.off = 0; cart_charge_writes++; }
}
static const char *cart_charge_hint(void)                 /* under the menu row: held off now / booted with it off */
{
    return cart_keep.off ? "NOW OFF" : cart_keep.boot_en ? "" : "BOOT OFF";
}
static void cart_turn(uint32_t knob, int32_t steps)
{
    uint32_t i, op = knob + 1u;
    if (knob >= 4u || !cart.online || cart.refresh ||
        (cart.waiting && cart.q[CART_OP] == CART_QUERY)) return;
    /* Don't edit a value for a parameter/sound whose name hasn't arrived yet.
     * Repeated turns of the same knob accumulate while its ACK is in flight. */
    if (cart.waiting && cart.q[CART_OP] && cart.q[CART_OP] < op) return;
    for (i = 0; i < knob; i++) if (cart.delta[i]) return;
    for (i = knob + 1u; i < 4u; i++) cart.delta[i] = 0;
    cart.delta[knob] = (int16_t)clamp((int32_t)cart.delta[knob] + steps, -8192, 8191);
}
/* The tracks the cartridge plays (core.h cart_tracks): the synth tracks whose GLO > MIDI channel is the channel the
 * cartridge answers (reply CART_MIDI_CH), while it answers. OMNI: none, the cartridge then plays every track's notes
 * as a layer and the tracks keep their own sound */
static void cart_tracks_update(void)
{
    uint32_t k, ch = cart.online ? cart.view[CART_MIDI_CH] : 0u, mask = 0;
    if (ch >= 1u && ch <= 16u)
        for (k = 0; k < NPART; k++)
            if ((uint32_t)song.g[G_CH1 + k] == ch)
                mask |= 1u << k;
    cart_tracks = (uint8_t)mask;
}
/* A track the cartridge plays: the engine and the preset it plays there (the screens show these), else 0 */
static int cart_track_names(uint32_t k, const char **eng, const char **pre)
{
    if (k >= NPART || !((cart_tracks >> k) & 1u) || !cart.online)
        return 0;
    *eng = (const char *)cart.view + CART_ENG_NAME;
    *pre = (const char *)cart.view + CART_PRE_NAME;
    return 1;
}
/* PRESETS on a track the cartridge plays (ui_input.c): the cartridge's presets, on past the last one into the next
 * engine's first, back past the first into the previous engine's last (ENGINE, then PRESET to the end once that
 * engine's reply is in). Turns while a request is out count from where it will land */
static void cart_browse(int32_t s)
{
    int32_t pre = (int32_t)cart_u14(cart.view + CART_PRE) + cart.delta[1];
    int32_t npre = (int32_t)cart_u14(cart.view + CART_NPRE), eng = cart.view[CART_ENG], neng = cart.view[CART_NENG];
    if (!cart.online || !s)
        return;
    if (cart.waiting && cart.q[CART_OP] == CART_PRESET)
        pre += cart_s14(cart.q + CART_ARG);
    if (pre + s >= npre && eng + 1 < neng) {
        cart_turn(0, 1);
    } else if (pre + s < 0 && eng > 0) {
        cart_turn(0, -1);
        if (cart.delta[0])
            cart.to_last = 1;
    } else {
        cart_turn(1, s);
    }
}
static void cart_service_link(int active)
{
    const uint8_t *p;
    uint32_t n, k;
    if (active && !cart.active) {
        cart.refresh = 1;
        memset(cart.delta, 0, sizeof cart.delta);
    }
    cart.active = active != 0;
    if (!usb.config || usb.suspended || cart.resets != usb.resets) {
        cart_offline(); cart.resets = usb.resets; cart.up = 0;
        cart.sent_ms = fm1_ms - 600u;
        if (!usb.config || usb.suspended) return;
    }
    /* USB just came up: a bounded number of discovery queries even with the page hidden, so the charger
     * policy above learns the cartridge is there without a visit to the CARTRIDGE page; a computer host sees a few
     * SysEx frames it ignores. Unanswered, the page goes quiet again until it is opened. */
    if (!cart.up) { cart.up = 1; cart.probe = CART_PROBES; }
    if (ota_frame_get(&p, &n) && n >= 3u && p[0] == 0x7d && p[1] == 'C' && p[2] == 'F') {
        uint8_t v[CART_REPLY_SIZE];
        v[0] = 0xf0; v[CART_REPLY_SIZE - 1] = 0xf7;
        if (n == CART_REPLY_SIZE - 2u) {
            memcpy(v + 1, p, n);
            if (cart.waiting && cart_valid(v, sizeof v) && v[CART_OP] == CART_REPLY &&
                cart_u14(v + CART_SEQ) == cart_u14(cart.q + CART_SEQ) && v[CART_STATUS] <= CART_BAD_REQUEST &&
                v[CART_NENG] && v[CART_ENG] < v[CART_NENG] &&
                cart_u14(v + CART_NPRE) && cart_u14(v + CART_PRE) < cart_u14(v + CART_NPRE) &&
                v[CART_NSLOT] && v[CART_SLOT] < v[CART_NSLOT] && v[CART_MIDI_CH] <= 16u &&
                cart_u14(v + CART_NPAR) && cart_u14(v + CART_PAR) < cart_u14(v + CART_NPAR) &&
                cart_s14(v + CART_MIN) <= cart_s14(v + CART_VAL) &&
                cart_s14(v + CART_VAL) <= cart_s14(v + CART_MAX)) {
                /* Always terminate strings, including a malformed peer's. */
                v[CART_PRE_NAME - 1] = v[CART_PAR_NAME - 1] = v[CART_VAL_NAME - 1] =
                    v[CART_UNIT - 1] = v[CART_FW_NAME - 1] = v[CART_SLOT - 1] = 0;
                /* Query ACKs change sequence/revision even when the displayed
                 * state is unchanged. Keep the LCD untouched in that case. */
                if (!cart.online || memcmp(cart.view + CART_ENG, v + CART_ENG,
                                           CART_REPLY_SIZE - 1u - CART_ENG)) cart.dirty = 1;
                memcpy(cart.view, v, sizeof v);
                cart.online = 1; cart.waiting = cart.refresh = 0; cart.seen_ms = fm1_ms;
                if (v[CART_STATUS]) memset(cart.delta, 0, sizeof cart.delta);
                if (cart.to_last && cart.q[CART_OP] == CART_ENGINE) {   /* (cart_browse) the previous engine is in */
                    cart.to_last = 0;
                    if (!v[CART_STATUS])
                        cart.delta[1] = 8191;                            /* its last preset: the server saturates */
                }
            }
        }
        ota_frame_done();
    }
    if (cart.waiting) {
        if (fm1_ms - cart.sent_ms < 250u) return;
        if (cart.retries >= 3u) { cart_offline(); cart.sent_ms = fm1_ms; return; }
    } else {
        uint8_t op = CART_QUERY;
        int16_t delta = 0;
        if (cart.online && !cart.refresh) for (k = 0; k < 4u; ++k) if (cart.delta[k]) {
            op = (uint8_t)(k + 1u); delta = cart.delta[k]; break;
        }
        /* Every knob ACK already contains the complete state. While connected,
         * query only on page entry, avoiding unnecessary control traffic.
         * Offline discovery keeps its bounded 500 ms retry. */
        if (!op && ((!active && !cart.probe) || (cart.online && !cart.refresh) ||
                    (!cart.refresh && fm1_ms - cart.sent_ms < 500u))) return;
        if (!op && !active) cart.probe--;
        if (SXQ - (so_w - so_r) < (CART_REQUEST_SIZE + 2u) / 3u) return;
        cart_header(cart.q, ++cart.seq, op, cart_u14(cart.view + CART_REV));
        cart_put14(cart.q + CART_ARG, delta + 8192);
        cart.q[CART_REQUEST_SIZE - 1] = 0xf7;
        /* Only remove a queued turn after the complete frame fits in TX. */
        if (SXQ - (so_w - so_r) < (CART_REQUEST_SIZE + 2u) / 3u) return;
        if (op) cart.delta[op - 1u] = 0;
        cart.retries = 0;
    }
    if (SXQ - (so_w - so_r) < (CART_REQUEST_SIZE + 2u) / 3u) return;
    if (ota_wire_send(cart.q, sizeof cart.q) == 0) {
#if FELUCCA_CARTRIDGE_DIAG
        cart.tx_count++;
#endif
        if (cart.q[CART_OP] == CART_QUERY) cart.refresh = 0;
        cart.waiting = 1; cart.retries++; cart.sent_ms = fm1_ms;
    }
}
static void cart_service(int active)
{
    cart_service_link(active);
    cart_tracks_update();
    cart_charge_apply();
}
