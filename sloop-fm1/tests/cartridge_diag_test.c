/* SPDX-License-Identifier: GPL-3.0-only */
#define FELUCCA_CARTRIDGE_DIAG 1
#define CARTRIDGE_UI_HARNESS_ONLY 1
#include "cartridge_ui_test.c"

int main(int argc, char **argv)
{
    uint16_t body[240u * CART_FOOT_Y];
    char line[32];
    uint32_t blits, tx;
    outdir = argc > 1 ? argv[1] : "build/host";
    panel = PANEL_DEFAULT; layers_init(); palette_set(4); host_tracks_init();
    usb.config = 1; fm1_ms = 600;
    cart_service(1); exchange();
    check(cart.tx_count == 1, "diagnostic counts an actual transmitted control request");
    felucca_dbg.late = 123; /* retained debug counter is not this boot's failures */
    ui.force = 1; cart_draw(); ui.force = 0;
    check(!cart_diag.rate_valid && !cart_diag.late, "first diagnostic sample has no rate and baselines retained late count");
    memcpy(body, screen, sizeof body); blits = lcd_blit_calls;
    uar.packets = 1000; fm1_ms += 999;
    cart_draw();
    check(lcd_blit_calls == blits, "diagnostic does not redraw before one second");
    fm1_ms++;
    uar.underruns = 7; uar.errors = 1; uar.overruns = 2; uar.timeouts = 3; uar.malformed = 4;
    felucca_dbg.late += 2;
    midi_out_event(0x643c9009u);  /* audible Note On */
    midi_out_event(0x643c9209u);  /* another channel still counts */
    midi_out_event(0x003c9009u);  /* velocity zero is Note Off */
    midi_out_event(0x003c8008u);  /* explicit Note Off */
    midi_out_event(0x0000f80fu); /* clock */
    mo_w = mo_r + MQ;
    midi_out_event(0x643c9009u);  /* full ring: not accepted */
    cart_draw();
    check(cart_diag.rate_valid && cart_diag.packets_sec == 1000 && cart_diag.underruns == 7 &&
          cart_diag.errors == 10 && cart_diag.notes == 2 && cart_diag.late == 2,
          "diagnostic snapshots audio failures, packet rate and accepted Note On only");
    check(lcd_blit_calls == blits + 1 && !memcmp(body, screen, sizeof body) && !lcd_full_fills,
          "one-second update touches only the composed footer, never blanks the display");
    tx = cart.tx_count;
    for (uint32_t i = 0; i < 8; i++) {
        fm1_ms += 1000; uar.packets += 1000;
        cart_service(1); cart_draw();
    }
    check(cart.tx_count == tx && so_w == so_r, "telemetry never requests remote state or adds periodic USB traffic");
    cart_diag_line(line, 0);
    check(!strcmp(line, "D1 P1000 U7 E10"), "first footer line identifies the build and audio diagnostics");
    cart_diag_line(line, 1);
    check(!strcmp(line, "N2 T1 L2"), "second footer line identifies notes, requests and late audio renders");
    ppm("cartridge-diagnostic");
    fm1_ms += 20000; uar.packets += 20000;
    cart_draw();
    check(!cart_diag.rate_valid, "long absence does not present an old average as a current rate");
    cart_diag.last_packets = UINT32_MAX - 499u; uar.packets = 500;
    cart_diag.last_ms = UINT32_MAX - 499u; fm1_ms = 500;
    cart_draw();
    check(cart_diag.rate_valid && cart_diag.packets_sec == 1000, "packet and millisecond counter wrap preserves the rate");
    cart_diag.packets_sec = cart_diag.underruns = cart_diag.errors = UINT32_MAX;
    cart_diag.notes = cart_diag.tx = cart_diag.late = UINT32_MAX;
    for (uint32_t row = 0; row < 2; row++) {
        cart_diag_line(line, row);
        check(text_w(&FONT_S, line) <= 228 && str_len(line) < sizeof line,
              "saturated counters fit inside the footer and string buffer");
    }
    ui.force = 1; cart_draw(); ui.force = 0;
    ppm("cartridge-diagnostic-max");
    return fails ? 1 : 0;
}
