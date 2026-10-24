/* SPDX-License-Identifier: GPL-3.0-only */
#include "cartridge_state.h"
#include <string.h>

static uint32_t fnv(uint32_t h, const char *s)
{
    for (; s && *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}
static uint32_t revision_of(const fm1_synth_api_t *s) { return fnv(fnv(2166136261u, s->name), s->revision); }
static size_t size_of(uint32_t count) { return offsetof(cartridge_state_t, entry) + count * sizeof(((cartridge_state_t *)0)->entry[0]); }
static uint16_t name_of(const fm1_param_t *d) { uint32_t h = fnv(2166136261u, d->name); return (uint16_t)(h ^ h >> 16); }

size_t cartridge_state_capture(const fm1_synth_api_t *s, cartridge_state_t *out)
{
    uint32_t n = s->param_count();
    if (!out || !n || n > CART_STATE_VALUES) return 0;
    memset(out, 0, sizeof *out);
    out->magic = CART_STATE_MAGIC;
    out->revision = revision_of(s);
    out->engine = (uint16_t)s->current_engine();
    out->preset = (uint16_t)s->current_preset();
    out->midi_channel = (uint16_t)s->midi_channel();
    out->count = (uint16_t)n;
    for (uint32_t i = 0; i < n; ++i) {
        fm1_param_t d;
        if (s->param_get(i, &d)) return 0;
        out->entry[i].value = d.value;
        out->entry[i].name = name_of(&d);
    }
    return size_of(n);
}

int cartridge_state_apply(const fm1_synth_api_t *s, const cartridge_state_t *in, size_t size)
{
    if (!in || size < offsetof(cartridge_state_t, entry) || in->magic != CART_STATE_MAGIC ||
        in->revision != revision_of(s) || in->count > CART_STATE_VALUES || size != size_of(in->count) ||
        in->engine >= s->engine_count() || in->preset >= s->preset_count(in->engine) || in->midi_channel > 16u)
        return -1;
    if (s->set_preset(in->engine, in->preset) || s->set_midi_channel(in->midi_channel)) return -1;
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t i = 1; i < in->count; ++i) {
            fm1_param_t d;
            int16_t v = in->entry[i].value;
            if (!s->param_get(i, &d) && name_of(&d) == in->entry[i].name && v >= d.min && v <= d.max && v != d.value)
                s->param_set(i, v);
        }
    return 0;
}

int cartridge_state_equal(const cartridge_state_t *a, size_t a_size, const cartridge_state_t *b, size_t b_size)
{
    return a && b && a_size == b_size && !memcmp(a, b, a_size);
}
