/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdint.h>
#define FELUCCA_CARTRIDGE 1
#define FELUCCA_OTA 1
#define FELUCCA_UAC_RX 1
#define FELUCCA_UAC 0
#define UAC_RX_BARRIER() __asm__ volatile("" ::: "memory")
#define UAC_RX_LOCK() 0u
#define UAC_RX_UNLOCK(s) ((void)(s))
#include "../firmware/src/uac_rx.c"
#define UI_PAGES_HARNESS_ONLY 1
#include "ui_pages_test.c"
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) { fm1_ms++; }
#ifdef CART_REAL_SERVER
#include "clipper_synth.h"
#include "cartridge_remote.h"
static cartridge_remote_t esp;
#endif
static uint8_t last_request[CART_REQUEST_SIZE];
static uint32_t cart_test_ch = 1;                  /* the MIDI channel the simulated cartridge answers */
static uint32_t drain_request(void)
{
    uint32_t n = 0;
    while (so_r != so_w) {
        uint32_t pkt = sx_out_q[so_r++ % SXQ], cin = pkt & 15u;
        uint32_t k, nb = cin == 4 || cin == 7 ? 3 : cin - 4;
        for (k = 0; k < nb; k++) { assert(n < sizeof last_request); last_request[n++] = (uint8_t)(pkt >> (8 * (k + 1))); }
    }
    return n;
}
static void response(uint8_t *r)
{
    uint32_t i;
    for (i = 0; i < CART_REPLY_SIZE; ++i) sysex_byte(r[i]);
    cart_service(1);
}
static void exchange(void)
{
    uint8_t r[CART_REPLY_SIZE] = {0};
    assert(drain_request() == CART_REQUEST_SIZE && cart_valid(last_request, sizeof last_request));
#ifdef CART_REAL_SERVER
    assert(cartridge_remote_handle(&esp, last_request, sizeof last_request, r));
#else
    memcpy(r, cart.view, sizeof r);
    cart_header(r, cart_u14(last_request + CART_SEQ), CART_REPLY, cart_u14(last_request + CART_REV) + 1);
    r[CART_STATUS] = CART_OK; r[CART_NENG] = 23; cart_put14(r + CART_NPRE, 8);
    str_cpy((char *)r + CART_FW_NAME, "CLIPPER", 12); r[CART_SLOT] = 0; r[CART_NSLOT] = 3;
    r[CART_MIDI_CH] = (uint8_t)cart_test_ch;
    cart_put14(r + CART_NPAR, 130); cart_put14(r + CART_VAL, 8192 + 64);
    cart_put14(r + CART_MIN, 8192); cart_put14(r + CART_MAX, 8192 + 127);
    str_cpy((char *)r + CART_ENG_NAME, "ANALOG", 12);
    str_cpy((char *)r + CART_PRE_NAME, "808 BOOM", 24);
    str_cpy((char *)r + CART_PAR_NAME, "ENG CUTOFF", 20);
    str_cpy((char *)r + CART_VAL_NAME, "1.2", 16); str_cpy((char *)r + CART_UNIT, "kHz", 8);
    r[CART_REPLY_SIZE - 1] = 0xf7;
#endif
    response(r);
}
static void refresh_page(void)
{
    cart_service(0);
    fm1_ms += 501;
    cart_service(1);
}
static void test_display_refresh(void)
{
    uint16_t before[240 * 240];
    uint8_t r[CART_REPLY_SIZE];
    uint32_t blits = lcd_blit_calls, clears = lcd_full_fills;
    uint16_t seq = cart.seq;
    memcpy(before, screen, sizeof before);
    for (uint32_t i = 0; i < 8; ++i) {
        fm1_ms += 501; cart_service(1);
        if (so_w != so_r) exchange();
        frame();
    }
    check(cart.online && !cart.waiting, "idle page retains the confirmed link state");
    check(cart.seq == seq && so_w == so_r,
          "connected idle CARTRIDGE page sends no periodic SysEx requests");
    check(lcd_blit_calls == blits && lcd_full_fills == clears,
          "unchanged replies never write to the LCD (no periodic flicker)");
    check(!memcmp(before, screen, sizeof before), "idle polling preserves every display pixel");

    /* A shorter label must replace the old text without a blank-screen pass. */
    refresh_page(); assert(drain_request() == CART_REQUEST_SIZE);
    memcpy(r, cart.view, sizeof r);
    cart_put14(r + CART_SEQ, cart_u14(last_request + CART_SEQ));
    memset(r + CART_PRE_NAME, 0, CART_PAR_NAME - CART_PRE_NAME);
    str_cpy((char *)r + CART_PRE_NAME, "X", CART_PAR_NAME - CART_PRE_NAME);
    response(r); frame();
    check(lcd_blit_calls > blits && memcmp(before, screen, sizeof before),
          "changed metadata refreshes the displayed text");
    check(lcd_full_fills == clears, "metadata update never blanks the whole LCD");
    memcpy(before, screen, sizeof before);
    for (uint32_t i = 0; i < 240u * 240u; ++i) screen[i] = 0x1234;
    ui.force = 1; frame();
    check(!memcmp(before, screen, sizeof before),
          "forced page redraw restores all pixels including gaps, without text remnants");
    check(lcd_full_fills == clears, "forced CARTRIDGE redraw uses composed strips without blanking");
    refresh_page(); exchange(); frame();
    /* Re-entry must refresh promptly even before the old 500 ms poll delay. */
    cart_service(0); fm1_ms += 1; cart_service(1);
    check(cart.waiting && cart.q[CART_OP] == CART_QUERY && so_w != so_r,
          "page re-entry requests current state immediately");
    cart_turn(3, 2);
    check(!cart.delta[3], "knobs wait for the page re-entry state before editing");
    if (so_w != so_r) exchange();
    /* A full TX ring cannot swallow the entry refresh or allow stale edits. */
    cart_service(0);
    so_w = so_r + SXQ;
    cart_service(1); cart_turn(0, 1);
    check(!cart.waiting && cart.refresh && !cart.delta[0],
          "entry refresh survives TX backpressure and blocks stale edits");
    so_r = so_w;
    cart_service(1); exchange();
    check(cart.online && !cart.refresh && !cart.waiting,
          "entry refresh completes when USB TX space is available");
}
/* MIDI OUT packets (usb.c midi_out_q) since mo_r with this status byte and note */
static uint32_t midi_seen(uint32_t status, uint32_t note)
{
    uint32_t i, n = 0;
    for (i = mo_r; i != mo_w; i++) {
        uint32_t pkt = midi_out_q[i % MQ];
        n += ((pkt >> 8) & 0xFFu) == status && ((pkt >> 16) & 0x7Fu) == note;
    }
    return n;
}
static void set_cart_channel(uint32_t ch)              /* the cartridge answers ch (0 OMNI) from its next reply */
{
#ifdef CART_REAL_SERVER
    assert(!clipper_synth_set_midi_channel(ch));
#endif
    cart_test_ch = ch;
    refresh_page(); exchange();
}
static uint32_t voices_on(const track_t *t)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++) n += t->v[i].active;
    return n;
}
/* the track on the cartridge's MIDI channel is the cartridge's: its names, its notes over MIDI (keys, steps), its
 * engine silent, PRESETS on its presets; OMNI or a lost link give it back */
static void test_cartridge_track(void)
{
    const char *ce, *cp;
    char b[16];
    uint8_t n60 = 60;
    uint32_t i, seen, pre;
    set_cart_channel(1);
    check(cart.online && cart.view[CART_MIDI_CH] == 1 && cart_tracks == 1u,
          "the track on the cartridge's MIDI channel (track 1) is the cartridge's track");
    check(cart_track_names(0, &ce, &cp) && !cart_track_names(1, &ce, &cp) && !cart_track_names(TRK_DRUM, &ce, &cp),
          "only that track takes the cartridge's engine and preset names");
    trk_short_name(0, b);
    check(!strncmp(b, (const char *)cart.view + CART_PRE_NAME, 12), "TRACKS shows the cartridge's preset on its track");
    frame();                                         /* the audio block takes the track over */
    mo_r = mo_w;
    trk_note_on(&trk[0], 60, 90);
    check(!voices_on(&trk[0]) && midi_seen(0x90, 60) == 1, "a note of the cartridge's track goes out over MIDI, its engine stays silent");
    mo_r = mo_w;
    seq_out_on(&trk[0], 61, 90); song.g[G_MIDI] = 1; seq_out_on(&trk[0], 61, 90); song.g[G_MIDI] = 0;
    check(mo_r == mo_w, "MIDI OUT = SEQ never sends a note of the cartridge's track twice");
    trk_note_off(&trk[0], 60);
    check(midi_seen(0x80, 60) == 1, "its note-off goes out on the same channel");
    mo_r = mo_w;
    trk_note_on(&trk[1], 62, 90);
    check(voices_on(&trk[1]) && mo_r == mo_w, "another track keeps its own engine and sends nothing");
    trk_note_off(&trk[1], 62);
    mo_r = mo_w;
    trk_note_on(&trk[0], 66, 90); trk[0].p[P_MUTE] = 1; frame();
    trk_note_on(&trk[0], 67, 90);
    check(midi_seen(0x80, 66) == 1 && !midi_seen(0x90, 67), "MUTE ends the notes the cartridge's track sent and sends no new ones");
    trk[0].p[P_MUTE] = 0;
    /* a step of the cartridge's track (a take, recorded or written) plays the cartridge with MIDI OUT = KEYS */
    put_step(&trk[0], 0, 1, &n60, ST_NOTE, 0);
    song.g[G_MIDI] = 0; mo_r = mo_w; transport_req = 1;
    for (i = seen = 0; i < 8u && !seen; i++) { frame(); seen = midi_seen(0x90, 60); }
    check(seen && !voices_on(&trk[0]), "a step of the cartridge's track plays the cartridge, MIDI OUT left at KEYS");
    transport_req = 2; frames(2);
    check(!(ct_set[0][0] | ct_set[0][1] | ct_set[0][2] | ct_set[0][3]), "STOP ends every note sent to the cartridge");
    put_step(&trk[0], 0, 0, &n60, ST_REST, 0);
    /* PRESETS on HOME browses the cartridge's presets on its track */
    ui.home = 1; song.sel = 0;
    pre = cart_u14(cart.view + CART_PRE);
    if (pre + 1u < cart_u14(cart.view + CART_NPRE)) {
        encs[panel.enc[EN_PRESET]] = 1; frame();
        check(cart.delta[1] == 1 && !cart.delta[0], "PRESETS on the cartridge's track asks the cartridge for its next preset");
        cart_service(0); exchange();
        check(last_request[CART_OP] == CART_PRESET && cart_s14(last_request + CART_ARG) == 1, "the request is a PRESET +1");
#ifdef CART_REAL_SERVER
        check(cart_u14(cart.view + CART_PRE) == pre + 1u, "the real cartridge loaded its next preset");
#endif
        ui.force = 1; frame(); ppm("cartridge-track-home");
    }
#ifndef CART_REAL_SERVER
    cart_put14(cart.view + CART_PRE, (uint16_t)(cart_u14(cart.view + CART_NPRE) - 1u)); cart.view[CART_ENG] = 3;
    encs[panel.enc[EN_PRESET]] = 1; frame();
    check(cart.delta[0] == 1 && !cart.delta[1], "PRESETS past the last preset goes on into the next engine");
    memset(cart.delta, 0, sizeof cart.delta);
    cart_put14(cart.view + CART_PRE, 0);
    encs[panel.enc[EN_PRESET]] = -1; frame();
    check(cart.delta[0] == -1 && cart.to_last, "PRESETS before the first preset goes back into the previous engine");
    cart_service(0); exchange();                     /* the engine's reply is in: the next request goes out with it */
    check(!cart.to_last && cart.waiting && cart.q[CART_OP] == CART_PRESET && cart_s14(cart.q + CART_ARG) == 8191,
          "then to that engine's last preset");
    exchange();
#endif
    ui.home = 0;
    {   /* the PRESETS page on the cartridge's track: KNOB 1 its presets, KNOB 2 its engines; the track's own sound
         * and the favourites stay as they were */
        uint32_t k, back = ui.page, e0 = trk[0].eng_req, p0 = trk[0].preset, f0 = fav[0];
        for (k = 0; k < NPAGES; k++) if (!strcmp(PAGES[k].title, "PRESETS")) break;
        open_family(PAGES[k].fam); ui.page = (uint8_t)k; ui.fam_last[PAGES[k].fam] = (uint8_t)k; page_entered(); frames(2);
        encs[panel.enc[EN_K1]] = 1; frame();
        cart_service(0); exchange();
        check((last_request[CART_OP] == CART_PRESET || last_request[CART_OP] == CART_ENGINE) &&
              cart_s14(last_request + CART_ARG) == 1, "PRESETS page, KNOB 1: the cartridge's next preset");
        encs[panel.enc[EN_K2]] = 1; frame();
        cart_service(0); exchange();
        check(last_request[CART_OP] == CART_ENGINE && cart_s14(last_request + CART_ARG) == 1,
              "PRESETS page, KNOB 2: the cartridge's next engine");
        encs[panel.enc[EN_K4]] = 1; frame();
        check(trk[0].eng_req == e0 && trk[0].preset == p0 && fav[0] == f0 && !cart.waiting,
              "the track's own engine, preset and favourites stay as they were");
        ui.force = 1; frame(); ppm("cartridge-track-presets");
        open_family(FAM_GLO); ui.page = (uint8_t)back; ui.fam_last[FAM_GLO] = (uint8_t)back; page_entered(); frames(2);
    }
    /* OMNI: no track is the cartridge's, the notes it had are ended, the track's engine and names are back */
    mo_r = mo_w;
    trk_note_on(&trk[0], 64, 90);
    set_cart_channel(0);
    check(!cart_tracks && !cart_track_names(0, &ce, &cp), "OMNI: no track is the cartridge's");
    frame();
    check(midi_seen(0x80, 64) == 1, "a track the cartridge gives back ends the notes it had sent");
    trk_note_on(&trk[0], 65, 90);
    check(voices_on(&trk[0]) > 0, "and its own engine plays again");
    trk_all_off(&trk[0]); frames(2);
    set_cart_channel(1);
    check(cart_tracks == 1u, "channel 1 again: track 1 is the cartridge's");
    frame();
}
#ifndef CARTRIDGE_UI_HARNESS_ONLY
int main(int argc, char **argv)
{
    outdir = argc > 1 ? argv[1] : "build/host";
    panel = PANEL_DEFAULT; layers_init(); palette_set(4); host_tracks_init();
#ifdef CART_REAL_SERVER
    assert(!clipper_synth_init(44100)); assert(!clipper_synth_set_preset(0, 0));
    assert(!clipper_synth_set_midi_channel(1));
    cartridge_remote_init(&esp, clipper_synth_api(), NULL, 42);
#endif
    usb.config = 1; fm1_ms = 600;
    cart_charge_boot();
    check(cart_keep.magic == CART_KEEP_MAGIC && cart_keep.boot_en == 1 && !cart_keep.off && !host_chg_writes,
          "boot reads the charger as the SoC left it (enabled) without writing");
    cart_service(0);
    check(cart.waiting && so_w != so_r && cart.probe == CART_PROBES - 1u,
          "hidden page probes the cartridge once USB is configured");
    assert(drain_request() == CART_REQUEST_SIZE && cart_valid(last_request, sizeof last_request));
    for (uint32_t i = 0; i < 80; ++i) { fm1_ms += 251; cart_service(0); drain_request(); }
    check(!cart.waiting && !cart.probe && so_w == so_r, "unanswered probes run out (bounded)");
    fm1_ms += 5000; cart_service(0);
    check(!cart.waiting && so_w == so_r, "hidden offline page then sends no discovery traffic");
    check(host_chg_en == 1 && !host_chg_writes, "offline: the charger untouched");
    open_family(FAM_GLO);
    uint32_t tries = 0;
    while (cur_page()->scope != SC_CART && tries++ < NPAGES) { encs[panel.enc[EN_SELECT]] = 1; frame(); }
    check(cur_page()->scope == SC_CART, "GLO + SELECT reaches CARTRIDGE");
    ui.force = 1; frame(); ppm("cartridge-offline");
    cart_turn(0, 1); check(cart.delta[0] == 0, "offline knobs never queue unseen edits");
    cart_service(1); exchange();
    check(cart.online && !cart.waiting && cart.view[CART_NENG] == 23, "USB SysEx handshake shows engine catalogue");
    {   /* v2: the page is numbered among the GLO pages and names the firmware the cartridge runs */
        uint32_t n, k = page_pos(&n);
        check(k == 2 && n >= 9, "CARTRIDGE is page 2 of the GLO family, numbered like the others");
        {   /* every English label fits its strip: header left and right never meet, rows and footer stay inside */
            static const char *const right[] = {"CLIPPER OK", "MELODEE OK", "FELUCCA OK", "NO LINK"};
            int32_t left = 6 + text_w(&FONT_S, "CARTRIDGE 10/10"), ok = 1;
            for (uint32_t i = 0; i < 4u; i++) ok &= left + 8 <= 234 - text_w(&FONT_S, right[i]);
            ok &= 6 + text_w(&FONT_S, "WAITING FOR CARTRIDGE") <= 234 && 6 + text_w(&FONT_S, "3 PARAMETER") <= 180;
            ok &= 6 + text_w(&FONT_S, CART_FOOT_FIRMWARE) <= 234 && 6 + text_w(&FONT_S, CART_FOOT_PAGES) <= 234;
            printf("widths: header %d, footer %d / %d, waiting %d\n", left, 6 + text_w(&FONT_S, CART_FOOT_FIRMWARE),
                   6 + text_w(&FONT_S, CART_FOOT_PAGES), 6 + text_w(&FONT_S, "WAITING FOR CARTRIDGE"));
            check(ok, "English header, rows and footers fit the 240 px screen without overlapping");
        }
        check(!strcmp((const char *)cart.view + CART_FW_NAME, "CLIPPER") && cart.view[CART_NSLOT] >= 1 &&
              cart.view[CART_SLOT] < cart.view[CART_NSLOT], "the reply names the running firmware and its slots");
    }
    check(host_chg_en == 0 && cart_keep.off && host_chg_writes == 1, "AUTO: charger held off while the cartridge link is up");
    ui.force = 1; frame(); ppm("cartridge-online");
    test_display_refresh();
    {   /* FIRMWARE (knob 3): the value row shows the slot picked "2/3" and the footer says what happens */
        uint16_t before[240 * 240];
        uint8_t r[CART_REPLY_SIZE];
        ui.force = 1; frame(); memcpy(before, screen, sizeof before);
        refresh_page(); assert(drain_request() == CART_REQUEST_SIZE);
        memcpy(r, cart.view, sizeof r);
        cart_put14(r + CART_SEQ, cart_u14(last_request + CART_SEQ));
        memset(r + CART_PAR_NAME, 0, CART_VAL_NAME - CART_PAR_NAME); str_cpy((char *)r + CART_PAR_NAME, "FIRMWARE", 20);
        memset(r + CART_VAL_NAME, 0, CART_UNIT - CART_VAL_NAME); str_cpy((char *)r + CART_VAL_NAME, "MELODEE", 16);
        memset(r + CART_UNIT, 0, CART_FW_NAME - CART_UNIT); str_cpy((char *)r + CART_UNIT, "IN 2 S", 8);
        cart_put14(r + CART_VAL, 8192 + 1); cart_put14(r + CART_MIN, 8192); cart_put14(r + CART_MAX, 8192 + 2);
        response(r); frame();
        check(cart_on_firmware(), "the FIRMWARE parameter is recognised from the reply");
        check(memcmp(before + CART_FOOT_Y * 240, screen + CART_FOOT_Y * 240, (240 - CART_FOOT_Y) * 240 * 2) != 0,
              "FIRMWARE selected: the footer explains knob 4 and the reboot");
        ppm("cartridge-firmware");
        uint8_t nslot = cart.view[CART_NSLOT];
        memcpy(r, cart.view, sizeof r); r[CART_NSLOT] = 0;      /* a malformed slot count never replaces the state */
        refresh_page(); assert(drain_request() == CART_REQUEST_SIZE);
        cart_put14(r + CART_SEQ, cart_u14(last_request + CART_SEQ)); response(r);
        check(cart.waiting && cart.view[CART_NSLOT] == nslot, "a reply with no slots is rejected");
        fm1_ms += 600; cart_service(1); exchange(); check(cart.online && !cart_on_firmware(), "back to the catalogue");
    }
    test_cartridge_track();
    int16_t local_params[P_COUNT]; memcpy(local_params, TSEL->p, sizeof local_params);
    uint8_t old_engine = cart.view[CART_ENG];
    encs[panel.enc[EN_K1]] = 1; encs[panel.enc[EN_K4]] = 3; frame();
    check(cart.delta[0] == 1 && cart.delta[3] == 0, "same-frame value cannot modify an unseen engine");
    cart_service(1);
    check(cart.q[CART_OP] == CART_ENGINE, "KNOB 1 sends engine command");
    cart_turn(3, 12); check(cart.delta[3] == 0, "value blocked while unseen engine change is pending");
    uint8_t original[CART_REQUEST_SIZE]; memcpy(original, cart.q, sizeof original);
    assert(drain_request() == CART_REQUEST_SIZE);
#ifdef CART_REAL_SERVER
    { uint8_t lost[CART_REPLY_SIZE]; assert(cartridge_remote_handle(&esp, last_request, sizeof last_request, lost)); }
#endif
    fm1_ms += 251; cart_service(1);
    check(!memcmp(cart.q, original, sizeof original), "lost response retries same sequence and revision");
    exchange();
#ifdef CART_REAL_SERVER
    check(cart.view[CART_ENG] == old_engine + 1, "real Clipper engine switched once after retry");
#else
    (void)old_engine;
#endif
    encs[panel.enc[EN_K2]] = 1; frame(); cart_service(1); exchange();
    check(last_request[CART_OP] == CART_PRESET, "KNOB 2 selects preset");
    encs[panel.enc[EN_K3]] = 1; frame(); cart_service(1); exchange();
    check(last_request[CART_OP] == CART_PARAM, "KNOB 3 selects parameter");
    encs[panel.enc[EN_K4]] = 5; frame(); cart_service(1); exchange();
    check(last_request[CART_OP] == CART_VALUE, "KNOB 4 edits parameter value");
    check(!memcmp(local_params, TSEL->p, sizeof local_params), "cartridge knobs leave Sloop track sound intact");
#ifdef CART_REAL_SERVER
    /* Browse every engine from the real UI, including all reply labels. */
    for (uint32_t e = 0; e < 23; ++e) {
        cart_turn(0, (int32_t)e - cart.view[CART_ENG]);
        if (cart.delta[0]) { cart_service(1); exchange(); }
        check(cart.view[CART_ENG] == e, "real ESP32 catalogue matches selected engine");
        ui.force = 1; frame();
    }
    cart_turn(0, -22); cart_service(1); exchange();
    ui.force = 1; frame(); ppm("cartridge-real-analog");
#endif
    /* A stale/malformed frame doesn't acknowledge a pending command. */
    cart_turn(3, 1); cart_service(1); assert(drain_request() == CART_REQUEST_SIZE);
    uint8_t stale[CART_REPLY_SIZE]; memcpy(stale, cart.view, sizeof stale);
    cart_put14(stale + CART_SEQ, cart_u14(cart.q + CART_SEQ) - 1); response(stale);
    check(cart.waiting, "stale ACK ignored");
    for (uint32_t i = 0; i < 3; ++i) { fm1_ms += 251; cart_service(1); if (so_w != so_r) drain_request(); }
    check(!cart.online && !cart.waiting, "timeout shows NO LINK and clears edits");
    check(host_chg_en == 1 && !cart_keep.off, "link lost: charger back as the boot found it");
    fm1_ms += 600; cart_service(1); exchange(); check(cart.online, "automatic recovery after timeout");
    check(host_chg_en == 0 && cart_keep.off, "recovered link: charger held off again");
    usb.suspended = 1; cart_service(1); check(!cart.online, "USB disconnect clears connected status");
    check(host_chg_en == 1 && !cart_keep.off, "USB suspend / unplug: charger restored");
    usb.suspended = 0; usb.resets++; cart_service(1); exchange(); check(cart.online, "USB reconnect fetches actual state");
    check(host_chg_en == 0 && cart_keep.off, "USB reset then reconnect: charger held off again");
    /* Existing editor namespace stays available to its own handler. */
    const uint8_t other[] = {0xf0, 0x00, 0x59, 0x30, 0xf7};
    for (uint32_t i = 0; i < sizeof other; ++i) sysex_byte(other[i]);
    cart_service(1); check(sx_ready, "unrelated SysEx left for OTA/editor"); ota_frame_done();
    {   /* menu HOME -> USB -> CHARGE: AUTO (held off with the link up) / ON (as the boot found it), RAM only */
        uint16_t color;
        uint32_t w;
        check(cart.online && host_chg_en == 0, "link up before the menu checks");
        check(mi_sec(MI_CHARGE) == MS_RETURN && mi_row(MI_CHARGE) == 1 && !strcmp(mi_value(MI_CHARGE, &color), "AUTO"),
              "CHARGE is the second knob of the USB section, AUTO by default");
        check(!strcmp(cart_charge_hint(), "NOW OFF"), "menu hint: the charger is held off now");
        ui.menu = 1; ui.menu_sel = MI_CHARGE; ui.force = 1; frame(); ppm("cartridge-menu-charge-auto");
        mi_set(MI_CHARGE, 1);
        check(cart_charge_on && host_chg_en == 1 && !cart_keep.off && !strcmp(mi_value(MI_CHARGE, &color), "ON") &&
              !cart_charge_hint()[0], "CHARGE ON: the charger back on with the link up, no hint");
        ui.force = 1; frame(); ppm("cartridge-menu-charge-on");
        mi_set(MI_CHARGE, 0);
        check(!cart_charge_on && host_chg_en == 0 && cart_keep.off, "OCT+ back to AUTO: held off again");
        mi_set(MI_CHARGE, -1); check(!cart_charge_on, "knob left: AUTO");
        mi_set(MI_CHARGE, 1); mi_set(MI_CHARGE, -1);
        check(!cart_charge_on && host_chg_en == 0 && cart_keep.off, "knob right then left: held off again");
        ui.menu = 0;
        /* An intentional reset puts the charger back; a crash or watchdog reset that did not gets it back at boot. */
        cart_charge_restore();
        check(host_chg_en == 1 && !cart_keep.off, "restore before UBOOT / update: charger as at boot");
        host_chg_en = 0; cart_keep.off = 1;             /* a watchdog reset while held off: P33 kept the bit */
        cart_charge_boot();
        check(host_chg_en == 1 && cart_keep.boot_en == 1 && !cart_keep.off,
              "boot after an unclean reset: charger restored, true boot state kept");
        cart_keep.magic = 0; cart_keep.off = 1; host_chg_en = 1; w = host_chg_writes;   /* power-on: RAM random */
        cart_charge_boot();
        check(cart_keep.boot_en == 1 && !cart_keep.off && host_chg_writes == w, "power-on ignores a stale record");
        cart_keep.magic = 0; host_chg_en = 0;         /* the SoC booted with the charger already off */
        cart_charge_boot(); cart_service(1);
        check(cart.online && !cart_keep.boot_en && host_chg_en == 0 && host_chg_writes == w &&
              !strcmp(cart_charge_hint(), "BOOT OFF"), "booted with the charger off: AUTO writes nothing, hint BOOT OFF");
    }
    return fails ? 1 : 0;
}
#endif
