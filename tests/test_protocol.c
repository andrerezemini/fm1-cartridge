/* SPDX-License-Identifier: GPL-3.0-only */
#include "fm1/usb_midi.h"
#include "fm1/usb_descriptors.h"
#include "fm1/cartridge_wire.h"
#include "sloop_descriptor.h"
#include "sloop_cartridge_descriptor.h"
#include "../firmware/main/usb_host_fifo.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void midi(void)
{
    fm1_midi_event_t e;
    const uint8_t note[] = {0x29,0x93,60,100};
    assert(fm1_midi_decode(note,&e));
    assert(e.cable == 2 && e.status == 0x93 && e.data1 == 60 && e.data2 == 100 && e.length == 3);
    const uint8_t zero[] = {9,0x90,60,0};
    assert(fm1_midi_decode(zero,&e) && e.data2 == 0);
    const uint8_t off[] = {8,0x80,60,0};
    assert(fm1_midi_decode(off,&e));
    const uint8_t clock[] = {15,0xf8,0xff,0xff};
    assert(fm1_midi_decode(clock,&e) && e.length == 1 && !e.data1 && !e.data2);
    const uint8_t program[] = {12,0xc0,10,0xff};
    assert(fm1_midi_decode(program,&e) && e.length == 2 && !e.data2);
    const uint8_t bad[][4] = {{9,0x80,60,100},{9,0x90,128,100},
        {9,0x90,60,128},{0,0,0,0},{4,0xf0,1,2},{15,0xf9,0,0},{15,0x90,60,90}};
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) assert(!fm1_midi_decode(bad[i],&e));
    assert(!fm1_midi_decode(NULL,&e) && !fm1_midi_decode(note,NULL));
}

static void descriptors(void)
{
    fm1_usb_endpoints_t e;
    assert(sizeof(sloop_config) == 178);
    assert(fm1_usb_find_endpoints(sloop_config,sizeof(sloop_config),&e));
    assert(e.midi_in_found && e.midi_interface == 1 && e.midi_alt == 0);
    assert(e.midi_ep == 0x81 && e.midi_max_packet == 64);
    assert(e.midi_out_found && e.midi_out_ep == 1 && e.midi_out_max_packet == 64);
    assert(!e.audio_out_found); /* Unmodified upstream offers capture only. */
    for (size_t i=0;i<sizeof(sloop_config);i++) {
        assert(!fm1_usb_find_endpoints(sloop_config,i,&e));
        assert(!e.midi_in_found && !e.audio_out_found);
    }
    uint8_t b[sizeof(sloop_config)+16];
    memcpy(b,sloop_config,sizeof(sloop_config));
    b[9] = 0;
    assert(!fm1_usb_find_endpoints(b,sizeof(sloop_config),&e));
    b[9] = 1;
    assert(!fm1_usb_find_endpoints(b,sizeof(sloop_config),&e));
    memcpy(b,sloop_config,sizeof(sloop_config));
    /* Synthetic append ONLY tests endpoint discovery; not a valid UAC topology. */
    const uint8_t proposed[] = {9,4,3,1,1,1,2,0,0, 7,5,4,9,180,0,1};
    memcpy(b+sizeof(sloop_config),proposed,sizeof(proposed));
    b[2] = sizeof(b); b[3]=0; b[4]=4;
    assert(fm1_usb_find_endpoints(b,sizeof(b),&e));
    assert(e.audio_out_found && e.audio_ep == 4 && e.audio_interface == 3 && e.audio_alt == 1);
    b[sizeof(b)-3] = 0; /* illegal zero max packet */
    assert(!fm1_usb_find_endpoints(b,sizeof(b),&e));
    /* Exercise malformed chains with deterministic mutations under ASan. */
    for (size_t i=0;i<sizeof(sloop_config);i++) {
        memcpy(b,sloop_config,sizeof(sloop_config));
        b[i] ^= 0xff;
        (void)fm1_usb_find_endpoints(b,sizeof(sloop_config),&e);
    }
}

static void paired_midi(void)
{
    /* OUT precedes IN on interface 2; a different interface's OUT is ignored. */
    uint8_t d[] = {9,2,0,0,2,1,0,0x80,50,
        9,4,1,0,1,1,3,0,0, 7,5,2,2,64,0,0,
        9,4,2,0,2,1,3,0,0, 7,5,3,2,64,0,0, 7,5,0x83,2,64,0,0};
    d[2] = sizeof d;
    fm1_usb_endpoints_t e;
    assert(fm1_usb_find_endpoints(d, sizeof d, &e));
    assert(e.midi_interface == 2 && e.midi_out_found && e.midi_out_ep == 3);
    d[9 + 16 + 9 + 2] = 0x84; /* now no OUT on selected interface */
    assert(fm1_usb_find_endpoints(d, sizeof d, &e) && !e.midi_out_found);
}

static void cartridge_descriptors(void)
{
    fm1_usb_endpoints_t e;
    assert(sizeof(sloop_cartridge_config) == 175);
    assert(fm1_usb_find_endpoints(sloop_cartridge_config, sizeof(sloop_cartridge_config), &e));
    assert(e.midi_in_found && e.midi_interface == 1 && e.midi_alt == 0);
    assert(e.midi_ep == 0x81 && e.midi_max_packet == 64);
    assert(e.midi_out_found && e.midi_out_ep == 1 && e.midi_out_max_packet == 64);
    assert(e.audio_out_found && e.audio_interface == 2 && e.audio_alt == 1);
    assert(e.audio_ep == 0x04 && e.audio_max_packet == 180);
    /* The real Sloop endpoint must fit the host hardware, not just parse.
     * An undersized periodic FIFO still allows SET_CONFIGURATION/MIDI but
     * IDF rejects the UAC interface before any PCM can be transmitted. */
    assert(e.audio_max_packet <= FM1_USB_PERIODIC_OUT_MAX_PACKET);
    assert(e.midi_max_packet <= FM1_USB_IN_MAX_PACKET);
    assert(64 <= FM1_USB_NON_PERIODIC_OUT_MAX_PACKET); /* EP0 control */
    for (size_t i = 0; i < sizeof(sloop_cartridge_config); i++) {
        assert(!fm1_usb_find_endpoints(sloop_cartridge_config, i, &e));
        assert(!e.midi_in_found && !e.audio_out_found);
    }
}

/* A newer peer's request (a later version byte, fields appended) reads as this version's; an older
 * version, a reply, or a broken frame does not. */
static void cartridge_versions(void)
{
    uint8_t q[CART_REQUEST_SIZE], out[CART_REQUEST_SIZE], longer[CART_REQUEST_SIZE + 3];
    cart_header(q, 77, CART_VALUE, 5); cart_put14(q + CART_ARG, 8192 + 2); q[CART_REQUEST_SIZE - 1] = 0xf7;
    assert(cart_request_of(q, sizeof q, out) && !memcmp(out, q, sizeof q));
    memcpy(longer, q, CART_REQUEST_SIZE - 1); longer[4] = CART_VERSION + 1;
    longer[CART_REQUEST_SIZE - 1] = 0x11; longer[CART_REQUEST_SIZE] = 0x22; longer[CART_REQUEST_SIZE + 1] = 0x33;
    longer[CART_REQUEST_SIZE + 2] = 0xf7;
    assert(cart_request_of(longer, sizeof longer, out) && out[4] == CART_VERSION + 1 &&
           !memcmp(out + 5, q + 5, CART_REQUEST_SIZE - 6) && out[CART_REQUEST_SIZE - 1] == 0xf7);
    longer[4] = CART_VERSION - 1; assert(!cart_request_of(longer, sizeof longer, out));
    longer[4] = CART_VERSION + 1; longer[CART_REQUEST_SIZE] = 0x80; assert(!cart_request_of(longer, sizeof longer, out));
    q[CART_OP] = CART_REPLY; assert(!cart_request_of(q, sizeof q, out));
    assert(!cart_request_of(q, sizeof q - 1, out));
}
int main(void) { midi(); descriptors(); paired_midi(); cartridge_descriptors(); cartridge_versions(); puts("protocol: ok"); return 0; }
