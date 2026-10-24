/* SPDX-License-Identifier: GPL-3.0-only */
/* Full-width rows keep factory preset names readable on the 240px display. */
#if FELUCCA_CARTRIDGE_DIAG
/* Optional bench display: local counters only, never a USB telemetry query.
 * Sample at most once a second. Snapshot ISR-owned counters with the receiver's
 * IRQ-preserving lock, then format/draw outside it. L starts at page first use
 * because the audio debug structure itself survives a warm reset. */
#define CART_ROW_H 43u
#define CART_FOOT_Y 198u
static struct {
    uint32_t last_ms, last_packets, late_base;
    uint32_t packets_sec, underruns, errors, notes, tx, late;
    uint8_t initialized, rate_valid;
} cart_diag;
static int cart_diag_sample(void)
{
    uint32_t now = fm1_ms, dt = now - cart_diag.last_ms, packets, late, flags;
    if (cart_diag.initialized && dt < 1000u) return 0;
    flags = UAC_RX_LOCK();
    packets = uar.packets;
    cart_diag.underruns = uar.underruns;
    cart_diag.errors = uar.errors + uar.overruns + uar.timeouts + uar.malformed;
    cart_diag.notes = usb.cart_notes;
    late = felucca_dbg.late;
    UAC_RX_UNLOCK(flags);
    cart_diag.rate_valid = cart_diag.initialized && dt <= 10000u;
    cart_diag.packets_sec = cart_diag.rate_valid ?
        (uint32_t)((uint64_t)(packets - cart_diag.last_packets) * 1000u / dt) : 0u;
    if (!cart_diag.initialized) cart_diag.late_base = late;
    cart_diag.late = late - cart_diag.late_base;
    cart_diag.tx = cart.tx_count;
    cart_diag.last_ms = now; cart_diag.last_packets = packets;
    cart_diag.initialized = 1;
    return 1;
}
static void cart_diag_field(char *b, const char *label, uint32_t value)
{
    str_cpy(b + str_len(b), label, 4);
    fmt_int(b + str_len(b), value > 9999u ? 9999 : (int32_t)value);
}
static void cart_diag_line(char *b, uint32_t row)
{
    b[0] = 0;
    if (!row) {
        str_cpy(b, "D1", 3);
        if (cart_diag.rate_valid) cart_diag_field(b, " P", cart_diag.packets_sec);
        else str_cpy(b + str_len(b), " P--", 5);
        cart_diag_field(b, " U", cart_diag.underruns);
        cart_diag_field(b, " E", cart_diag.errors);
    } else {
        cart_diag_field(b, "N", cart_diag.notes);
        cart_diag_field(b, " T", cart_diag.tx);
        cart_diag_field(b, " L", cart_diag.late);
    }
}
#else
#define CART_ROW_H 46u
#define CART_FOOT_Y 210u
#endif
#define CART_FOOT_PAGES "SELECT: PAGES / GLO"
#define CART_FOOT_FIRMWARE "KNOB 4: SLOT, REBOOTS IN 2 S"   /* FIRMWARE selected: what knob 4 does */
/* the firmware the cartridge runs (reply CART_FW_NAME): "MELODEE OK" in the header; the FIRMWARE
 * parameter (knob 3) lists the slots, knob 4 picks one and the cartridge reboots into it 2 s later */
static int cart_on_firmware(void)
{
    return cart.online && str_eq((const char *)cart.view + CART_PAR_NAME, "FIRMWARE");
}
static void cart_draw(void)
{
    uint32_t row, n, k;
    int redraw = ui.force || cart.dirty;
    char b[32], num[12];
    static const char *const labels[4] = {"1 ENGINE", "2 PRESET", "3 PARAMETER", "4 VALUE"};
    static const uint8_t offsets[4] = {CART_ENG_NAME, CART_PRE_NAME, CART_PAR_NAME, CART_VAL_NAME};
#if FELUCCA_CARTRIDGE_DIAG
    int telemetry = cart_diag_sample();
    if (!redraw && !telemetry) return;
#else
    if (!redraw) return;
#endif
    cart.dirty = 0;
    /* Compose each strip off-screen, then replace it without a blank pass.
     * The strips cover every pixel, including gaps, on forced page entry. */
    if (redraw) {
        cv_begin(240, 26, C_BLACK);
        k = page_pos(&n);                                   /* "CARTRIDGE 2/10": numbered as the other GLO pages */
        str_cpy(b, "CARTRIDGE", sizeof b);
        if (n > 1u) {
            str_cpy(b + str_len(b), " ", 2); fmt_int(b + str_len(b), (int32_t)k);
            str_cpy(b + str_len(b), "/", 2); fmt_int(b + str_len(b), (int32_t)n);
        }
        cv_text(6, 3, &FONT_S, b, C_HI);
        if (cart.online) {                                  /* "<firmware> OK": what the cartridge runs */
            str_cpy(b, (const char *)cart.view + CART_FW_NAME, 12);
            if (!b[0]) str_cpy(b, "LINK", sizeof b);
            str_cpy(b + str_len(b), uar.active ? " OK" : " NO AUDIO", 10);   /* (the stream stopped: replug) */
        } else str_cpy(b, "NO LINK", sizeof b);
        cv_text(234 - text_w(&FONT_S, b), 3, &FONT_S, b, cart.online ? C_WHITE : C_DIM);
        cv_blit(0, 0);
        for (row = 0; row < 4u; row++) {
            cv_begin(240, CART_ROW_H, C_BLACK);
            cv_text(6, 1, &FONT_S, labels[row], TE_COL[row]);
            if (cart.online) {
                uint32_t pos = row == 0 ? cart.view[CART_ENG] : row == 1 ? cart_u14(cart.view + CART_PRE) :
                               row == 2 ? cart_u14(cart.view + CART_PAR) : (uint32_t)cart_s14(cart.view + CART_VAL);
                uint32_t total = row == 0 ? cart.view[CART_NENG] : row == 1 ? cart_u14(cart.view + CART_NPRE) :
                                 row == 2 ? cart_u14(cart.view + CART_NPAR) :
                                 (uint32_t)(cart_s14(cart.view + CART_MAX) + 1);   /* FIRMWARE: max is the last slot */
                if (row < 3u || cart_on_firmware()) {       /* the slots too: "2/3" on the FIRMWARE value */
                    fmt_int(num, (int32_t)pos + 1); str_cpy(num + str_len(num), "/", 2);
                    fmt_int(num + str_len(num), (int32_t)total);
                    cv_text(234 - text_w(&FONT_S, num), 1, &FONT_S, num, C_DIM);
                }
                str_cpy(b, (const char *)cart.view + offsets[row], sizeof b);
                if (row == 3u) {
                    str_cpy(b + str_len(b), " ", 2);
                    str_cpy(b + str_len(b), (const char *)cart.view + CART_UNIT, 8);
                }
            } else str_cpy(b, row ? "--" : "WAITING FOR CARTRIDGE", sizeof b);
            cv_text(6, 21, &FONT_S, b, C_WHITE);
            cv_rect(6, CART_ROW_H - 2u, 228, 1, C_LINE);
            cv_blit(0, 26u + row * CART_ROW_H);
        }
    }
    cv_begin(240, 240u - CART_FOOT_Y, C_BLACK);
#if FELUCCA_CARTRIDGE_DIAG
    for (row = 0; row < 2u; row++) {
        cart_diag_line(b, row);
        cv_text(6, 2u + row * 20u, &FONT_S, b, C_WHITE);
    }
#else
    cv_text(6, 8, &FONT_S, cart_on_firmware() ? CART_FOOT_FIRMWARE : CART_FOOT_PAGES, C_DIM);
#endif
    cv_blit(0, CART_FOOT_Y);
}
