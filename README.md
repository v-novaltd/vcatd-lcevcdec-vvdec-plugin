# vcatd-lcevcdec-vvdec-plugin

MPEG-5 LCEVC VVC decoder plugin for [VCAT (Video Codec Acid Test)](https://github.com/Video-Codec-Acid-Test-VCAT/vcat-d).

This is a fork of the [vcatd-vvdec-plugin](https://github.com/Video-Codec-Acid-Test-VCAT/vcatd-vvdec-plugin) (fork commit [`4b7ea711`](https://github.com/Video-Codec-Acid-Test-VCAT/vcatd-vvdec-plugin/commit/4b7ea7118e347ca00c7cff9d966ed2519ef9f941)): in a single plugin it decodes the VVC base layer with [VVdeC](https://github.com/fraunhoferhhi/vvdec) and applies the LCEVC enhancement layer with [LCEVCdec](https://github.com/v-novaltd/LCEVCdec). This VCAT-d plugin is compatible with the VCAT-d 1.0.4 plugin interface.

It registers MIME `video/vvc` with the `vcat.lcevcdec-vvdec-vulkan` and `vcat.lcevcdec-vvdec-cpu`
plugins to access the CPU and Vulkan pipelines of LCEVCdec. The `vcat.vvdec` plugin can be
enabled alongside the LCEVCdec plugins and is selectable per-stream in VCAT for side by side testing.

## How it works

For each VVC access unit (single-track, LCEVC enhancement carried in-band as SEI):

1. The LCEVC enhancement payload is extracted and submitted before base decoding.
2. `vvdec` decodes the base picture using the Java direct input buffer.
3. The base frame is copied into an LCEVC-owned picture; the VVdeC frame is released immediately.
4. LCEVCdec decodes the enhanced picture directly into an `AHardwareBuffer`, which is presented with SurfaceControl at ExoPlayer's render time (see below).

When no enhancement is present, the base passes through unchanged.

### Hardware buffer output

The plugin presents frames itself rather than through an LCEVCdec renderer. Each output picture is
allocated with `LCEVC_AllocPictureExternal()` on an `AHardwareBuffer` from a small pool, so LCEVCdec
writes the decoded frame straight into the buffer that is put on screen:

1. A free pool buffer is acquired; decoding waits if none is free. With Vulkan the buffer is a
   GPU-only RGBA overlay buffer that LCEVCdec writes as it converts from YUV, and it stays bound to
   its LCEVC picture so it is only imported once.
2. Once `LCEVC_ReceiveDecoderPicture()` returns, the buffer is complete and is handed to Java as an
   opaque handle in the ExoPlayer output buffer.
3. At render time it is attached to a child `ASurfaceControl` of the output `Surface` with
   `ASurfaceTransaction_setBuffer()`, scaled to fit, and composited by SurfaceFlinger, as a hardware
   overlay where the device supports it.
4. When the next frame replaces it on screen, the transaction callback returns it to the pool with
   SurfaceFlinger's release fence. Dropped frames return to the pool directly.

The CPU pipeline decodes 8-bit NV12 into CPU-writable buffers in the same way. Pictures the decoder
cannot write into a buffer, such as CPU 10-bit output, are copied into one instead.

## Build

```bash
git clone https://github.com/v-novaltd/vcatd-lcevcdec-vvdec-plugin
cd vcatd-lcevcdec-vvdec-plugin
./gradlew :vcatd-lcevcdec-vvdec-plugin:dist
```

System Perfetto tracing is optional and uses Conan to gather Perfetto and fmt dependencies:

```bash
./gradlew -PbuildWithConan=TRUE -PbuildWithPerfettoSystemSink=TRUE :vcatd-lcevcdec-vvdec-plugin:dist
```

This enables the `lcevc.dec`, `lcevc.dec.jni`, and `lcevc.dec.vulkan` Perfetto categories. The plugin also emits `jni:vcat.*` diagnostics for its JNI and vvdec/LCEVC orchestration.

LCEVCdec defaults to `https://github.com/v-novaltd/LCEVCdec.git` at `main`.
Use `-PlcevcRemote=<repository>` and `-PlcevcRef=<branch-or-tag>` to select a specific version.
To build from a local checkout, use the `lcevcLocal` Gradle property or the `LCEVC_SRC` 
environment variable, for example:

```bash
./gradlew -PlcevcLocal="$PWD/../LCEVCdec" :vcatd-lcevcdec-vvdec-plugin:dist
```

Outputs: `app/build/outputs/dist/vcatd-lcevcdec-vvdec-vulkan-plugin.aar` and
`app/build/outputs/dist/vcatd-lcevcdec-vvdec-cpu-plugin.aar`.

The build fetches and statically builds, per ABI:
- vvdec (Fraunhofer H.266 decoder)
- LCEVCdec (V-Nova MPEG-5 LCEVC decoder SDK, CPU and Vulkan pipelines) + its `extract` helper library
- Perfetto 48.1 and fmt when system tracing is enabled

## Using the plugin with VCAT

Copy both AARs into VCAT's `decoder-plugins` directory before building the app.
The AAR manifests name `LcevcVvdecLibrary` as their native loader so the stock
VCAT plugin loader can load JNI before it queries the decoder version.
Under `video/vvc` in Decoder Configuration, select
`vcat.lcevcdec-vvdec-vulkan` or `vcat.lcevcdec-vvdec-cpu`. Both use two
LCEVCdec worker threads by default. In V-Nova's testing, the Vulkan pipeline gives 
superior performance and battery life on real Android phones.

## Contributions

We are not currently accepting contributions to this project however please raise GitHub issues 
for any problems or feedback.

## License

This plugin is licensed under GPL-3.0-or-later. See [LICENSE](LICENSE).

LCEVCdec is licensed under [BSD-3-Clause-Clear](licenses/LCEVCdec-LICENSE.txt).

VVdeC is licensed under [Clear BSD](licenses/VVDEC-LICENSE.txt).

The [VvcNalUnitUtil.java](app/src/main/java/com/roncatech/libvcat/lcevcvvdec/VvcNalUnitUtil.java) source file is taken from [AOSP](https://source.android.com/) and is licensed under [Apache 2.0](licenses/APACHE-2.0-LICENSE.txt)

This project is derived from the original [vcatd-vvdec-plugin](https://github.com/Video-Codec-Acid-Test-VCAT/vcatd-vvdec-plugin). Original upstream copyright and license notices are preserved in the relevant source files.

Fraunhofer VVdeC and V-Nova LCEVCdec are separately licensed under their respective BSD-3-Clause-Clear licenses, including the applicable patent exclusions. Their separate licensing terms are not replaced by the GPL license for the integration plugin.

See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and the licenses in the licenses/ directory for further information.
