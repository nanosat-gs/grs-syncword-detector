/*
 * service.c
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
 * \brief ZMQ service around the syncword detector.
 *
 * The detector is a library: syncword_create, syncword_detect,
 * syncword_destroy, and nothing else. This turns it into the stage that sits
 * between the demodulator and the decoder.
 *
 * ## Input: the bit envelope
 *
 * SUB on the demodulator's PUB (:5555 by default). One message per window,
 * no topic frame, ONE BYTE PER BIT -- each byte 0x00 or 0x01. That is
 * exactly a `bool*`, which is what syncword_detect searches over, so the
 * payload is used as-is with no unpacking.
 *
 * ## Output: the raw packet envelope
 *
 * PUB (:5558 by default), three frames:
 *
 *     [0] topic   "raw_packet"       so a subscriber can filter
 *     [1] header  JSON, one line     provenance and offsets
 *     [2] payload packed bytes       MSB-first, the bits after the syncword
 *
 * This envelope is the seam with the decode half of the station, and it is
 * defined here because this is the first stage that has anything resembling
 * a packet to hand over.
 *
 * Why the payload is a FIXED number of bytes: this stage does not know where
 * the frame ends. Length lives inside NGHam, and parsing NGHam is the
 * decoder's job, not the detector's. So a generous fixed slice after the
 * syncword is published and the decoder takes what it needs. 255 bytes
 * covers the largest NGHam frame.
 *
 * Why packed here and not one-byte-per-bit like the input: byte framing
 * starts at the syncword. Before it there is no byte boundary to speak of --
 * which is the whole reason the search has to be bit-by-bit -- but after it
 * there is, and the decoder wants bytes. MSB-first, matching the order the
 * sync word BA 67 54 7E is written in.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include <zmq.h>

#include "syncword.h"

/* NGHam sync word, straight from the reference implementation:
 *
 *     const uint8_t NGH_SYNC[] = {0x5D, 0xE6, 0x2A, 0x7E};   (ngham.c)
 *
 * NOT "BA67547E", which is what the slice document carried. That value is the
 * SAME vector with the bits of each byte reversed -- true only if you expand
 * it LSB-first. Searched MSB-first, as this service does, it matches nothing.
 *
 * Measured, not reasoned: against a real FloripaSat-1 recording, BA67547E
 * MSB-first found ZERO packets and 5DE62A7E found nineteen, each followed by
 * an NGHam size tag at Hamming distance 0 and preceded by the 0xAA preamble
 * the reference implementation defines.
 *
 * The bug survived because our own simulator generated BA67547E MSB-first
 * too: transmitter and receiver agreed with each other and both disagreed
 * with the satellite. Only off-air data could catch it. */
#define SYNCWORD_DEFAULT_BYTES          "5DE62A7E"

#define SERVICE_DEFAULT_BITS_SOURCE     "tcp://localhost:5555"
#define SERVICE_DEFAULT_PACKETS_BIND    "tcp://*:5558"
#define SERVICE_DEFAULT_PACKET_BYTES    255
#define SERVICE_DEFAULT_MAX_ERRORS      1
#define SERVICE_TOPIC                   "raw_packet"

/* Room for the packet itself plus one sync word, so a match landing near the
 * end of the buffer can still be completed by the next message. */
#define SERVICE_BUFFER_SLACK_BITS       4096

static volatile sig_atomic_t do_exit = 0;

static void sig_handler(int signum)
{
    (void)signum;

    do_exit = 1;
}

/**
 * \brief Reads an environment variable, falling back to a default.
 */
static const char *env_or(const char *name, const char *fallback)
{
    const char *value = getenv(name);

    return ((value != NULL) && (value[0] != '\0')) ? value : fallback;
}

/**
 * \brief Reads an integer from the environment, with a floor.
 *
 * Returns -1 on a value that is present but not an integer at or above
 * `floor`, so the caller can refuse to boot instead of silently using the
 * default. A typo in the .env must not become a whole pass received with the
 * wrong packet size.
 *
 * The floor is a parameter because the two settings differ: a packet of zero
 * bytes is meaningless, but tolerating zero bit errors is a perfectly good
 * ask -- it means the sync word has to match exactly. An earlier version
 * read max_errors with an offset of one and shifted it back, which also
 * shifted the value the operator actually asked for: GRS_SYNCWORD_MAX_ERRORS=1
 * silently became zero tolerance.
 */
static long env_int(const char *name, long fallback, long floor)
{
    const char *value = getenv(name);
    char *end = NULL;
    long parsed;

    if ((value == NULL) || (value[0] == '\0'))
    {
        return fallback;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);

    if ((errno != 0) || (end == value) || (*end != '\0') || (parsed < floor))
    {
        return -1;
    }

    return parsed;
}

/**
 * \brief Parses a hex string such as "BA67547E" into bytes.
 *
 * \return Number of bytes written, or -1 if the string is not valid hex of
 *         even length.
 */
static int parse_hex(const char *text, uint8_t *out, size_t out_len)
{
    size_t len = strlen(text);
    size_t i;

    if (((len % 2U) != 0U) || ((len / 2U) > out_len) || (len == 0U))
    {
        return -1;
    }

    for (i = 0U; i < (len / 2U); i++)
    {
        char pair[3] = {text[2U * i], text[(2U * i) + 1U], '\0'};
        char *end = NULL;
        long value;

        errno = 0;
        value = strtol(pair, &end, 16);

        if ((errno != 0) || (*end != '\0') || (value < 0) || (value > 0xFF))
        {
            return -1;
        }

        out[i] = (uint8_t)value;
    }

    return (int)(len / 2U);
}

/**
 * \brief Packs a bit array into bytes, MSB first.
 */
static void pack_bits(const bool *bits, size_t bit_count, uint8_t *out)
{
    size_t i;

    memset(out, 0, bit_count / 8U);

    for (i = 0U; i < bit_count; i++)
    {
        if (bits[i])
        {
            out[i / 8U] |= (uint8_t)(1U << (7U - (i % 8U)));
        }
    }
}

/**
 * \brief Current UTC time as ISO-8601 with a Z suffix.
 */
static void iso_now(char *out, size_t out_len)
{
    time_t now = time(NULL);
    struct tm utc;

    if (gmtime_r(&now, &utc) == NULL)
    {
        snprintf(out, out_len, "1970-01-01T00:00:00Z");

        return;
    }

    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

/**
 * \brief Publishes one raw packet: topic, JSON header, packed payload.
 */
static int publish_packet(void *publisher, const char *syncword_hex, uint64_t sequence,
                          uint64_t bit_offset, int max_errors, const bool *bits, size_t bit_count)
{
    char header[512];
    char timestamp[32];
    uint8_t *payload = malloc(bit_count / 8U);
    int header_len;
    int result = 0;

    if (payload == NULL)
    {
        return -1;
    }

    pack_bits(bits, bit_count, payload);
    iso_now(timestamp, sizeof(timestamp));

    /* `bit_offset` is the absolute position, in the bit stream since boot, of
     * the first bit AFTER the sync word. It is what makes a packet traceable
     * back to a point in a recorded capture -- without it, a packet that
     * decodes badly cannot be located in the IQ that produced it.
     *
     * `max_sync_errors` is the TOLERANCE that was in force, not the distance
     * actually measured: syncword_detect returns only an index, so the real
     * Hamming distance of the match is not available here. The field is named
     * for what it holds -- calling it `sync_errors` would have every consumer
     * reading it as a per-packet quality figure, which it is not. */
    header_len = snprintf(header, sizeof(header),
                          "{\"seq\":%llu,\"bit_offset\":%llu,\"bits\":%zu,\"bytes\":%zu,"
                          "\"max_sync_errors\":%d,\"syncword\":\"%s\",\"bit_order\":\"msb_first\","
                          "\"detected_at\":\"%s\"}",
                          (unsigned long long)sequence, (unsigned long long)bit_offset,
                          bit_count, bit_count / 8U, max_errors, syncword_hex, timestamp);

    if ((header_len < 0) || ((size_t)header_len >= sizeof(header)))
    {
        free(payload);

        return -1;
    }

    if (zmq_send(publisher, SERVICE_TOPIC, strlen(SERVICE_TOPIC), ZMQ_SNDMORE) < 0) { result = -1; }
    else if (zmq_send(publisher, header, (size_t)header_len, ZMQ_SNDMORE) < 0) { result = -1; }
    else if (zmq_send(publisher, payload, bit_count / 8U, 0) < 0) { result = -1; }

    free(payload);

    return result;
}

int main(void)
{
    struct sigaction sigact;

    const char *bits_source = env_or("GRS_SYNCWORD_BITS_SOURCE", SERVICE_DEFAULT_BITS_SOURCE);
    const char *packets_bind = env_or("GRS_SYNCWORD_PACKETS_BIND", SERVICE_DEFAULT_PACKETS_BIND);
    const char *syncword_hex = env_or("GRS_SYNCWORD_BYTES", SYNCWORD_DEFAULT_BYTES);

    long packet_bytes = env_int("GRS_SYNCWORD_PACKET_BYTES", SERVICE_DEFAULT_PACKET_BYTES, 1);
    long max_errors = env_int("GRS_SYNCWORD_MAX_ERRORS", SERVICE_DEFAULT_MAX_ERRORS, 0);

    uint8_t syncword_bytes[32];
    int syncword_len;
    const char *bit_order = env_or("GRS_SYNCWORD_BIT_ORDER", "msb");
    bool lsb_first = (strcmp(bit_order, "lsb") == 0);

    void *context = NULL;
    void *subscriber = NULL;
    void *publisher = NULL;

    SyncWord *syncword = NULL;

    bool *buffer = NULL;
    size_t capacity;
    size_t count = 0U;

    size_t packet_bits;
    uint64_t sequence = 0U;
    uint64_t consumed_bits = 0U;

    if (packet_bytes < 0)
    {
        fprintf(stderr, "GRS_SYNCWORD_PACKET_BYTES must be a positive integer\n");

        return EXIT_FAILURE;
    }

    if (max_errors < 0)
    {
        fprintf(stderr, "GRS_SYNCWORD_MAX_ERRORS must be a non-negative integer\n");

        return EXIT_FAILURE;
    }

    syncword_len = parse_hex(syncword_hex, syncword_bytes, sizeof(syncword_bytes));

    if (syncword_len < 0)
    {
        fprintf(stderr, "GRS_SYNCWORD_BYTES must be hex of even length, got '%s'\n", syncword_hex);

        return EXIT_FAILURE;
    }

    packet_bits = (size_t)packet_bytes * 8U;
    capacity = packet_bits + ((size_t)syncword_len * 8U) + SERVICE_BUFFER_SLACK_BITS;

    buffer = malloc(capacity * sizeof(bool));

    if (buffer == NULL)
    {
        fprintf(stderr, "Failed to allocate the bit buffer\n");

        return EXIT_FAILURE;
    }

    /* Bit order, configurable because it is the setting most likely to be
     * wrong and least likely to be suspected: a wrong order does not throw,
     * does not warn and does not degrade -- it simply finds nothing, which
     * looks exactly like a satellite that did not transmit. */
    syncword = syncword_create(syncword_bytes, (size_t)syncword_len, lsb_first);

    if (syncword == NULL)
    {
        fprintf(stderr, "Failed to build the sync word\n");
        free(buffer);

        return EXIT_FAILURE;
    }

    sigact.sa_handler = sig_handler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);

    context = zmq_ctx_new();
    subscriber = zmq_socket(context, ZMQ_SUB);
    publisher = zmq_socket(context, ZMQ_PUB);

    if ((context == NULL) || (subscriber == NULL) || (publisher == NULL))
    {
        fprintf(stderr, "Failed to create the ZMQ sockets: %s\n", zmq_strerror(errno));
        goto cleanup;
    }

    if (zmq_connect(subscriber, bits_source) != 0)
    {
        fprintf(stderr, "Failed to connect to %s: %s\n", bits_source, zmq_strerror(errno));
        goto cleanup;
    }

    /* Subscribe to everything: the bit envelope carries no topic frame. */
    zmq_setsockopt(subscriber, ZMQ_SUBSCRIBE, "", 0);

    if (zmq_bind(publisher, packets_bind) != 0)
    {
        fprintf(stderr, "Failed to bind %s: %s\n", packets_bind, zmq_strerror(errno));
        goto cleanup;
    }

    /* The receive timeout is what lets SIGTERM be noticed: without it the
     * process sits in zmq_recv forever and `docker stop` has to kill it. */
    {
        int timeout_ms = 500;
        zmq_setsockopt(subscriber, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
    }

    printf("grs-syncword-detector: bits from %s\n", bits_source);
    printf("grs-syncword-detector: raw packets on %s\n", packets_bind);
    printf("grs-syncword-detector: syncword %s (%s-first), up to %ld bit errors, "
           "%ld byte packets\n", syncword_hex, lsb_first ? "lsb" : "msb",
           max_errors, packet_bytes);
    fflush(stdout);

    while (!do_exit)
    {
        zmq_msg_t message;
        size_t incoming;
        const uint8_t *payload;
        size_t i;

        zmq_msg_init(&message);

        if (zmq_msg_recv(&message, subscriber, 0) < 0)
        {
            zmq_msg_close(&message);

            if (errno == EAGAIN)
            {
                continue;   /* Timeout: loop back and re-check do_exit. */
            }

            if (errno == EINTR)
            {
                continue;
            }

            fprintf(stderr, "Receive failed: %s\n", zmq_strerror(errno));

            break;
        }

        incoming = zmq_msg_size(&message);
        payload = (const uint8_t *)zmq_msg_data(&message);

        for (i = 0U; i < incoming; i++)
        {
            if (count >= capacity)
            {
                /* Buffer full without a match: drop the oldest half. Losing
                 * old bits is the right trade -- a sync word that has not
                 * appeared in a full buffer is not going to appear in those
                 * bits later, and growing without bound would turn a noisy
                 * pass into an out-of-memory kill. */
                size_t keep = capacity / 2U;

                memmove(buffer, buffer + (count - keep), keep * sizeof(bool));
                consumed_bits += (uint64_t)(count - keep);
                count = keep;
            }

            buffer[count] = (payload[i] != 0U);
            count++;
        }

        zmq_msg_close(&message);

        /* Drain every complete packet the buffer now holds. */
        while (!do_exit)
        {
            int after_sync = syncword_detect(buffer, syncword, (int)max_errors, (int)count);
            size_t start;

            if (after_sync < 0)
            {
                /* No match. Keep the last (syncword - 1) bits so a sync word
                 * straddling two messages is still found. */
                size_t keep = (size_t)(syncword_len * 8) - 1U;

                if (count > keep)
                {
                    memmove(buffer, buffer + (count - keep), keep * sizeof(bool));
                    consumed_bits += (uint64_t)(count - keep);
                    count = keep;
                }

                break;
            }

            start = (size_t)after_sync;

            if ((count - start) < packet_bits)
            {
                /* Match found, packet not complete yet. Rewind to just before
                 * the sync word so the next pass finds it again. */
                size_t rewind = start - (size_t)(syncword_len * 8);

                if (rewind > 0U)
                {
                    memmove(buffer, buffer + rewind, (count - rewind) * sizeof(bool));
                    consumed_bits += (uint64_t)rewind;
                    count -= rewind;
                }

                break;
            }

            if (publish_packet(publisher, syncword_hex, sequence,
                               consumed_bits + (uint64_t)start, (int)max_errors,
                               buffer + start, packet_bits) != 0)
            {
                fprintf(stderr, "Failed to publish packet %llu\n", (unsigned long long)sequence);
            }
            else
            {
                sequence++;

                if ((sequence % 10U) == 1U)
                {
                    printf("grs-syncword-detector: %llu raw packets\n",
                           (unsigned long long)sequence);
                    fflush(stdout);
                }
            }

            {
                size_t advance = start + packet_bits;

                memmove(buffer, buffer + advance, (count - advance) * sizeof(bool));
                consumed_bits += (uint64_t)advance;
                count -= advance;
            }
        }
    }

    printf("grs-syncword-detector: stopping after %llu raw packets\n",
           (unsigned long long)sequence);

cleanup:

    if (subscriber != NULL) { zmq_close(subscriber); }
    if (publisher != NULL) { zmq_close(publisher); }
    if (context != NULL) { zmq_ctx_destroy(context); }

    syncword_destroy(syncword);
    free(buffer);

    return EXIT_SUCCESS;
}
