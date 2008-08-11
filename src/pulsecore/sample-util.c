/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <liboil/liboilfuncs.h>
#include <liboil/liboil.h>

#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/g711.h>

#include "sample-util.h"
#include "endianmacros.h"

#define PA_SILENCE_MAX (PA_PAGE_SIZE*16)

pa_memblock *pa_silence_memblock(pa_memblock* b, const pa_sample_spec *spec) {
    void *data;

    pa_assert(b);
    pa_assert(spec);

    data = pa_memblock_acquire(b);
    pa_silence_memory(data, pa_memblock_get_length(b), spec);
    pa_memblock_release(b);

    return b;
}

pa_memchunk* pa_silence_memchunk(pa_memchunk *c, const pa_sample_spec *spec) {
    void *data;

    pa_assert(c);
    pa_assert(c->memblock);
    pa_assert(spec);

    data = pa_memblock_acquire(c->memblock);
    pa_silence_memory((uint8_t*) data+c->index, c->length, spec);
    pa_memblock_release(c->memblock);

    return c;
}

static uint8_t silence_byte(pa_sample_format_t format) {
    switch (format) {
        case PA_SAMPLE_U8:
            return 0x80;
        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_S32LE:
        case PA_SAMPLE_S32BE:
        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_FLOAT32BE:
            return 0;
        case PA_SAMPLE_ALAW:
            return 0xd5;
        case PA_SAMPLE_ULAW:
            return 0xff;
        default:
            pa_assert_not_reached();
    }
    return 0;
}

void* pa_silence_memory(void *p, size_t length, const pa_sample_spec *spec) {
    pa_assert(p);
    pa_assert(length > 0);
    pa_assert(spec);

    memset(p, silence_byte(spec->format), length);
    return p;
}

static void calc_linear_integer_stream_volumes(pa_mix_info streams[], unsigned nstreams, const pa_sample_spec *spec) {
    unsigned k;

    pa_assert(streams);
    pa_assert(spec);

    for (k = 0; k < nstreams; k++) {
        unsigned channel;

        for (channel = 0; channel < spec->channels; channel++) {
            pa_mix_info *m = streams + k;
            m->linear[channel].i = (int32_t) (pa_sw_volume_to_linear(m->volume.values[channel]) * 0x10000);
        }
    }
}

static void calc_linear_integer_volume(int32_t linear[], const pa_cvolume *volume) {
    unsigned channel;

    pa_assert(linear);
    pa_assert(volume);

    for (channel = 0; channel < volume->channels; channel++)
        linear[channel] = (int32_t) (pa_sw_volume_to_linear(volume->values[channel]) * 0x10000);
}

static void calc_linear_float_stream_volumes(pa_mix_info streams[], unsigned nstreams, const pa_sample_spec *spec) {
    unsigned k;

    pa_assert(streams);
    pa_assert(spec);

    for (k = 0; k < nstreams; k++) {
        unsigned channel;

        for (channel = 0; channel < spec->channels; channel++) {
            pa_mix_info *m = streams + k;
            m->linear[channel].f = pa_sw_volume_to_linear(m->volume.values[channel]);
        }
    }
}

static void calc_linear_float_volume(float linear[], const pa_cvolume *volume) {
    unsigned channel;

    pa_assert(linear);
    pa_assert(volume);

    for (channel = 0; channel < volume->channels; channel++)
        linear[channel] = pa_sw_volume_to_linear(volume->values[channel]);
}

size_t pa_mix(
        pa_mix_info streams[],
        unsigned nstreams,
        void *data,
        size_t length,
        const pa_sample_spec *spec,
        const pa_cvolume *volume,
        pa_bool_t mute) {

    pa_cvolume full_volume;
    unsigned k;
    size_t d = 0;

    pa_assert(streams);
    pa_assert(data);
    pa_assert(length);
    pa_assert(spec);

    if (!volume)
        volume = pa_cvolume_reset(&full_volume, spec->channels);

    for (k = 0; k < nstreams; k++)
        streams[k].ptr = (uint8_t*) pa_memblock_acquire(streams[k].chunk.memblock) + streams[k].chunk.index;

    switch (spec->format) {

        case PA_SAMPLE_S16NE:{
            unsigned channel = 0;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_stream_volumes(streams, nstreams, spec);
            calc_linear_integer_volume(linear, volume);

            for (d = 0;; d += sizeof(int16_t)) {
                int32_t sum = 0;
                unsigned i;

                if (PA_UNLIKELY(d >= length))
                    goto finish;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t v, cv = m->linear[channel].i;

                    if (PA_UNLIKELY(d >= m->chunk.length))
                        goto finish;

                    if (PA_UNLIKELY(cv <= 0) || PA_UNLIKELY(!!mute) || PA_UNLIKELY(linear[channel] <= 0))
                        v = 0;
                    else {
                        v = *((int16_t*) m->ptr);
                        v = (v * cv) / 0x10000;
                    }

                    sum += v;
                    m->ptr = (uint8_t*) m->ptr + sizeof(int16_t);
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
                sum = (sum * linear[channel]) / 0x10000;
                *((int16_t*) data) = (int16_t) sum;

                data = (uint8_t*) data + sizeof(int16_t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_S16RE:{
            unsigned channel = 0;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_stream_volumes(streams, nstreams, spec);
            calc_linear_integer_volume(linear, volume);

            for (d = 0;; d += sizeof(int16_t)) {
                int32_t sum = 0;
                unsigned i;

                if (PA_UNLIKELY(d >= length))
                    goto finish;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t v, cv = m->linear[channel].i;

                    if (PA_UNLIKELY(d >= m->chunk.length))
                        goto finish;

                    if (PA_UNLIKELY(cv <= 0) || PA_UNLIKELY(!!mute) || PA_UNLIKELY(linear[channel] <= 0))
                        v = 0;
                    else {
                        v = PA_INT16_SWAP(*((int16_t*) m->ptr));
                        v = (v * cv) / 0x10000;
                    }

                    sum += v;
                    m->ptr = (uint8_t*) m->ptr + sizeof(int16_t);
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
                sum = (sum * linear[channel]) / 0x10000;
                *((int16_t*) data) = PA_INT16_SWAP((int16_t) sum);

                data = (uint8_t*) data + sizeof(int16_t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_S32NE:{
            unsigned channel = 0;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_stream_volumes(streams, nstreams, spec);
            calc_linear_integer_volume(linear, volume);

            for (d = 0;; d += sizeof(int32_t)) {
                int64_t sum = 0;
                unsigned i;

                if (PA_UNLIKELY(d >= length))
                    goto finish;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int64_t v;
                    int32_t cv = m->linear[channel].i;

                    if (PA_UNLIKELY(d >= m->chunk.length))
                        goto finish;

                    if (PA_UNLIKELY(cv <= 0) || PA_UNLIKELY(!!mute) || PA_UNLIKELY(linear[channel] <= 0))
                        v = 0;
                    else {
                        v = *((int32_t*) m->ptr);
                        v = (v * cv) / 0x10000;
                    }

                    sum += v;
                    m->ptr = (uint8_t*) m->ptr + sizeof(int32_t);
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x80000000LL, 0x7FFFFFFFLL);
                sum = (sum * linear[channel]) / 0x10000;
                *((int32_t*) data) = (int32_t) sum;

                data = (uint8_t*) data + sizeof(int32_t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_S32RE:{
            unsigned channel = 0;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_stream_volumes(streams, nstreams, spec);
            calc_linear_integer_volume(linear, volume);

            for (d = 0;; d += sizeof(int32_t)) {
                int64_t sum = 0;
                unsigned i;

                if (PA_UNLIKELY(d >= length))
                    goto finish;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int64_t v;
                    int32_t cv = m->linear[channel].i;

                    if (PA_UNLIKELY(d >= m->chunk.length))
                        goto finish;

                    if (PA_UNLIKELY(cv <= 0) || PA_UNLIKELY(!!mute) || PA_UNLIKELY(linear[channel] <= 0))
                        v = 0;
                    else {
                        v = PA_INT32_SWAP(*((int32_t*) m->ptr));
                        v = (v * cv) / 0x10000;
                    }

                    sum += v;
                    m->ptr = (uint8_t*) m->ptr + sizeof(int32_t);
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x80000000LL, 0x7FFFFFFFLL);
                sum = (sum * linear[channel]) / 0x10000;
                *((int32_t*) data) = PA_INT32_SWAP((int32_t) sum);

                data = (uint8_t*) data + sizeof(int32_t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_U8: {
            unsigned channel = 0;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_stream_volumes(streams, nstreams, spec);
            calc_linear_integer_volume(linear, volume);

            for (d = 0;; d ++) {
                int32_t sum = 0;
                unsigned i;

                if (PA_UNLIKELY(d >= length))
                    goto finish;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t v, cv = m->linear[channel].i;

                    if (PA_UNLIKELY(d >= m->chunk.length))
                        goto finish;

                    if (PA_UNLIKELY(cv <= 0) || PA_UNLIKELY(!!mute) || PA_UNLIKELY(linear[channel] <= 0))
                        v = 0;
                    else {
                        v = (int32_t) *((uint8_t*) m->ptr) - 0x80;
                        v = (v * cv) / 0x10000;
                    }

                    sum += v;
                    m->ptr = (uint8_t*) m->ptr + 1;
                }

                sum = (sum * linear[channel]) / 0x10000;
                sum = PA_CLAMP_UNLIKELY(sum, -0x80, 0x7F);
                *((uint8_t*) data) = (uint8_t) (sum + 0x80);

                data = (uint8_t*) data + 1;

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_ULAW: {
            unsigned channel = 0;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_stream_volumes(streams, nstreams, spec);
            calc_linear_integer_volume(linear, volume);

            for (d = 0;; d ++) {
                int32_t sum = 0;
                unsigned i;

                if (PA_UNLIKELY(d >= length))
                    goto finish;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t v, cv = m->linear[channel].i;

                    if (PA_UNLIKELY(d >= m->chunk.length))
                        goto finish;

                    if (PA_UNLIKELY(cv <= 0) || PA_UNLIKELY(!!mute) || PA_UNLIKELY(linear[channel] <= 0))
                        v = 0;
                    else {
                        v = (int32_t) st_ulaw2linear16(*((uint8_t*) m->ptr));
                        v = (v * cv) / 0x10000;
                    }

                    sum += v;
                    m->ptr = (uint8_t*) m->ptr + 1;
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
                sum = (sum * linear[channel]) / 0x10000;
                *((uint8_t*) data) = (uint8_t) st_14linear2ulaw(sum >> 2);

                data = (uint8_t*) data + 1;

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_ALAW: {
            unsigned channel = 0;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_stream_volumes(streams, nstreams, spec);
            calc_linear_integer_volume(linear, volume);

            for (d = 0;; d ++) {
                int32_t sum = 0;
                unsigned i;

                if (PA_UNLIKELY(d >= length))
                    goto finish;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    int32_t v, cv = m->linear[channel].i;

                    if (PA_UNLIKELY(d >= m->chunk.length))
                        goto finish;

                    if (PA_UNLIKELY(cv <= 0) || PA_UNLIKELY(!!mute) || PA_UNLIKELY(linear[channel] <= 0))
                        v = 0;
                    else {
                        v = (int32_t) st_alaw2linear16(*((uint8_t*) m->ptr));
                        v = (v * cv) / 0x10000;
                    }

                    sum += v;
                    m->ptr = (uint8_t*) m->ptr + 1;
                }

                sum = PA_CLAMP_UNLIKELY(sum, -0x8000, 0x7FFF);
                sum = (sum * linear[channel]) / 0x10000;
                *((uint8_t*) data) = (uint8_t) st_13linear2alaw(sum >> 3);

                data = (uint8_t*) data + 1;

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_FLOAT32NE: {
            unsigned channel = 0;
            float linear[PA_CHANNELS_MAX];

            calc_linear_float_stream_volumes(streams, nstreams, spec);
            calc_linear_float_volume(linear, volume);

            for (d = 0;; d += sizeof(float)) {
                float sum = 0;
                unsigned i;

                if (PA_UNLIKELY(d >= length))
                    goto finish;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    float v, cv = m->linear[channel].f;

                    if (PA_UNLIKELY(d >= m->chunk.length))
                        goto finish;

                    if (PA_UNLIKELY(cv <= 0) || PA_UNLIKELY(!!mute) || PA_UNLIKELY(linear[channel] <= 0))
                        v = 0;
                    else {
                        v = *((float*) m->ptr);
                        v *= cv;
                    }

                    sum += v;
                    m->ptr = (uint8_t*) m->ptr + sizeof(float);
                }

                sum *= linear[channel];
                *((float*) data) = sum;

                data = (uint8_t*) data + sizeof(float);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_FLOAT32RE: {
            unsigned channel = 0;
            float linear[PA_CHANNELS_MAX];

            calc_linear_float_stream_volumes(streams, nstreams, spec);
            calc_linear_float_volume(linear, volume);

            for (d = 0;; d += sizeof(float)) {
                float sum = 0;
                unsigned i;

                if (PA_UNLIKELY(d >= length))
                    goto finish;

                for (i = 0; i < nstreams; i++) {
                    pa_mix_info *m = streams + i;
                    float v, cv = m->linear[channel].f;

                    if (PA_UNLIKELY(d >= m->chunk.length))
                        goto finish;

                    if (PA_UNLIKELY(cv <= 0) || PA_UNLIKELY(!!mute) || PA_UNLIKELY(linear[channel] <= 0))
                        v = 0;
                    else
                        v = PA_FLOAT32_SWAP(*(float*) m->ptr) *cv;

                    sum += v;
                    m->ptr = (uint8_t*) m->ptr + sizeof(float);
                }

                sum *= linear[channel];
                *((float*) data) = PA_FLOAT32_SWAP(sum);

                data = (uint8_t*) data + sizeof(float);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        default:
            pa_log_error("ERROR: Unable to mix audio data of format %s.", pa_sample_format_to_string(spec->format));
            pa_assert_not_reached();
    }

finish:

    for (k = 0; k < nstreams; k++)
        pa_memblock_release(streams[k].chunk.memblock);

    return d;
}


void pa_volume_memchunk(
        pa_memchunk*c,
        const pa_sample_spec *spec,
        const pa_cvolume *volume) {

    void *ptr;

    pa_assert(c);
    pa_assert(spec);
    pa_assert(c->length % pa_frame_size(spec) == 0);
    pa_assert(volume);

    if (pa_memblock_is_silence(c->memblock))
        return;

    if (pa_cvolume_channels_equal_to(volume, PA_VOLUME_NORM))
        return;

    if (pa_cvolume_channels_equal_to(volume, PA_VOLUME_MUTED)) {
        pa_silence_memchunk(c, spec);
        return;
    }

    ptr = (uint8_t*) pa_memblock_acquire(c->memblock) + c->index;

    switch (spec->format) {

        case PA_SAMPLE_S16NE: {
            int16_t *d, *e;
            unsigned channel;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_volume(linear, volume);

            e = (int16_t*) ptr + c->length/sizeof(int16_t);

            for (channel = 0, d = ptr; d < e; d++) {
                int32_t t;

                t = (int32_t)(*d);
                t = (t * linear[channel]) / 0x10000;
                t = PA_CLAMP_UNLIKELY(t, -0x8000, 0x7FFF);
                *d = (int16_t) t;

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }
            break;
        }

        case PA_SAMPLE_S16RE: {
            int16_t *d, *e;
            unsigned channel;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_volume(linear, volume);

            e = (int16_t*) ptr + c->length/sizeof(int16_t);

            for (channel = 0, d = ptr; d < e; d++) {
                int32_t t;

                t = (int32_t) PA_INT16_SWAP(*d);
                t = (t * linear[channel]) / 0x10000;
                t = PA_CLAMP_UNLIKELY(t, -0x8000, 0x7FFF);
                *d = PA_INT16_SWAP((int16_t) t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_S32NE: {
            int32_t *d, *e;
            unsigned channel;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_volume(linear, volume);

            e = (int32_t*) ptr + c->length/sizeof(int32_t);

            for (channel = 0, d = ptr; d < e; d++) {
                int64_t t;

                t = (int64_t)(*d);
                t = (t * linear[channel]) / 0x10000;
                t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
                *d = (int32_t) t;

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }
            break;
        }

        case PA_SAMPLE_S32RE: {
            int32_t *d, *e;
            unsigned channel;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_volume(linear, volume);

            e = (int32_t*) ptr + c->length/sizeof(int32_t);

            for (channel = 0, d = ptr; d < e; d++) {
                int64_t t;

                t = (int64_t) PA_INT32_SWAP(*d);
                t = (t * linear[channel]) / 0x10000;
                t = PA_CLAMP_UNLIKELY(t, -0x80000000LL, 0x7FFFFFFFLL);
                *d = PA_INT32_SWAP((int32_t) t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }

        case PA_SAMPLE_U8: {
            uint8_t *d, *e;
            unsigned channel;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_volume(linear, volume);

            e = (uint8_t*) ptr + c->length;

            for (channel = 0, d = ptr; d < e; d++) {
                int32_t t;

                t = (int32_t) *d - 0x80;
                t = (t * linear[channel]) / 0x10000;
                t = PA_CLAMP_UNLIKELY(t, -0x80, 0x7F);
                *d = (uint8_t) (t + 0x80);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }
            break;
        }

        case PA_SAMPLE_ULAW: {
            uint8_t *d, *e;
            unsigned channel;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_volume(linear, volume);

            e = (uint8_t*) ptr + c->length;

            for (channel = 0, d = ptr; d < e; d++) {
                int32_t t;

                t = (int32_t) st_ulaw2linear16(*d);
                t = (t * linear[channel]) / 0x10000;
                t = PA_CLAMP_UNLIKELY(t, -0x8000, 0x7FFF);
                *d = (uint8_t) st_14linear2ulaw(t >> 2);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }
            break;
        }

        case PA_SAMPLE_ALAW: {
            uint8_t *d, *e;
            unsigned channel;
            int32_t linear[PA_CHANNELS_MAX];

            calc_linear_integer_volume(linear, volume);

            e = (uint8_t*) ptr + c->length;

            for (channel = 0, d = ptr; d < e; d++) {
                int32_t t;

                t = (int32_t) st_alaw2linear16(*d);
                t = (t * linear[channel]) / 0x10000;
                t = PA_CLAMP_UNLIKELY(t, -0x8000, 0x7FFF);
                *d = (uint8_t) st_13linear2alaw(t >> 3);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }
            break;
        }

        case PA_SAMPLE_FLOAT32NE: {
            float *d;
            int skip;
            unsigned n;
            unsigned channel;

            d = ptr;
            skip = spec->channels * sizeof(float);
            n = c->length/sizeof(float)/spec->channels;

            for (channel = 0; channel < spec->channels; channel ++) {
                float v, *t;

                if (PA_UNLIKELY(volume->values[channel] == PA_VOLUME_NORM))
                    continue;

                v = (float) pa_sw_volume_to_linear(volume->values[channel]);
                t = d + channel;
                oil_scalarmult_f32(t, skip, t, skip, &v, n);
            }
            break;
        }

        case PA_SAMPLE_FLOAT32RE: {
            float *d, *e;
            unsigned channel;
            float linear[PA_CHANNELS_MAX];

            calc_linear_float_volume(linear, volume);

            e = (float*) ptr + c->length/sizeof(float);

            for (channel = 0, d = ptr; d < e; d++) {
                float t;

                t = PA_FLOAT32_SWAP(*d);
                t *= linear[channel];
                *d = PA_FLOAT32_SWAP(t);

                if (PA_UNLIKELY(++channel >= spec->channels))
                    channel = 0;
            }

            break;
        }


        default:
            pa_log_warn(" Unable to change volume of format %s.", pa_sample_format_to_string(spec->format));
            /* If we cannot change the volume, we just don't do it */
    }

    pa_memblock_release(c->memblock);
}

size_t pa_frame_align(size_t l, const pa_sample_spec *ss) {
    size_t fs;

    pa_assert(ss);

    fs = pa_frame_size(ss);

    return (l/fs) * fs;
}

int pa_frame_aligned(size_t l, const pa_sample_spec *ss) {
    size_t fs;

    pa_assert(ss);

    fs = pa_frame_size(ss);

    return l % fs == 0;
}

void pa_interleave(const void *src[], unsigned channels, void *dst, size_t ss, unsigned n) {
    unsigned c;
    size_t fs;

    pa_assert(src);
    pa_assert(channels > 0);
    pa_assert(dst);
    pa_assert(ss > 0);
    pa_assert(n > 0);

    fs = ss * channels;

    for (c = 0; c < channels; c++) {
        unsigned j;
        void *d;
        const void *s;

        s = src[c];
        d = (uint8_t*) dst + c * ss;

        for (j = 0; j < n; j ++) {
            oil_memcpy(d, s, ss);
            s = (uint8_t*) s + ss;
            d = (uint8_t*) d + fs;
        }
    }
}

void pa_deinterleave(const void *src, void *dst[], unsigned channels, size_t ss, unsigned n) {
    size_t fs;
    unsigned c;

    pa_assert(src);
    pa_assert(dst);
    pa_assert(channels > 0);
    pa_assert(ss > 0);
    pa_assert(n > 0);

    fs = ss * channels;

    for (c = 0; c < channels; c++) {
        unsigned j;
        const void *s;
        void *d;

        s = (uint8_t*) src + c * ss;
        d = dst[c];

        for (j = 0; j < n; j ++) {
            oil_memcpy(d, s, ss);
            s = (uint8_t*) s + fs;
            d = (uint8_t*) d + ss;
        }
    }
}

static pa_memblock *silence_memblock_new(pa_mempool *pool, uint8_t c) {
    pa_memblock *b;
    size_t length;
    void *data;

    pa_assert(pool);

    length = PA_MIN(pa_mempool_block_size_max(pool), PA_SILENCE_MAX);

    b = pa_memblock_new(pool, length);

    data = pa_memblock_acquire(b);
    memset(data, c, length);
    pa_memblock_release(b);

    pa_memblock_set_is_silence(b, TRUE);

    return b;
}

void pa_silence_cache_init(pa_silence_cache *cache) {
    pa_assert(cache);

    memset(cache, 0, sizeof(pa_silence_cache));
}

void pa_silence_cache_done(pa_silence_cache *cache) {
    pa_sample_format_t f;
    pa_assert(cache);

    for (f = 0; f < PA_SAMPLE_MAX; f++)
        if (cache->blocks[f])
            pa_memblock_unref(cache->blocks[f]);

    memset(cache, 0, sizeof(pa_silence_cache));
}

pa_memchunk* pa_silence_memchunk_get(pa_silence_cache *cache, pa_mempool *pool, pa_memchunk* ret, const pa_sample_spec *spec, size_t length) {
    pa_memblock *b;
    size_t l;

    pa_assert(cache);
    pa_assert(pa_sample_spec_valid(spec));

    if (!(b = cache->blocks[spec->format]))

        switch (spec->format) {
            case PA_SAMPLE_U8:
                cache->blocks[PA_SAMPLE_U8] = b = silence_memblock_new(pool, 0x80);
                break;
            case PA_SAMPLE_S16LE:
            case PA_SAMPLE_S16BE:
            case PA_SAMPLE_S32LE:
            case PA_SAMPLE_S32BE:
            case PA_SAMPLE_FLOAT32LE:
            case PA_SAMPLE_FLOAT32BE:
                cache->blocks[PA_SAMPLE_S16LE] = b = silence_memblock_new(pool, 0);
                cache->blocks[PA_SAMPLE_S16BE] = pa_memblock_ref(b);
                cache->blocks[PA_SAMPLE_S32LE] = pa_memblock_ref(b);
                cache->blocks[PA_SAMPLE_S32BE] = pa_memblock_ref(b);
                cache->blocks[PA_SAMPLE_FLOAT32LE] = pa_memblock_ref(b);
                cache->blocks[PA_SAMPLE_FLOAT32BE] = pa_memblock_ref(b);
                break;
            case PA_SAMPLE_ALAW:
                cache->blocks[PA_SAMPLE_ALAW] = b = silence_memblock_new(pool, 0xd5);
                break;
            case PA_SAMPLE_ULAW:
                cache->blocks[PA_SAMPLE_ULAW] = b = silence_memblock_new(pool, 0xff);
                break;
            default:
                pa_assert_not_reached();
    }

    pa_assert(b);

    ret->memblock = pa_memblock_ref(b);

    l = pa_memblock_get_length(b);
    if (length > l || length == 0)
        length = l;

    ret->length = pa_frame_align(length, spec);
    ret->index = 0;

    return ret;
}

void pa_sample_clamp(pa_sample_format_t format, void *dst, size_t dstr, const void *src, size_t sstr, unsigned n) {
    const float *s;
    float *d;

    s = src; d = dst;

    if (format == PA_SAMPLE_FLOAT32NE) {

        float minus_one = -1.0, plus_one = 1.0;
        oil_clip_f32(d, dstr, s, sstr, n, &minus_one, &plus_one);

    } else {
        pa_assert(format == PA_SAMPLE_FLOAT32RE);

        for (; n > 0; n--) {
            float f;

            f = PA_FLOAT32_SWAP(*s);
            f = PA_CLAMP_UNLIKELY(f, -1.0, 1.0);
            *d = PA_FLOAT32_SWAP(f);

            s = (const float*) ((const uint8_t*) s + sstr);
            d = (float*) ((uint8_t*) d + dstr);
        }
    }
}
