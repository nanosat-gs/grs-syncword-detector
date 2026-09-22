#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "syncword.h"

int hamming_distance(bool* syncword, bool* window_bits, size_t len) {
    int dist = 0;
    for (int i = 0; i < len; i++) {
        dist += (syncword[i] != window_bits[i]);
    }
    return dist;
}

void byte_to_bits(uint8_t val, bool out[8], bool lsb_first) {
    for(int i = 0; i < 8; i++) {
        int bit = (val >> (lsb_first ? i : (7 - i))) & 1;
        out[i] = bit;
    }
}

SyncWord* syncword_create(uint8_t* bytes, size_t byte_count, bool lsb_first) {
    SyncWord* sw = malloc(sizeof(SyncWord));
    if (!sw) return NULL;

    sw->bit_len = byte_count * 8;
    sw->bits = malloc(sw->bit_len * sizeof(bool));
    if (!sw->bits) {
        return NULL;
    }

    for (size_t b = 0; b < byte_count; b++) {
        bool buf[8];
        byte_to_bits(bytes[b], buf, lsb_first);

        memcpy(sw->bits + (b * 8), buf, 8 * sizeof(bool));
    }

    return sw;
}

int syncword_detect(bool* stream_bits, SyncWord* syncword, int max_errors, int size_stream_bits) {
    if (syncword->bit_len > size_stream_bits) {
        return -1;
    }

    int last_index = size_stream_bits - syncword->bit_len;

    for (int i = 0; i <= last_index; i++) {
        int dist = hamming_distance(syncword->bits, stream_bits + i, syncword->bit_len);
        if (dist <= max_errors) {
            return (int)(i + syncword->bit_len);
        }
    }
    return -1;
}

void syncword_destroy(SyncWord* sw) {
    if (!sw) return;
    free(sw->bits);
    free(sw);
}
