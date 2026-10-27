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

import android.view.Surface;

import java.nio.ByteBuffer;

public final class NativeLcevcVvdec {

    public static final int RESULT_OK = 0;
    public static final int ERROR_IO = -5;
    public static final int ERROR_AGAIN = -11;
    public static final int ERROR_NO_DEVICE = -19;

    public static final int DEQUEUE_PENDING = 0;
    public static final int DEQUEUE_ERROR = -1;
    public static final int DEQUEUE_END_OF_STREAM = -2;

    // Non-positive LCEVC threads use the SDK default; nonzero gpuPipeline selects Vulkan.
    public static native long nativeCreate(int threads, int lcevcThreads, int gpuPipeline);

    // Flushes decoder state (drains/clears internal queues).
    public static native void nativeFlush(long ctx);

    // Destroys the decoder context and frees resources.
    public static native void nativeClose(long ctx);

    public static native void nativeSignalEof(long ctx);

    // Queues one compressed sample (direct ByteBuffer required).
    // Returns RESULT_OK on success, ERROR_AGAIN for backpressure, or another negative errno.
    public static native int nativeQueueInput(
            long ctx, ByteBuffer buffer, int offset, int size, long ptsUs, boolean decodeOnly);

    // Attempts to dequeue a decoded frame.
    // Returns 0 if no frame yet; otherwise a non-zero native handle.
    // outWidthHeight[0]=w, [1]=h, [2]=decodeOnly; outPtsUs[0]=pts.
    // No picture: width=0 pending, DEQUEUE_ERROR (height=code), or DEQUEUE_END_OF_STREAM.
    public static native long nativeDequeueFrame(long ctx, int[] outWidthHeight, long[] outPtsUs);

    // Presents the picture's hardware buffer via SurfaceControl.
    // Returns 0 on success; negative on error.
    public static native int nativeRenderToSurface(long ctx, long nativePic);

    // Releases a previously dequeued native picture handle.
    public static native void nativeReleasePicture(long ctx, long nativePic);

    // Get the vvdec decoder version string (e.g., "3.0.0").
    public static native String vvdecGetVersion();

    // Binds/unbinds the output Surface (cached as ANativeWindow in JNI).
    public static native void nativeSetSurface(long handle, Surface surface);

    private NativeLcevcVvdec() {}
}
