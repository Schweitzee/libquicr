# GStreamer-Based Video Transcoding Module for Media-over-QUIC

This module implements a C++ video transcoding client based on GStreamer pipelines, designed for use with the libquicr Media-over-QUIC (MoQ) library.

## Overview

The `TranscodeClient` class provides a complete solution for:
- Receiving fragmented CMAF/MP4 video streams
- Decoding and rescaling video to target resolutions
- Re-encoding with configurable codec settings
- Outputting fragmented CMAF/MP4 streams
- Preserving timing information through the pipeline

## Architecture

### Components

1. **TranscodeClient** (`transcode_client.h/cpp`)
   - Main transcoding class with GStreamer pipeline integration
   - Handles input/output data flow
   - Exposes callbacks for processed video segments

2. **CMAFParser** (internal to `transcode_client.cpp`)
   - Parses fragmented MP4 bytestream
   - Extracts init segments (ftyp + moov boxes)
   - Extracts media fragments (moof + mdat box pairs)
   - Invokes appropriate callbacks

3. **qc_transcode.cpp**
   - Example integration with libquicr MoQ layer
   - Demonstrates `DoSubscriber` function pattern
   - Shows how to connect input/output tracks through the transcode client

### GStreamer Pipeline

```
appsrc → qtdemux → h264parse → avdec_h264 → videoconvert → videoscale →
  capsfilter → x264enc → h264parse → mp4mux → appsink
```

**Pipeline Flow:**
1. `appsrc`: Receives fragmented MP4 input from C++ code
2. `qtdemux`: Demuxes MP4 container to extract video stream
3. `h264parse`: Parses H.264 elementary stream
4. `avdec_h264`: Decodes H.264 video
5. `videoconvert`: Converts pixel formats if needed
6. `videoscale`: Scales video to target resolution
7. `capsfilter`: Enforces target width/height constraints
8. `x264enc`: Re-encodes video with low-latency settings
9. `h264parse`: Parses encoded stream again
10. `mp4mux`: Remuxes into fragmented MP4/CMAF format
11. `appsink`: Delivers output to C++ code

## API Usage

### Basic Example

```cpp
#include "transcode_client.h"

// 1. Configure transcoding parameters
transcode::TranscodeConfig config;
config.target_width = 640;
config.target_height = 480;
config.target_bitrate = 500000;  // 500 kbps
config.codec = "x264";
config.low_latency = true;
config.gop_size = 60;

// 2. Create transcode client
auto client = std::make_shared<transcode::TranscodeClient>(config);

// 3. Set output callbacks
client->SetOutputInitCallback([](const uint8_t* data, size_t size) {
    // Handle transcoded init segment (ftyp + moov)
    PublishInitSegment(data, size);
});

client->SetOutputFragmentCallback([](const uint8_t* data, size_t size) {
    // Handle transcoded media fragment (moof + mdat)
    PublishFragment(data, size);
});

// 4. Start pipeline
if (!client->Start()) {
    std::cerr << "Failed to start transcode pipeline" << std::endl;
    return;
}

// 5. Feed input data
// When you receive CMAF init segment:
client->PushInputInit(init_data, init_size);

// When you receive CMAF media fragments:
client->PushInputFragment(fragment_data, fragment_size);

// 6. Cleanup when done
client->Flush();
client->Stop();
```

### Integration with MoQ (DoSubscriber Pattern)

```cpp
void DoSubscriber(quicr::Client& client,
                  const quicr::FullTrackName& input_track,
                  const quicr::FullTrackName& output_track,
                  uint32_t target_width,
                  uint32_t target_height)
{
    // Create transcode client
    transcode::TranscodeConfig config;
    config.target_width = target_width;
    config.target_height = target_height;
    auto transcode_client = std::make_shared<transcode::TranscodeClient>(config);

    // Create output publisher
    auto output_publisher = std::make_shared<MyPublishTrackHandler>(output_track);

    // Wire up callbacks
    transcode_client->SetOutputInitCallback([output_publisher](auto* data, auto size) {
        output_publisher->PublishInit(data, size);
    });

    transcode_client->SetOutputFragmentCallback([output_publisher](auto* data, auto size) {
        output_publisher->PublishFragment(data, size);
    });

    // Create subscriber that feeds transcode client
    auto subscriber = std::make_shared<MySubscribeTrackHandler>(
        input_track,
        [transcode_client](const auto& hdr, auto data) {
            if (hdr.object_id == 0) {
                transcode_client->PushInputInit(data.data(), data.size());
                transcode_client->Start();
            } else {
                transcode_client->PushInputFragment(data.data(), data.size());
            }
        }
    );

    // Connect to MoQ
    client.Connect(output_publisher);
    client.Connect(subscriber);
}
```

## Building

### Dependencies

1. **CMake** (>= 3.13)
2. **C++20 compiler** (GCC, Clang, or MSVC)
3. **GStreamer 1.x** with development headers
4. **GStreamer plugins**:
   - gst-plugins-base (qtdemux, videoscale, videoconvert, appsrc, appsink)
   - gst-plugins-good (mp4mux)
   - gst-plugins-bad or gst-plugins-ugly (x264enc, avdec_h264)

### Installing GStreamer on Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-tools \
    gstreamer1.0-x
```

### Installing GStreamer on macOS

```bash
brew install gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-plugins-ugly gst-libav
```

### Building the Project

```bash
mkdir build
cd build
cmake ..
make qc_transcode
```

The build system will automatically detect GStreamer. If not found, you'll see:
```
WARNING: GStreamer or GStreamer-app not found. qc_transcode will not be built.
```

## Configuration

### TranscodeConfig Options

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `target_width` | `uint32_t` | 640 | Output video width in pixels |
| `target_height` | `uint32_t` | 480 | Output video height in pixels |
| `target_bitrate` | `uint32_t` | 500000 | Target bitrate in bits/second |
| `codec` | `std::string` | "x264" | Video codec ("x264", "x265") |
| `gop_size` | `uint32_t` | 60 | GOP (keyframe interval) size |
| `low_latency` | `bool` | true | Enable low-latency encoding mode |

## Input/Output Format

### Input Requirements
- **Container**: Fragmented MP4 (CMAF/fMP4)
- **Structure**:
  - First object: Init segment (ftyp + moov boxes)
  - Subsequent objects: Media fragments (moof + mdat boxes)
- **Video codec**: H.264 (other codecs may work but are untested)

### Output Format
- **Container**: Fragmented MP4 (CMAF/fMP4)
- **Structure**: Same as input (init segment + fragments)
- **Video codec**: Configurable (default: H.264 via x264enc)
- **Fragments**: Typically 1-second duration

## Timing and Synchronization

The TranscodeClient preserves timing information through the pipeline:

1. **Input timestamps**: Derived from CMAF timing metadata
2. **appsrc configuration**: Uses `format=GST_FORMAT_TIME` for timestamped data
3. **Pipeline processing**: GStreamer maintains timestamps through decode/encode
4. **Output timestamps**: Embedded in output CMAF fragments

For live streaming, the pipeline is configured with:
- `appsrc is-live=true` for live streaming mode
- Low-latency encoder settings (ultrafast preset, zerolatency tune)
- Minimal buffering throughout the pipeline

## Limitations and Future Improvements

### Current Limitations
1. **Codec support**: Currently hardcoded for H.264 input/output
2. **Audio**: Not yet implemented (video-only)
3. **Error handling**: Basic error handling; production use needs enhancement
4. **CMAF parsing**: Simple box-level parser; doesn't validate full MP4 spec
5. **Threading**: Single-threaded; GStreamer handles internal threading

### Potential Improvements
1. **Multiple codecs**: Support VP8/VP9, AV1, HEVC
2. **Audio transcoding**: Add audio pipeline path
3. **Adaptive bitrate**: Dynamic bitrate adjustment based on network conditions
4. **Hardware acceleration**: Use VAAPI, NVENC, VideoToolbox for encoding/decoding
5. **More robust parsing**: Full MP4 box parser with validation
6. **Statistics**: Expose quality metrics, latency measurements
7. **Configuration**: Runtime pipeline reconfiguration

## Testing

### Manual Testing with GStreamer CLI

You can test the pipeline components separately:

```bash
# Generate test fragmented MP4
gst-launch-1.0 videotestsrc num-buffers=300 ! \
    video/x-raw,width=1280,height=720,framerate=30/1 ! \
    x264enc ! h264parse ! \
    mp4mux fragment-duration=1000 ! \
    filesink location=test_input.mp4

# Test transcoding pipeline manually
gst-launch-1.0 filesrc location=test_input.mp4 ! \
    qtdemux ! h264parse ! avdec_h264 ! \
    videoscale ! video/x-raw,width=640,height=480 ! \
    x264enc bitrate=500 speed-preset=ultrafast tune=zerolatency ! \
    h264parse ! mp4mux fragment-duration=1000 ! \
    filesink location=test_output.mp4
```

### Unit Testing

TODO: Add unit tests for:
- CMAFParser box parsing
- Pipeline creation and configuration
- Callback invocation
- Error conditions

## Performance Considerations

1. **CPU usage**: Software encoding is CPU-intensive; consider hardware acceleration
2. **Latency**: Typical glass-to-glass latency is 100-500ms depending on GOP size
3. **Memory**: GStreamer buffers several frames; ~10-50MB per pipeline instance
4. **Throughput**: Single instance can handle 720p30 @ ~500kbps on modern CPUs

## Troubleshooting

### Pipeline Fails to Start

1. Check GStreamer element availability:
   ```bash
   gst-inspect-1.0 qtdemux
   gst-inspect-1.0 x264enc
   ```

2. Enable GStreamer debug logging:
   ```bash
   export GST_DEBUG=3
   ./qc_transcode
   ```

### No Output Callbacks

1. Verify input data is valid CMAF/fMP4
2. Check that `Start()` was called before pushing fragments
3. Ensure init segment was pushed before fragments

### Video Quality Issues

1. Increase `target_bitrate` in config
2. Adjust `gop_size` (smaller = better quality, more keyframes)
3. Change encoder preset (x264enc `speed-preset`)

## References

- [GStreamer Documentation](https://gstreamer.freedesktop.org/documentation/)
- [CMAF Specification (ISO/IEC 23000-19)](https://www.iso.org/standard/79106.html)
- [Media over QUIC (MoQ)](https://datatracker.ietf.org/wg/moq/about/)
- [libquicr Project](https://github.com/Quicr/libquicr)

## License

SPDX-FileCopyrightText: Copyright (c) 2024 Cisco Systems
SPDX-License-Identifier: BSD-2-Clause
