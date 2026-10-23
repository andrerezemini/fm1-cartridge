/* SPDX-License-Identifier: GPL-3.0-only */
/* The real menu and panel input, including the playback-only build's item map. */
#include <stdint.h>
#define FELUCCA_UAC_RX 1
#ifndef FELUCCA_UAC
#define FELUCCA_UAC 0
#endif
#define UAC_RX_BARRIER() __asm__ volatile("" ::: "memory")
#define UAC_RX_LOCK() 0u
#define UAC_RX_UNLOCK(s) ((void)(s))
#include "../firmware/src/uac_rx.c"
#define UI_PAGES_HARNESS_ONLY 1
#include "ui_pages_test.c"

int main(int argc, char **argv)
{
    uint32_t i, saved, sig;
    int32_t width = 4;
    uint16_t color;
    outdir = argc > 1 ? argv[1] : "build/host";
    panel = PANEL_DEFAULT;
    layers_init();
    palette_set(4);
    host_tracks_init();
    for (i = 0; i < MS_COUNT; i++) {
        check(MS_FIRST[i + 1] > MS_FIRST[i] && MS_FIRST[i + 1] - MS_FIRST[i] <= 4,
              "each menu section has one to four rows");
        width += text_w(&FONT_S, MS_NAME[i]) + MI_TAB_GAP;
        ui.menu = 1; ui.menu_sel = MS_FIRST[i]; ui.force = 1;
        draw_menu();
    }
    check(width - MI_TAB_GAP <= 240, "all section tabs fit the display");
    check(mi_sec(MI_RETURN) == MS_RETURN && mi_row(MI_RETURN) == 0,
          "USB RETURN is the first knob in the USB section");
    check(!strcmp(mi_value(MI_RETURN, &color), "100%"), "return starts at unity");
    saved = lights_word();
    for (i = 0; i < 150; i++) mi_set(MI_RETURN, -1);
    check(uac_rx_gain == 0 && !strcmp(mi_value(MI_RETURN, &color), "OFF"), "gain clamps at mute");
    for (i = 0; i < 50; i++) mi_set(MI_RETURN, 1);
    check(uac_rx_gain == 64 && !strcmp(mi_value(MI_RETURN, &color), "50%"), "gain reaches half scale");
    for (i = 0; i < 150; i++) mi_set(MI_RETURN, 1);
    check(uac_rx_gain == 128, "gain clamps at unity");
    ui.menu = 1; ui.menu_sel = MI_RETURN; ui.force = 1;
    frame();
    ppm("cartridge-usb-return");
    sig = ui.menu_sig;
    encs[panel.enc[EN_K1]] = -1;
    frame();
    check(uac_rx_gain < 128 && ui.menu_sig != sig, "knob changes return and redraws without reopening menu");
    mi_set(MI_RETURN, 0);
    check(uac_rx_gain == 0, "OCT+ mutes the return");
    ui.force = 1; frame(); ppm("cartridge-usb-muted");
    mi_set(MI_RETURN, 0);
    check(uac_rx_gain == 128, "OCT+ restores unity");
    check(lights_word() == saved, "return gain does not change the saved settings word");
    return fails ? 1 : 0;
}
