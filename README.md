<h1 align="center">
    GRS Syncword Detector
    <br>
</h1>

<h4 align="center">Syncword Detector of the SpaceLab's Ground Station.</h4>

<p align="center">
    <a href="https://github.com/spacelab-ufsc/grs-syncword-detector">
        <img src="https://img.shields.io/badge/status-development-green?style=for-the-badge">
    </a>
    <a href="https://github.com/spacelab-ufsc/grs-syncword-detector/releases">
        <img alt="GitHub commits since latest release (by date)" src="https://img.shields.io/github/commits-since/spacelab-ufsc/grs-syncword-detector/latest?style=for-the-badge">
    </a>
    <a href="https://github.com/spacelab-ufsc/grs-syncword-detector/blob/main/LICENSE">
        <img src="https://img.shields.io/badge/license-GPL3-yellow?style=for-the-badge">
    </a>
</p>

<p align="center">
    <a href="#overview">Overview</a> •
    <a href="#dependencies">Dependencies</a> •
    <a href="#building">Building</a> •
    <a href="#running-as-a-service-station-branch">Running</a> •
    <a href="#documentation">Documentation</a> •
    <a href="#license">License</a>
</p>

## Overview

GRS Syncword Detector is a C library component of SpaceLab's Ground Station signal processing pipeline. It sits between the demodulator and the decoder, and is responsible for identifying the start of a transmission by detecting a known synchronization word (syncword) within an incoming bit stream.

The component supports both MSB-first and LSB-first bit endianness, and performs tolerance-based matching by counting the number of bits that match the expected syncword pattern, allowing it to handle minor bit errors in the received stream.

## Dependencies

The library has no external dependencies beyond the C standard library. The
ZMQ service (`station` branch) needs libzmq:

* Ubuntu: `sudo apt install build-essential libzmq3-dev`
* Fedora: `sudo dnf install gcc make zeromq-devel`

## Building

One source tree, two products:

| Target | What |
|---|---|
| `libsyncword.a` | The detector itself (`syncword.c`), for whoever wants to link it. Never pulls in libzmq |
| `grs_syncword` | The ZMQ service (`service.c`) that the ground station runs |

```
make            # library, service and the smoke test
make check      # builds the library and proves it detects (no ZMQ, no radio)
make install    # installs grs_syncword in /usr/local/bin
make clean
```

## Running as a service (`station` branch)

This is the `nanosat-gs` fork. The `station` branch (based on upstream commit
`01e3d04`, whose search is bit by bit — required, since the bit stream from the
demodulator has no byte alignment) wraps the detector in a ZMQ service used by
the SpaceLab ground station ([nanosat-gs/grs-station](https://github.com/nanosat-gs/grs-station)):

```
./grs_syncword
```

### ZMQ interface

**Input (SUB, default `tcp://localhost:5555`)** — the demodulator's bits: one
message per window, no topic frame, **one byte per bit** (`0x00`/`0x01`). That
is exactly the `bool*` that `syncword_detect` searches, used as-is.

**Output (PUB, default `tcp://*:5558`)** — one message per detected packet,
three frames:

| Frame | Content |
|---|---|
| 0 | Topic `raw_packet` |
| 1 | JSON header, one line: `seq`, `bit_offset`, `bits`, `bytes`, `max_sync_errors`, `syncword`, `bit_order`, `detected_at` |
| 2 | Payload: a **fixed** slice of 255 bytes after the syncword, packed MSB-first |

The slice is fixed because this stage does not know where the frame ends: the
length lives inside NGHam, and parsing NGHam is the decoder's job. 255 bytes
covers the largest NGHam frame.

### Configuration (environment variables)

| Variable | Default | Description |
|---|---|---|
| `GRS_SYNCWORD_BITS_SOURCE` | `tcp://localhost:5555` | Where to subscribe to bits |
| `GRS_SYNCWORD_PACKETS_BIND` | `tcp://*:5558` | Where to publish raw packets |
| `GRS_SYNCWORD_BYTES` | `5DE62A7E` | Syncword, in hex |
| `GRS_SYNCWORD_BIT_ORDER` | `msb` | `msb` or `lsb` expansion of the syncword bytes |
| `GRS_SYNCWORD_PACKET_BYTES` | `255` | Size of the slice published after each syncword |
| `GRS_SYNCWORD_MAX_ERRORS` | `1` | Bit errors tolerated in the syncword |

> **The NGHam syncword is `5D E6 2A 7E`, searched MSB-first.** `BA 67 54 7E`
> (which circulated in earlier documents) is the same vector with the bits of
> each byte reversed: searched MSB-first, it finds **zero** packets in a real
> FloripaSat-1 recording, while `5D E6 2A 7E` finds nineteen.

### Changes in the `station` branch

- `syncword.c` compiles against its own header (upstream `main` declares
  different `SyncWord` types in the two files).
- The ZMQ service and the raw packet envelope (`service.c`).
- Correct syncword value and bit order (see the note above).
- A syncword falling **inside** the 255-byte slice of the previous packet is no
  longer skipped. With 1200 baud bursts, that was half of the beacon packets.

## Documentation

The documentation of the upstream project is generated using the Sphinx tool, and it is available [here](https://spacelab-ufsc.github.io/grs-syncword-detector/). It does not cover the changes in the `station` branch, and the Sphinx sources are not in this repository.

### Dependencies

* Sphinx
* sphinx-rtd-theme

### Building the Documentation

```
make html
```

## License

This project is licensed under GPLv3 license.
