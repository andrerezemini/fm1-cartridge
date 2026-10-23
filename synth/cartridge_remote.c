/* SPDX-License-Identifier: GPL-3.0-only */
#include "cartridge_remote.h"
#include <string.h>
static int bounded(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void label(uint8_t *p, const char *s, size_t capacity)
{
    if (!s) return;
    for (size_t i = 0; i + 1 < capacity && s[i]; ++i)
        p[i] = (uint8_t)s[i] >= 32 && (uint8_t)s[i] < 127 ? (uint8_t)s[i] : '?';
}
static void copy_text(char *out, const char *s, size_t capacity)
{
    size_t i = 0;
    if (s) for (; i + 1 < capacity && s[i]; ++i) out[i] = s[i];
    out[i] = 0;
}
static int has_slots(const cartridge_remote_t *r) { return r->slots && r->slots->count() > 1u; }
void cartridge_remote_init(cartridge_remote_t *r, const fm1_synth_api_t *synth, const fm1_slots_t *slots, uint16_t seed)
{
    memset(r, 0, sizeof *r);
    r->synth = synth; r->slots = slots; r->revision = seed & 16383u;
    r->slot_cursor = has_slots(r) ? slots->current() : 0;
}
uint32_t cartridge_remote_param_count(const cartridge_remote_t *r)
{
    uint32_t n = r->synth->param_count();
    return n ? n + (has_slots(r) ? 1u : 0u) : 0u;
}
int cartridge_remote_param_get(const cartridge_remote_t *r, uint32_t index, fm1_param_t *out)
{
    if (!out) return -1;
    if (has_slots(r)) {
        if (index == 1) {
            uint8_t count = r->slots->count();
            if (count > CART_SLOTS_MAX) count = CART_SLOTS_MAX;
            memset(out, 0, sizeof *out);
            out->min = 0; out->max = (int16_t)(count - 1u); out->value = (int16_t)r->slot_cursor;
            copy_text(out->name, "FIRMWARE", sizeof out->name);
            copy_text(out->text, r->slots->name(r->slot_cursor), sizeof out->text);
            copy_text(out->unit, r->slot_cursor == r->slots->current() ? "CURRENT" : "IN 2 S", sizeof out->unit);
            return 0;
        }
        if (index > 1) index--;
    }
    return r->synth->param_get(index, out);
}
int cartridge_remote_param_set(cartridge_remote_t *r, uint32_t index, int32_t value)
{
    if (has_slots(r)) {
        if (index == 1) {
            uint8_t count = r->slots->count();
            if (count > CART_SLOTS_MAX) count = CART_SLOTS_MAX;
            if (value < 0 || value >= (int32_t)count) return -1;
            /* an empty slot is shown but never armed: the knob passes over it */
            if ((uint8_t)value != r->slots->current() && !r->slots->bootable((uint8_t)value)) return -1;
            if ((uint8_t)value != r->slot_cursor) { r->slot_cursor = (uint8_t)value; r->slot_dirty = 1; }
            return 0;
        }
        if (index > 1) index--;
    }
    return r->synth->param_set(index, value);
}
int cartridge_remote_handle(cartridge_remote_t *r, const uint8_t *q, size_t n, uint8_t p[CART_REPLY_SIZE])
{
    const fm1_synth_api_t *s = r ? r->synth : NULL;
    if (!r || !s || !p || n != CART_REQUEST_SIZE || !cart_valid(q, n) || q[CART_OP] > CART_VALUE) return 0;
    if (r->cached && !memcmp(r->request, q, n)) { memcpy(p, r->reply, CART_REPLY_SIZE); return 1; }
    unsigned engine = s->current_engine(), preset = s->current_preset();
    int delta = cart_s14(q + CART_ARG), status = CART_OK;
    unsigned op = q[CART_OP];
    if (op && cart_u14(q + CART_REV) != r->revision) status = CART_STALE;
    else if (op) {
        if (op == CART_ENGINE) {
            unsigned e = (unsigned)bounded((int)engine + delta, 0, (int)s->engine_count() - 1);
            if (e != engine) { s->set_preset(e, 0); r->parameter = 0; }
        } else if (op == CART_PRESET) {
            unsigned pr = (unsigned)bounded((int)preset + delta, 0, (int)s->preset_count(engine) - 1);
            if (pr != preset) { s->set_preset(engine, pr); r->parameter = 0; }
        } else if (op == CART_PARAM) {
            r->parameter = (uint16_t)bounded((int)r->parameter + delta, 0, (int)cartridge_remote_param_count(r) - 1);
        } else {
            fm1_param_t d;
            if (cartridge_remote_param_get(r, r->parameter, &d)) status = CART_BAD_REQUEST;
            else {
                int target = bounded((int)d.value + delta, d.min, d.max);
                /* FIRMWARE: step over slots that cannot boot, in the knob's direction */
                while (cartridge_remote_param_set(r, r->parameter, target) && target != d.value)
                    target += delta < 0 ? 1 : -1;
                if (target == d.value && delta) status = CART_BAD_REQUEST;
            }
        }
        r->revision = (r->revision + 1) & 16383u;
    }
    unsigned count = cartridge_remote_param_count(r);
    if (!count) return 0;
    if (r->parameter >= count) r->parameter = 0;
    fm1_param_t d;
    if (cartridge_remote_param_get(r, r->parameter, &d)) return 0;
    engine = s->current_engine(); preset = s->current_preset();
    memset(p, 0, CART_REPLY_SIZE);
    cart_header(p, cart_u14(q + CART_SEQ), CART_REPLY, r->revision);
    p[CART_STATUS] = (uint8_t)status;
    p[CART_ENG] = (uint8_t)engine; p[CART_NENG] = (uint8_t)s->engine_count();
    cart_put14(p + CART_PRE, (uint16_t)preset); cart_put14(p + CART_NPRE, (uint16_t)s->preset_count(engine));
    cart_put14(p + CART_PAR, r->parameter); cart_put14(p + CART_NPAR, count);
    cart_put14(p + CART_VAL, d.value + 8192); cart_put14(p + CART_MIN, d.min + 8192);
    cart_put14(p + CART_MAX, d.max + 8192);
    label(p + CART_ENG_NAME, s->engine_name(engine), 12);
    label(p + CART_PRE_NAME, s->preset_name(engine, preset), 24);
    label(p + CART_PAR_NAME, d.name, 20); label(p + CART_VAL_NAME, d.text, 16);
    label(p + CART_UNIT, d.unit, 8);
    label(p + CART_FW_NAME, s->name, 12);
    p[CART_SLOT] = r->slots ? r->slots->current() : 0;
    p[CART_NSLOT] = r->slots ? r->slots->count() : 1;
    p[CART_MIDI_CH] = (uint8_t)(s->midi_channel() <= 16u ? s->midi_channel() : 0u);
    p[CART_REPLY_SIZE - 1] = 0xf7;
    memcpy(r->request, q, n); memcpy(r->reply, p, CART_REPLY_SIZE); r->cached = 1;
    return 1;
}
