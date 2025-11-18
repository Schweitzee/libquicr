# CMAF Transcode Client for Media-over-QUIC

## Overview

This module provides a C++ transcode client for processing fragmented CMAF (Common Media Application Format) streams in Media-over-QUIC applications. It accepts fragmented MP4 input streams and produces rescaled fragmented CMAF output while preserving timestamp information.

## Features

- **Fragmented CMAF Support**: Handles CMAF initialization segments and media fragments (moof+mdat)
- **Video Scaling**: Rescales video to target resolution using high-quality algorithms
- **Timestamp Preservation**: Maintains PTS/DTS relationships for correct playback timing
- **Configurable Encoding**: Adjustable bitrate, framerate, and encoder presets
- **Stream-based Processing**: Processes fragments as they arrive (low latency)
- **Callback-based Output**: Delivers transcoded init segments and fragments via callbacks

## Components

### 1. TranscodeClient Class (`transcode_client.h`, `transcode_client.cpp`)

The core transcoding engine that handles:
- CMAF fragment parsing
- Video decoding (H.264, HEVC, etc.)
- Frame scaling
- Video re-encoding
- CMAF fragment generation

**Key Methods:**
```cpp
// Create transcode client
auto client = TranscodeClient::Create(config);

// Set output callbacks
client->SetOutputInitCallback([](const uint8_t* data, size_t size) {
    // Handle output init segment
});

client->SetOutputFragmentCallback([](const uint8_t* data, size_t size) {
    // Handle output fragment
});

// Push input data
client->PushInputInit(init_data, init_size);
client->PushInputFragment(fragment_data, fragment_size);

// Finalize
client->Flush();
```

### 2. qc_transcode Application (`qc_transcode.cpp`)

Example application demonstrating how to use the TranscodeClient in a Media-over-QUIC subscriber context.

### 3. TranscodeSubscribeTrackHandler

Example track handler that:
- Receives CMAF fragments from MoQ stream
- Passes them to TranscodeClient
- Outputs transcoded stream to file or forwards to another stage

## Dependencies

### Required FFmpeg Libraries

The transcode client requires FFmpeg development libraries:

- **libavcodec** - Video codec support (decoding/encoding)
- **libavformat** - Container format handling (MP4/CMAF)
- **libavutil** - Utility functions
- **libswscale** - Video scaling

### Installing FFmpeg on Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    pkg-config
```

### Installing FFmpeg on Fedora/RHEL

```bash
sudo dnf install -y \
    ffmpeg-devel \
    pkg-config
```

### Installing FFmpeg on macOS

```bash
brew install ffmpeg pkg-config
```

## Building

### Build the Project

From the libquicr root directory:

```bash
mkdir -p build
cd build
cmake .. -DQUICR_BUILD_TESTS=ON
make qc_transcode
```

The `qc_transcode` executable will be built in `build/cmd/examples/`.

### Build Options

You can customize the build with CMake options:

```bash
cmake .. \
    -DQUICR_BUILD_TESTS=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DLINT=OFF
make qc_transcode
```

## Usage

### Running qc_transcode

Basic usage:

```bash
./qc_transcode \
    --url moq://relay-server:1234 \
    --sub_namespace video/camera1 \
    --sub_name stream \
    --width 1280 \
    --height 720 \
    --output transcoded_stream
```

### Command Line Options

**Connection Options:**
- `--url <uri>` - MoQ relay server URL (default: `moq://localhost:1234`)
- `--endpoint_id <id>` - Client endpoint ID (default: `moq-transcode`)

**Subscription Options:**
- `--sub_namespace <ns>` - Track namespace to subscribe (required)
- `--sub_name <name>` - Track name to subscribe (required)

**Transcode Options:**
- `--width <pixels>` - Target output width (default: 1280)
- `--height <pixels>` - Target output height (default: 720)
- `--bitrate <bps>` - Target bitrate in bits/sec (0=auto, default: 0)
- `--preset <name>` - Encoder preset: ultrafast, fast, medium, slow, veryslow (default: medium)
- `--output <prefix>` - Output file prefix for saving transcoded stream

**Debug Options:**
- `--debug` - Enable debug logging
- `--qlog <path>` - Enable QUIC logging
- `--ssl_keylog` - Enable SSL key logging

### Example Scenarios

#### 1. Basic Transcoding (1080p → 720p)

```bash
./qc_transcode \
    --url moq://relay:1234 \
    --sub_namespace live/conference \
    --sub_name main-video \
    --width 1280 \
    --height 720 \
    --output conference_720p
```

#### 2. Low-Latency Transcoding (Fast Preset)

```bash
./qc_transcode \
    --url moq://relay:1234 \
    --sub_namespace live/gaming \
    --sub_name gameplay \
    --width 1920 \
    --height 1080 \
    --preset ultrafast \
    --output gameplay_stream
```

#### 3. High-Quality Transcoding with Custom Bitrate

```bash
./qc_transcode \
    --url moq://relay:1234 \
    --sub_namespace vod/movies \
    --sub_name feature \
    --width 3840 \
    --height 2160 \
    --bitrate 20000000 \
    --preset slow \
    --output feature_4k
```

#### 4. Debug Mode

```bash
./qc_transcode \
    --url moq://relay:1234 \
    --sub_namespace test/debug \
    --sub_name stream \
    --width 640 \
    --height 480 \
    --debug \
    --output debug_stream
```

## Integrating into Your Application

### Step 1: Create Configuration

```cpp
#include "transcode_client.h"

quicr::transcode::TranscodeConfig config;
config.target_width = 1280;
config.target_height = 720;
config.target_bitrate = 2000000;  // 2 Mbps
config.encoder_preset = "medium";
config.debug = false;
```

### Step 2: Create TranscodeClient

```cpp
auto transcode_client = quicr::transcode::TranscodeClient::Create(config);
```

### Step 3: Register Callbacks

```cpp
// Handle output init segment
transcode_client->SetOutputInitCallback([](const uint8_t* data, size_t size) {
    // Forward to publisher or save to file
    PublishInitSegment(data, size);
});

// Handle output fragments
transcode_client->SetOutputFragmentCallback([](const uint8_t* data, size_t size) {
    // Forward to publisher or save to file
    PublishFragment(data, size);
});
```

### Step 4: Process Input in Subscribe Handler

```cpp
class MySubscribeHandler : public quicr::SubscribeTrackHandler {
    void ObjectReceived(const quicr::ObjectHeaders& hdr,
                       quicr::BytesSpan data) override {

        // Detect init segment (starts with 'ftyp')
        if (IsInitSegment(data)) {
            transcode_client_->PushInputInit(data.data(), data.size());
        }
        // Detect media fragment (starts with 'moof')
        else if (IsMediaFragment(data)) {
            transcode_client_->PushInputFragment(data.data(), data.size());
        }
    }
};
```

### Step 5: Cleanup

```cpp
// When done
transcode_client->Flush();
transcode_client->Close();
```

## CMAF Fragment Detection

The implementation automatically detects CMAF segments by examining the MP4 box type:

- **Init Segment**: Starts with `ftyp` box (0x66747970)
- **Media Fragment**: Starts with `moof` box (0x6D6F6F66)

```cpp
bool IsInitSegment(const uint8_t* data, size_t size) {
    if (size < 8) return false;
    uint32_t box_type = (data[4] << 24) | (data[5] << 16) |
                        (data[6] << 8) | data[7];
    return box_type == 0x66747970;  // 'ftyp'
}

bool IsMediaFragment(const uint8_t* data, size_t size) {
    if (size < 8) return false;
    uint32_t box_type = (data[4] << 24) | (data[5] << 16) |
                        (data[6] << 8) | data[7];
    return box_type == 0x6D6F6F66;  // 'moof'
}
```

## Timestamp Handling

The TranscodeClient preserves timestamp information:

1. **Input Time Base**: Extracted from input CMAF stream
2. **Frame PTS/DTS**: Copied from decoded frames to scaled frames
3. **Output Rescaling**: Timestamps are rescaled for output stream
4. **Monotonic Timeline**: Ensures output fragments maintain correct timing

This ensures that:
- Playback timing remains correct
- Audio/video sync is maintained (when audio is added)
- Seeking works properly in the output stream

## Performance Considerations

### Encoder Presets

- **ultrafast**: Lowest CPU usage, larger file sizes, lower quality
- **fast**: Good balance for real-time streaming
- **medium**: Default, good quality/speed tradeoff
- **slow**: Higher quality, more CPU intensive
- **veryslow**: Best quality, highest CPU usage

### Threading

The current implementation processes fragments sequentially. For high-throughput scenarios, consider:

- Processing fragments in parallel (separate TranscodeClient per GOP)
- Using hardware acceleration (VAAPI, NVENC, etc.)
- Implementing frame buffering/pipelining

### Memory Usage

- Each TranscodeClient allocates frame buffers (~4-8 MB per instance)
- Output buffers grow dynamically but are cleared after each fragment
- Consider limiting the number of concurrent transcode sessions

## Troubleshooting

### Build Errors

**Error**: `Could not find required package: libavcodec`

**Solution**: Install FFmpeg development libraries (see Dependencies section)

**Error**: `undefined reference to avcodec_*`

**Solution**: Ensure pkg-config can find FFmpeg:
```bash
pkg-config --modversion libavcodec
```

### Runtime Errors

**Error**: `Failed to open input: Invalid data found when processing input`

**Solution**: Ensure input is valid fragmented MP4 (CMAF). Check that init segment is sent before fragments.

**Error**: `Failed to find decoder`

**Solution**: Input codec not supported by your FFmpeg build. Check codec with:
```bash
ffmpeg -codecs | grep h264
```

**Error**: `No video stream found in input`

**Solution**: Input contains no video track, or init segment is corrupted.

## Limitations

Current implementation limitations:

1. **Video Only**: Audio tracks are not yet transcoded (video-only for now)
2. **Single Track**: Only processes the first video track found
3. **Sequential Processing**: Fragments are processed one at a time
4. **H.264/HEVC**: Optimized for these codecs (others may work but untested)

## Future Enhancements

Potential improvements:

- [ ] Audio transcoding support
- [ ] Multi-track support
- [ ] Hardware acceleration (VAAPI, NVENC, QuickSync)
- [ ] Parallel fragment processing
- [ ] Adaptive bitrate support
- [ ] HDR/10-bit color support
- [ ] Multiple output renditions (ABR ladder)

## License

SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
SPDX-License-Identifier: BSD-2-Clause

## Support

For issues or questions:
- Check the troubleshooting section above
- Review FFmpeg documentation: https://ffmpeg.org/documentation.html
- Review CMAF specification: ISO/IEC 23000-19
