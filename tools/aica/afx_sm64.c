/* Build AICAflow assets from a job written by tools/aica/sm64_afx.py.
 *
 *   clang -std=c11 -O2 -Wall -Wextra -Werror -I$AICAFORGE/dependencies/AICAflow/driver/format/include \
 *       -I$AICAFORGE/src tools/aica/afx_sm64.c $AICAFORGE/src/afx_compile_c.c \
 *       $AICAFORGE/dependencies/AICAflow/driver/format/src/codec.c -lm -o build/afx_sm64
 *   build/afx_sm64 song.job song.afb song.afx     (writes song.afc too)
 *
 * The job: "SJOB", zone count, event count, duration ticks, loop tick (~0 for
 * none), tick rate numerator, channels, tick rate denominator (0 = 1); then each zone (format, loop, loop start, loop
 * end, frames, bytes, data padded to 4) and each event (tick, order, opcode,
 * channel, setup, mask, 18 field words). Zones are complete AICA samples: the
 * NOTE events carry the pitch, pan and level words, so every zone is one
 * setup template selected by index. */
#include <aicaflow/codec.h>
#include "afx_compile_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, uint32_t *bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(size > 0 ? (size_t)size : 1);
    if (!data || (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size)) { fclose(f); free(data); return NULL; }
    fclose(f);
    *bytes = (uint32_t)size;
    return data;
}
static int write_file(const char *path, const uint8_t *data, uint32_t bytes) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int bad = fwrite(data, 1, bytes, f) != bytes;
    return fclose(f) || bad ? -1 : 0;
}
static int sibling(const char *path, const char *suffix, char *out, size_t cap) {
    const char *dot = strrchr(path, '.');
    size_t stem = dot ? (size_t)(dot - path) : strlen(path);
    if (stem + strlen(suffix) + 1 > cap) return -1;
    memcpy(out, path, stem); strcpy(out + stem, suffix);
    return 0;
}

/* Insert a lane map (every channel in lane 0) between the setups and the stream and set
 * AFX_FLAG_LANES, so the host can pan the whole flow with afx_instance_lanes_set(AFX_LANE_PAN).
 * The stream moves by the map's size: LOOP targets are stream-relative and setups keep their
 * place, so only the header changes. AFC checkpoints hold image positions: dropped here. */
static int add_lanes(afx_c_output_t *out) {
    afx_file_header_t h;
    if (afx_file_validate(out->afx, out->afx_bytes, &h)) return -1;
    uint32_t lanes = h.required_channels, setups = h.setup_count * AFX_SETUP_BYTES;
    uint32_t bytes = out->afx_bytes + lanes;
    uint8_t *afx = calloc(1, bytes);
    if (!afx) return -1;
    memcpy(afx, out->afx, h.image_offset + setups);
    memcpy(afx + h.image_offset + setups + lanes, out->afx + h.image_offset + h.stream_offset, h.stream_size);
    h.flags |= AFX_FLAG_LANES;
    h.stream_offset = setups + lanes;
    h.image_size += lanes;
    h.total_size = bytes;
    h.control_id = afx_control_id(afx + h.image_offset, h.image_size);
    afx_encode_header(afx, &h);
    free(out->afx); out->afx = afx; out->afx_bytes = bytes;
    free(out->afc); out->afc = NULL; out->afc_bytes = 0;
    return afx_c_set_work_profile(out->afx, out->afx_bytes);
}

int main(int argc, char **argv) {
    int lanes = argc == 5 && !strcmp(argv[4], "--lanes");
    if (lanes) argc--;
    if (argc != 4) return fprintf(stderr, "usage: %s song.job out.afb out.afx [--lanes]\n", argv[0]), 2;
    uint32_t bytes;
    uint8_t *job = read_file(argv[1], &bytes);
    if (!job || bytes < 32 || afx_read32(job) != 0x424F4A53u) return fprintf(stderr, "bad job %s\n", argv[1]), 1;
    uint32_t zone_count = afx_read32(job + 4), event_count = afx_read32(job + 8), duration = afx_read32(job + 12);
    uint32_t loop_tick = afx_read32(job + 16), rate_num = afx_read32(job + 20), channels = afx_read32(job + 24);
    uint32_t rate_den = afx_read32(job + 28) ? afx_read32(job + 28) : 1u;
    /* The compiler takes whole ticks per second; the exact rational rate goes in the header after. */
    uint32_t rate = (rate_num + rate_den / 2u) / rate_den;
    afx_c_zone_t *zones = calloc(zone_count, sizeof(*zones));
    afx_c_event_t *events = calloc(event_count, sizeof(*events));
    if (!zones || !events) return 1;
    uint32_t at = 32;
    for (uint32_t i = 0; i < zone_count; ++i) {
        if (at + 24 > bytes) return fprintf(stderr, "truncated job\n"), 1;
        uint32_t fmt = afx_read32(job + at), loop = afx_read32(job + at + 4), loop_start = afx_read32(job + at + 8);
        uint32_t loop_end = afx_read32(job + at + 12), frames = afx_read32(job + at + 16), size = afx_read32(job + at + 20);
        at += 24;
        if (at + size > bytes) return fprintf(stderr, "truncated job\n"), 1;
        zones[i] = (afx_c_zone_t){
            .sample = { job + at, size, frames, (uint8_t)fmt, 60, (uint8_t)loop,
                        (uint16_t)loop_start, (uint16_t)loop_end, 0, 44100 },
            .key_max = 127, .velocity_max = 127 };
        at += (size + 3u) & ~3u;
    }
    for (uint32_t i = 0; i < event_count; ++i) {
        if (at + 52 > bytes) return fprintf(stderr, "truncated job\n"), 1;
        afx_c_event_t *e = &events[i];
        e->tick = afx_read32(job + at); e->order = afx_read32(job + at + 4);
        e->opcode = job[at + 8]; e->channel = job[at + 9];
        e->setup = afx_read16(job + at + 10); e->mask = afx_read32(job + at + 12);
        for (uint32_t f = 0; f < 18; ++f) e->fields[f] = afx_read16(job + at + 16 + 2 * f);
        at += 52;
    }
    (void)channels;
    afx_c_output_t out;
    int result = loop_tick == UINT32_MAX
        ? afx_c_compile_events(events, event_count, duration, rate, zones, zone_count, &out)
        : afx_c_compile_events_loop(events, event_count, duration, loop_tick, rate, zones, zone_count, &out);
    if (result) return fprintf(stderr, "cannot compile %s (%u zones, %u events, %u ticks)\n", argv[1], zone_count, event_count, duration), 1;
    /* Header words 68/72: the authored tick rate. Not part of the control image, so the
     * control identity (and with it the AFC binding) is unchanged. */
    afx_write32(out.afx + 68, rate_num);
    afx_write32(out.afx + 72, rate_den);
    if (afx_file_validate(out.afx, out.afx_bytes, NULL)) return fprintf(stderr, "invalid AFX after the rate patch\n"), 1;
    if (lanes && add_lanes(&out)) return fprintf(stderr, "cannot add the lane map\n"), 1;
    char afc[4096];
    if (write_file(argv[2], out.afb, out.afb_bytes) || write_file(argv[3], out.afx, out.afx_bytes) ||
        (out.afc && (sibling(argv[3], ".afc", afc, sizeof(afc)) || write_file(afc, out.afc, out.afc_bytes))))
        return fprintf(stderr, "cannot write outputs\n"), 1;
    printf("wrote %s (%u bytes) and %s (%u bytes, %u stream bytes%s)\n", argv[2], out.afb_bytes, argv[3], out.afx_bytes,
           afx_read32(out.afx + 28), loop_tick == UINT32_MAX ? "" : ", looping");
    afx_c_output_free(&out);
    free(zones); free(events); free(job);
    return 0;
}
