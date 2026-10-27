/*
 * VCAT (Video Codec Acid Test)
 *
 * SPDX-FileCopyrightText: Copyright (C) 2020-2025 VCAT authors and RoncaTech
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of VCAT.
 *
 * VCAT is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VCAT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with VCAT. If not, see <https://www.gnu.org/licenses/gpl-3.0.html>.
 *
 * For proprietary/commercial use cases, a written GPL-3.0 waiver or
 * a separate commercial license is required from RoncaTech LLC.
 *
 * All VCAT artwork is owned exclusively by RoncaTech LLC. Use of VCAT logos
 * and artwork is permitted for the purpose of discussing, documenting,
 * or promoting VCAT itself. Any other use requires prior written permission
 * from RoncaTech LLC.
 *
 * Contact: legal@roncatech.com
 */
/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 V-Nova International Limited
 *
 * Modified by V-Nova International Limited, to support integration with LCEVCdec.
 * V-Nova's modifications are licensed under GPL-3.0-or-later.
 */

package com.roncatech.libvcat.lcevcvvdec;

import android.os.Handler;
import android.util.Log;
import android.view.Surface;

import com.google.android.exoplayer2.C;
import com.google.android.exoplayer2.Format;
import com.google.android.exoplayer2.RendererCapabilities;
import com.google.android.exoplayer2.decoder.CryptoConfig; // ExoPlayer 2.x
import com.google.android.exoplayer2.decoder.Decoder;
import com.google.android.exoplayer2.decoder.DecoderException;
import com.google.android.exoplayer2.decoder.DecoderInputBuffer;
import com.google.android.exoplayer2.decoder.DecoderReuseEvaluation;
import com.google.android.exoplayer2.decoder.VideoDecoderOutputBuffer;
import com.google.android.exoplayer2.video.DecoderVideoRenderer;
import com.google.android.exoplayer2.video.VideoRendererEventListener;

/** Software VVC (H.266) renderer using vvdec via JNI, patterned after the dav1d path. */
public final class LcevcVvdecVideoRenderer extends DecoderVideoRenderer {

    private static final String TAG = "LcevcVvdecVideoRenderer";
    private static final int MAX_DROPPED_FRAMES_TO_NOTIFY = 50;

    private final int threads;
    private final int lcevcThreads;
    private final boolean lcevcGpu;

    private LcevcVvdecDecoder decoder;
    private Surface currentSurface;

    public LcevcVvdecVideoRenderer(
            long allowedJoiningTimeMs,
            Handler eventHandler,
            VideoRendererEventListener eventListener,
            int threads,
            int lcevcThreads,
            boolean lcevcGpu) {
        super(allowedJoiningTimeMs, eventHandler, eventListener, MAX_DROPPED_FRAMES_TO_NOTIFY);
        this.threads = Math.max(1, threads);
        this.lcevcThreads = lcevcThreads;
        this.lcevcGpu = lcevcGpu;
    }

    @Override
    public void handleMessage(int messageType, Object message)
            throws com.google.android.exoplayer2.ExoPlaybackException {
        super.handleMessage(messageType, message);
        if (messageType == com.google.android.exoplayer2.Renderer.MSG_SET_VIDEO_OUTPUT) {
            currentSurface = message instanceof Surface ? (Surface) message : null;
            if (decoder != null) {
                decoder.setOutputSurface(currentSurface);
            }
        }
    }

    @Override
    public String getName() {
        return "LcevcVvdecVideoRenderer";
    }

    @Override
    protected Decoder<
                    DecoderInputBuffer,
                    ? extends VideoDecoderOutputBuffer,
                    ? extends DecoderException>
            createDecoder(Format format, CryptoConfig cryptoConfig)
                    throws LcevcVvdecDecoderException {
        LcevcVvdecLibrary.load();
        this.decoder = new LcevcVvdecDecoder(threads, lcevcThreads, lcevcGpu);
        if (this.currentSurface != null) {
            this.decoder.setOutputSurface(this.currentSurface);
        }
        this.decoder.setInputFormat(format);
        return this.decoder;
    }

    @Override
    protected void renderOutputBufferToSurface(
            VideoDecoderOutputBuffer outputBuffer, Surface surface)
            throws LcevcVvdecDecoderException {
        if (decoder == null) {
            throw new LcevcVvdecDecoderException(
                    "Failed to render output buffer to surface: decoder is not initialized.");
        }
        if (surface != currentSurface) {
            currentSurface = surface; // may be null
            decoder.setOutputSurface(currentSurface); // JNI caches/releases ANativeWindow
        }

        try {
            decoder.renderToSurface((LcevcVvdecOutputBuffer) outputBuffer);
        } finally {
            outputBuffer.release();
        }
    }

    @Override
    protected void setDecoderOutputMode(int outputMode) {
        // Native pictures are presented directly to the Surface.
        if (outputMode != C.VIDEO_OUTPUT_MODE_YUV
                && outputMode != C.VIDEO_OUTPUT_MODE_SURFACE_YUV
                && outputMode != C.VIDEO_OUTPUT_MODE_NONE) {
            throw new IllegalArgumentException("Unsupported output mode: " + outputMode);
        }
    }

    @Override
    protected DecoderReuseEvaluation canReuseDecoder(String name, Format oldF, Format newF) {
        // Re-init on format change (simplest + safest for benchmarking).
        return new DecoderReuseEvaluation(
                name, oldF, newF, DecoderReuseEvaluation.REUSE_RESULT_NO, /* discardReasons */ 0);
    }

    @Override
    public int supportsFormat(Format format) {
        final String mime = format.sampleMimeType;

        // ExoPlayer 2.x doesn't define a constant for VVC; accept common strings.
        final boolean isVvc = "video/vvc".equals(mime) || "video/h266".equals(mime);
        if (!isVvc) {
            return RendererCapabilities.create(C.FORMAT_UNSUPPORTED_TYPE);
        }
        if (format.drmInitData != null) {
            // SW path is clear-only
            return RendererCapabilities.create(C.FORMAT_UNSUPPORTED_DRM);
        }
        return RendererCapabilities.create(C.FORMAT_HANDLED);
    }

    @Override
    protected boolean shouldDropOutputBuffer(long earlyUs, long elapsedRealtimeUs) {
        // Drop only if >50 ms late (tune as needed for tests).
        return earlyUs < -50_000;
    }

    @Override
    protected boolean shouldDropBuffersToKeyframe(long earlyUs, long elapsedRealtimeUs) {
        // Seek forward to keyframe if we're ~0.5 s behind.
        boolean veryLate = earlyUs < -500_000;
        if (veryLate) {
            // Telemetry reports how many frames a skip discarded, but not how late the renderer
            // believed it was when it asked -- and that is the number in dispute. Runs have
            // reported thousands of frames discarded in one interval with zero drops anywhere in
            // the body of the clip, which cannot come from falling behind gradually: reaching
            // this threshold means first spending a long time past shouldDropOutputBuffer's
            // 50 ms, which would leave a trail. Logging earlyUs here distinguishes a decoder
            // that really is seconds late from a discontinuity in the timestamps it is compared
            // against. Rate-limited to one line a second so a genuine stall cannot flood.
            long nowMs = elapsedRealtimeUs / 1000L;
            if (nowMs - lastVeryLateLogMs > 1000L) {
                lastVeryLateLogMs = nowMs;
                Log.w(
                        TAG,
                        "VERY-LATE earlyUs="
                                + earlyUs
                                + " ("
                                + (earlyUs / 1000L)
                                + " ms late)"
                                + " consecutive="
                                + (++veryLateCount));
            }
        }
        return veryLate;
    }

    /** Diagnostics for the skip-to-keyframe threshold; see shouldDropBuffersToKeyframe. */
    private long lastVeryLateLogMs = 0L;

    private int veryLateCount = 0;

    // shouldForceRenderOutputBuffer is deliberately NOT overridden.
    //
    // It used to return `earlyUs <= 0` -- "render when due or slightly late". In
    // DecoderVideoRenderer.processOutputBuffer the force-render check runs *before* the drop
    // checks, so that rendered every late frame and left shouldDropOutputBuffer below it
    // unreachable. Playback then ran at whatever rate the decoder managed instead of dropping to
    // stay on the clock.
    //
    // That makes a benchmark actively misleading, because a decoder that cannot keep up is
    // rewarded. Measured on CPH2625 with the 604.8 s / 29.97 fps VVC stream: vvdec 3.0.0
    // sustained 3.8 loops/hour against a realtime 5.95, i.e. 0.64x -- playing in slow motion,
    // reporting frames_dropped = 0, and lasting 8.0 h, while 3.2.0 played at 0.97x realtime and
    // "only" lasted 6.57 h. The slower decoder looked better.
    //
    // VCAT measures user experience, so playback has to advance at the content's frame rate and
    // shed frames when the decoder falls behind -- what a video player does. ExoPlayer's default
    // gives that: force-render only when late AND more than 100 ms since the last render, which
    // keeps the stall guard and leaves shouldDropOutputBuffer to decide.

    @Override
    protected void onDisabled() {
        try {
            if (decoder != null) {
                decoder.setOutputSurface(null);
            }
            currentSurface = null;
        } finally {
            super.onDisabled();
        }
    }
}
