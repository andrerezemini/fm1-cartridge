/* SPDX-License-Identifier: GPL-3.0-only
 * Cartridge return: real usb.c against an indexed-SIE/DMA fake. No silicon
 * behavior, acoustic output or DMA timing is asserted by this host test. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef T_CAPTURE
#define T_CAPTURE 0
#endif
#ifndef T_RX
#define T_RX 1
#endif
#ifndef T_CDC
#define T_CDC 0
#endif
#ifndef T_RATE
#define T_RATE 0
#endif
#define HALF_FRAMES 256
#define FELUCCA_UAC T_CAPTURE
#define FELUCCA_UAC_RX T_RX
#define FELUCCA_CDC (T_CDC != 0)
#define FELUCCA_OTA 0
#define FM1_USB_TEST 1
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
static uint32_t fm1_ms;
static uint8_t globals[16], banks[5][32], index_now, read_result;
static uint8_t response[64];
static uint32_t response_n, rx_syncs, rx_copies, rx_bytes, rx_resets, rx_errors, rx_ticks;
static uint8_t rx_active;
static void *dma_rx4;
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)
static void fm1_delay_ms(uint32_t n) { (void)n; }
static void fm1_usb_reset(void) { memset(globals, 0, sizeof globals); memset(banks, 0, sizeof banks); }
static void fm1_usb_attach(void *p) { (void)p; }
static void fm1_usb_off(void) {}
static uint32_t fm1_usb_sie_on(void) { return 1; }
static void fm1_usb_sie_wr_start(uint32_t r, uint32_t v)
{
    if (r == 14) { index_now = (uint8_t)v; CHECK(v < 5); return; }
    if (r < 16) { globals[r] = (uint8_t)v; return; }
    CHECK(r < 32);
    if (index_now == 0 && r == 17) {
        if (v & 0x40u) banks[0][17] &= ~1u;
        if (v & 0x80u) banks[0][17] &= ~0x10u;
        if (v & 0x20u) banks[0][17] |= 4u; /* SendStall -> SentStall */
        if (v & 2u) banks[0][17] |= 2u;
        if (!v) banks[0][17] = 0;
    } else if (r == 20 && (v & 0x10u)) {
        banks[index_now][20] = 0; /* FlushFIFO consumes ready/error status */
        banks[index_now][22] = banks[index_now][23] = 0;
    } else banks[index_now][r] = (uint8_t)v;
}
static void fm1_usb_sie_rd_start(uint32_t r)
{
    read_result = r < 16 ? globals[r] : banks[index_now][r];
    if (r >= 2 && r <= 6) globals[r] = 0; /* read-to-clear interrupts */
}
static uint32_t fm1_usb_sie_done(void) { return 1; }
static uint32_t fm1_usb_sie_data(void) { return read_result; }
static void fm1_usb_ep0_buf(void *p) { (void)p; }
static void fm1_usb_ep_txbuf(uint32_t ep, void *p) { (void)ep; (void)p; }
static void fm1_usb_ep_rxbuf(uint32_t ep, void *p) { (void)ep; (void)p; }
static void fm1_usb_ep0_send(void *p, uint32_t n) { CHECK(n <= 64); memcpy(response, p, n); response_n = n; }
static void fm1_usb_ep_send(uint32_t ep, void *p, uint32_t n) { (void)ep; (void)p; (void)n; }
static void fm1_usb_ep4_txbuf(void *p) { (void)p; }
static void fm1_usb_ep4_send(void *p, uint32_t n) { (void)p; (void)n; }
static void fm1_usb_ep4_rxbuf(void *p) { dma_rx4 = p; }
static void fm1_usb_rx_sync(void) { rx_syncs++; }
static void fm1_usb_ep_enable(uint32_t n) { (void)n; }
static uint32_t fm1_usb_sof_take(void) { return 1; }
/* The receiver has its own sample/ring tests. Here assert transport boundaries. */
static void uac_rx_stream(uint32_t alt) { rx_active = (uint8_t)alt; rx_resets++; }
static void uac_rx_push(const uint8_t *p, uint32_t n)
{
    CHECK(rx_syncs != 0); CHECK(p == dma_rx4); CHECK(rx_active);
    rx_copies++; rx_bytes = n; rx_syncs = 0;
}
static void uac_rx_tick(uint32_t t) { CHECK(t == fm1_ms); rx_ticks++; }
static void uac_rx_packet_error(void) { rx_errors++; }
#include "../firmware/src/usb.c"

static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static void descriptors(void)
{
    const uint8_t *c, *d; uint16_t n, dn;
    uint32_t off = 0, seen_if = 0, eps = 0, acbytes = 0, aclen = 0;
    int iface = -1, alt = -1, expected_ep = 0, actual_ep = 0;
    int capture = 0, playback = 0, terminal3 = 0, terminal4 = 0, playback_fmt = 0;
#if FELUCCA_CDC
    usb_cdc_on = T_CDC == 1;
#endif
#if FELUCCA_UAC_48K
    usb_rate_mode = T_RATE;
#endif
    CHECK(get_desc(0x100, &d, &dn)); CHECK(get_desc(0x200, &c, &n));
    CHECK(dn == 18 && d[13] == 3 + T_RX);
    CHECK(n == le16(c+2)); CHECK(c[4] == 2 + T_CAPTURE + T_RX + 2*(T_CDC == 1));
    if (getenv("UAC_DUMP")) {
        for (uint32_t i=0;i<dn;i++) printf("%02X",d[i]); puts("");
        for (uint32_t i=0;i<n;i++) printf("%02X",c[i]); puts("");
        exit(0);
    }
    while (off < n) {
        const uint8_t *p = c + off;
        CHECK(p[0] >= 2 && off+p[0] <= n); if (p[0]<2 || off+p[0]>n) break;
        if (p[1] == 4) {
            if (iface >= 0) CHECK(actual_ep == expected_ep);
            iface=p[2]; alt=p[3]; expected_ep=p[4]; actual_ep=0;
            seen_if |= 1u << iface;
        }
        if (p[1] == 0x0B && p[2] == 0) CHECK(p[3] == 2 + T_CAPTURE + T_RX);
        if (p[1] == 0x24 && iface == 0) {
            acbytes += p[0];
            if (p[2] == 1) {
                aclen=le16(p+5);
                CHECK(p[0] == 9 + T_CAPTURE + T_RX && p[7] == 1 + T_CAPTURE + T_RX);
                CHECK(p[8] == 1);
                for (uint32_t i=0;i<(uint32_t)(T_CAPTURE+T_RX);i++) CHECK(p[9+i] == 2+i);
            }
            if (p[2] == 2 && p[3] == 3) { terminal3++; CHECK(le16(p+4)==0x101 && p[7]==2 && le16(p+8)==3); }
            if (p[2] == 3 && p[3] == 4) { terminal4++; CHECK(le16(p+4)==0x301 && p[7]==3); }
        }
        if (p[1] == 0x24 && iface == 2+T_CAPTURE && T_RX && alt == 1) {
            if (p[2] == 1) CHECK(p[3]==3 && le16(p+5)==1);
            if (p[2] == 2) { playback_fmt++; CHECK(p[0]==11 && p[4]==2 && p[5]==2 && p[6]==16 && p[7]==1 && p[8]==0x44 && p[9]==0xAC && p[10]==0); }
        }
        if (p[1] == 5) {
            uint32_t bit=(p[2]&15u)+((p[2]&0x80u)?16u:0u);
            actual_ep++; CHECK(!(eps & (1u<<bit))); eps |= 1u<<bit;
            if (p[2]==4) { playback++; CHECK(iface==2+T_CAPTURE && alt==1 && p[3]==9 && le16(p+4)==180 && p[6]==1 && p[7]==0 && p[8]==0); }
            if (p[2]==0x84) { capture++; CHECK(T_CAPTURE && p[3]==5); }
        }
        off += p[0];
    }
    CHECK(off==n && actual_ep==expected_ep);
    CHECK(acbytes==aclen); CHECK(seen_if == (1u<<c[4])-1);
    CHECK(playback==T_RX && playback_fmt==T_RX && capture==T_CAPTURE && terminal3==T_RX && terminal4==T_RX);
}
static void setup(uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
    usb.e0_tx = 0;
    response_n = 0;
    ep0buf[0]=bm; ep0buf[1]=req; ep0buf[2]=(uint8_t)val; ep0buf[3]=(uint8_t)(val>>8);
    ep0buf[4]=(uint8_t)idx; ep0buf[5]=(uint8_t)(idx>>8); ep0buf[6]=(uint8_t)len; ep0buf[7]=(uint8_t)(len>>8);
    banks[0][17]=1; banks[0][22]=8;
    ep0_service();
}
static int stalled(void) { return (banks[0][17]&4u)!=0; }
static void control(void)
{
    setup(0,9,1,0,0); CHECK(!stalled() && usb.config==1);
    setup(1,11,0,99,0); CHECK(stalled());
    setup(0x81,10,0,99,1); CHECK(stalled());
    setup(2,1,0,5,0); CHECK(stalled());
    setup(2,1,0,2,0); CHECK(stalled()); /* no CDC notify OUT endpoint */
    setup(2,1,0,0x91,0); CHECK(stalled()); /* reserved address bits */
#if T_RX
    CHECK(dma_rx4==ep4rx && !rx_active);
    CHECK(banks[4][21]==0x40 && banks[4][19]==0xFF);
    setup(1,11,1,UAC_RX_IF,0); CHECK(!stalled() && rx_active && uac_rx_usb.alt==1);
    setup(0x81,10,0,UAC_RX_IF,1); CHECK(response_n==1 && response[0]==1);
    setup(1,11,2,UAC_RX_IF,0); CHECK(stalled());
    setup(1,11,1,0x100+UAC_RX_IF,0); CHECK(stalled());
    setup(2,1,0,4,0); CHECK(!stalled() && rx_active);
    for (uint32_t req=0x81;req<=0x84;req++) {
        setup(0xA2,(uint8_t)req,0x100,4,3);
        CHECK(!stalled() && response_n==3);
        CHECK(response[0]==(req==0x84?0:0x44) && response[1]==(req==0x84?0:0xAC) && response[2]==0);
    }
    setup(0x22,1,0x100,4,3); CHECK(!stalled() && uac_rx_usb.e0_rx);
    ep0buf[0]=0x44; ep0buf[1]=0xAC; ep0buf[2]=0; banks[0][17]=1; banks[0][22]=3;
    ep0_service(); CHECK(!stalled() && uac_rx_usb.rate_sets==1);
    setup(0x22,1,0x100,4,3);
    ep0buf[0]=0x80; ep0buf[1]=0xBB; ep0buf[2]=0; banks[0][17]=1; banks[0][22]=3;
    ep0_service(); CHECK(stalled() && uac_rx_usb.rate_rejected==1);
    setup(0x22,1,0x100,4,3);
    banks[0][17]=1; banks[0][22]=2;
    ep0_service(); CHECK(stalled() && !uac_rx_usb.e0_rx);
    setup(0x22,1,0x100,4,3);
    banks[0][17]=0x10; ep0_service(); CHECK(!uac_rx_usb.e0_rx); /* abandoned SET_CUR */
    setup(0x22,1,0x100,4,0); CHECK(stalled());
    setup(0xA2,0x81,0x101,4,3); CHECK(stalled());
    setup(0xA2,0x81,0x100,0x104,3); CHECK(stalled());
    setup(1,11,0,UAC_RX_IF,0); CHECK(!rx_active);
#else
    setup(0xA2,0x81,0x100,4,3); CHECK(stalled());
    setup(2,1,0,4,0); CHECK(stalled());
#endif
    setup(0,9,0,0,0); CHECK(!usb.config && !rx_active);
    setup(1,11,0,1,0); CHECK(stalled());
    setup(0,9,0x101,0,0); CHECK(stalled());
}
#if T_RX
static void rx_packet(uint32_t n, uint32_t errors)
{
    banks[4][20]=(uint8_t)(1u|errors); banks[4][21]=(uint8_t)(0x40u|(errors>>8));
    banks[4][22]=(uint8_t)n; banks[4][23]=(uint8_t)(n>>8);
}
static void transport(void)
{
    usb.up=1;
    setup(0,9,1,0,0); setup(1,11,1,UAC_RX_IF,0);
    uint32_t before=rx_copies;
    rx_packet(176,0); uac_rx_service(); CHECK(rx_copies==before+1 && rx_bytes==176 && !(banks[4][20]&1) && banks[4][21]==0x40);
    uac_rx_service(); CHECK(rx_copies==before+1); /* once only */
    rx_packet(180,0); uac_rx_service(); CHECK(rx_copies==before+2 && rx_bytes==180);
    rx_packet(0,0); uac_rx_service(); CHECK(rx_copies==before+3 && rx_bytes==0);
    rx_packet(181,0); uac_rx_service(); CHECK(rx_bytes==181); /* receiver rejects before read */
    rx_packet(1023,0); uac_rx_service(); CHECK(rx_bytes==1023 && sizeof ep4rx>=1023);
    before=rx_copies;
    /* OverRun: a newer packet was lost, the buffered one is intact and is still read. */
    rx_packet(176,4); uac_rx_service(); CHECK(rx_errors==1 && rx_copies==before+1 && !(banks[4][20]&1));
    /* DataError / IncompRx: this packet is corrupt, skipped and flushed. */
    rx_packet(176,8); uac_rx_service(); CHECK(rx_errors==2 && rx_copies==before+1 && !(banks[4][20]&1));
    rx_packet(176,0x100); uac_rx_service(); CHECK(rx_errors==3 && rx_copies==before+1 && banks[4][21]==0x40);
    banks[4][20]=0; banks[4][21]=0x40;
    before=rx_resets;
    for (int i=0;i<4;i++) usb_poll();
    CHECK(rx_resets==before && rx_active); /* SOF is not a receiver reset */
    globals[6]=1; usb_poll(); CHECK(!rx_active && uac_rx_usb.paused);
    globals[6]=2; usb_poll(); CHECK(rx_active && !uac_rx_usb.paused);
    globals[6]=4; usb_poll(); CHECK(!rx_active && !uac_rx_usb.alt && !usb.config);
    setup(0,9,1,0,0); setup(1,11,1,UAC_RX_IF,0);
    usb_detach(); CHECK(!rx_active && !uac_rx_usb.alt);
}
#endif
int main(void)
{
    descriptors();
    if (getenv("UAC_RX_DUMP")) {
        FILE *f = fopen(getenv("UAC_RX_DUMP"), "wb");
        CHECK(f != NULL);
        setup(0x80, 6, 0x200, 0, 0xFFFF);
        do {
            if (f) CHECK(fwrite(response, 1, response_n, f) == response_n);
            if (!usb.e0_tx) break;
            banks[0][17] &= ~2u;
            ep0_service();
        } while (1);
        if (f) CHECK(fclose(f) == 0);
        return failures != 0;
    }
    control();
#if T_RX
    transport();
#endif
    printf("USB RX=%d capture=%d CDC=%d rate=%d: %s\n",T_RX,T_CAPTURE,T_CDC,T_RATE,failures?"FAILED":"ok");
    return failures!=0;
}
