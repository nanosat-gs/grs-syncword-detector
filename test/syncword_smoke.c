/*
 * syncword_smoke.c
 *
 * Copyright The GRS Syncword Detector Contributors.
 *
 * This file is part of GRS Syncword Detector.
 *
 * GRS Syncword Detector is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GRS Syncword Detector is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GRS Syncword Detector. If not, see <http://www.gnu.org/licenses/>.
 *
 */

/**
 * \brief Smoke test for the detector.
 *
 * Builds the FloripaSat sync word (5D E6 2A 7E, as ngham.c defines it), plants
 * it in a bit stream at an offset that is NOT a multiple of eight, and
 * requires the detector to find it.
 *
 * The misaligned offset is the whole point. What arrives from the
 * demodulator is a bit stream with no byte synchronisation whatsoever -- byte
 * boundaries only start to mean something once the sync word has been found.
 * A detector that only looks at byte boundaries finds the sync word in one
 * pass out of eight, and the other seven look exactly like a satellite that
 * did not transmit.
 *
 * Runs with no ZMQ and no radio, which is what makes it usable as a build
 * gate: `make check`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "syncword.h"

#define SMOKE_STREAM_BITS   512
#define SMOKE_PLANT_OFFSET  100     /* Deliberately not a multiple of 8. */

static int expect(const char *what, int got, int wanted)
{
    if (got != wanted)
    {
        fprintf(stderr, "FAIL: %s returned %d, expected %d\n", what, got, wanted);

        return 1;
    }

    printf("ok: %s -> %d\n", what, got);

    return 0;
}

int main(void)
{
    uint8_t syncword_bytes[4] = {0x5D, 0xE6, 0x2A, 0x7E};  /* NGH_SYNC do ngham.c */
    bool stream[SMOKE_STREAM_BITS];
    SyncWord *sw = NULL;
    size_t i;
    int failures = 0;

    sw = syncword_create(syncword_bytes, sizeof(syncword_bytes), false);

    if (sw == NULL)
    {
        fprintf(stderr, "FAIL: syncword_create returned NULL\n");

        return EXIT_FAILURE;
    }

    /* A stream of zeros with the sync word planted at a misaligned offset. */
    memset(stream, 0, sizeof(stream));

    for (i = 0U; i < sw->bit_len; i++)
    {
        stream[SMOKE_PLANT_OFFSET + i] = sw->bits[i];
    }

    failures += expect("exact match at a misaligned offset",
                       syncword_detect(stream, sw, 0, SMOKE_STREAM_BITS),
                       SMOKE_PLANT_OFFSET + (int)sw->bit_len);

    /* One flipped bit must still match when one error is tolerated: a real
     * pass is never clean, and a detector that only accepts perfect sync
     * words drops frames that the error correction could have recovered. */
    stream[SMOKE_PLANT_OFFSET + 5U] = !stream[SMOKE_PLANT_OFFSET + 5U];

    failures += expect("one bit error, tolerated",
                       syncword_detect(stream, sw, 1, SMOKE_STREAM_BITS),
                       SMOKE_PLANT_OFFSET + (int)sw->bit_len);

    failures += expect("one bit error, not tolerated",
                       syncword_detect(stream, sw, 0, SMOKE_STREAM_BITS),
                       -1);

    /* Nothing planted at all must report nothing found -- a detector that
     * finds a sync word in silence is worse than useless downstream. */
    memset(stream, 0, sizeof(stream));

    failures += expect("empty stream, nothing found",
                       syncword_detect(stream, sw, 0, SMOKE_STREAM_BITS),
                       -1);

    syncword_destroy(sw);

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);

        return EXIT_FAILURE;
    }

    printf("all checks passed\n");

    return EXIT_SUCCESS;
}
