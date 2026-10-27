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

import com.google.android.exoplayer2.C;
import com.google.android.exoplayer2.Format;
import com.google.android.exoplayer2.decoder.Decoder;
import com.google.android.exoplayer2.decoder.DecoderInputBuffer;

import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantReadWriteLock;

/**
 * Independent input/output queues: reordered and asynchronous output is not one-to-one with input.
 */
final class LcevcVvdecDecoder
        implements Decoder<DecoderInputBuffer, LcevcVvdecOutputBuffer, LcevcVvdecDecoderException> {
    interface Backend {
        int queue(ByteBuffer data, long pts, boolean decodeOnly);

        long dequeue(int[] info, long[] pts);

        void eof();

        void flush();

        void releasePicture(long picture);

        void close();

        void setSurface(Surface surface);

        int render(long picture);
    }

    private final Object lock = new Object();
    private final Backend backend;
    private final Thread worker;
    private final ArrayDeque<DecoderInputBuffer> freeInputs = new ArrayDeque<>();
    private final ArrayDeque<DecoderInputBuffer> inputs = new ArrayDeque<>();
    private final ArrayDeque<LcevcVvdecOutputBuffer> freeOutputs = new ArrayDeque<>();
    private final ArrayDeque<LcevcVvdecOutputBuffer> outputs = new ArrayDeque<>();
    private DecoderInputBuffer dequeuedInput;
    private volatile Format inputFormat;
    private boolean released;
    private int generation;
    private LcevcVvdecDecoderException error;

    LcevcVvdecDecoder(int threads, int lcevcThreads, boolean gpu)
            throws LcevcVvdecDecoderException {
        this(new NativeBackend(threads, lcevcThreads, gpu));
    }

    // Injectable backend exercises the actual asynchronous queue/lifecycle logic in unit tests.
    LcevcVvdecDecoder(Backend backend) {
        this.backend = backend;
        for (int i = 0; i < 8; ++i) {
            freeInputs.add(
                    new DecoderInputBuffer(DecoderInputBuffer.BUFFER_REPLACEMENT_MODE_DIRECT));
        }
        for (int i = 0; i < 3; ++i) {
            freeOutputs.add(
                    new LcevcVvdecOutputBuffer(
                            buffer -> releaseOutput((LcevcVvdecOutputBuffer) buffer)));
        }
        worker = new Thread(this::run, "LCEVC-VVDEC");
        worker.start();
    }

    @Override
    public String getName() {
        return "vcat-lcevc-vvdec";
    }

    void setInputFormat(Format format) {
        inputFormat = format;
    }

    void setOutputSurface(Surface surface) {
        backend.setSurface(surface);
    }

    void renderToSurface(LcevcVvdecOutputBuffer output) throws LcevcVvdecDecoderException {
        int rc = backend.render(output.nativePic);
        if (rc < 0) {
            throw new LcevcVvdecDecoderException("LCEVC presentation failed: " + rc);
        }
    }

    private void checkError() throws LcevcVvdecDecoderException {
        if (error != null) {
            throw error;
        }
        if (released) {
            throw new LcevcVvdecDecoderException("Decoder released");
        }
    }

    @Override
    public DecoderInputBuffer dequeueInputBuffer() throws LcevcVvdecDecoderException {
        synchronized (lock) {
            checkError();
            if (dequeuedInput != null) {
                throw new IllegalStateException("Input already dequeued");
            }
            dequeuedInput = freeInputs.poll();
            return dequeuedInput;
        }
    }

    @Override
    public void queueInputBuffer(DecoderInputBuffer input) throws LcevcVvdecDecoderException {
        synchronized (lock) {
            checkError();
            if (input != dequeuedInput) {
                throw new IllegalArgumentException("Unexpected input");
            }
            inputs.add(input);
            dequeuedInput = null;
            lock.notifyAll();
        }
    }

    @Override
    public LcevcVvdecOutputBuffer dequeueOutputBuffer() throws LcevcVvdecDecoderException {
        synchronized (lock) {
            checkError();
            return outputs.poll();
        }
    }

    private void recycleInput(DecoderInputBuffer input) {
        input.clear();
        freeInputs.add(input);
    }

    private void releaseOutput(LcevcVvdecOutputBuffer output) {
        synchronized (lock) {
            if (output.nativePic != 0) {
                backend.releasePicture(output.nativePic);
            }
            output.clear();
            freeOutputs.add(output);
            lock.notifyAll();
        }
    }

    private void clearQueues() {
        if (dequeuedInput != null) {
            recycleInput(dequeuedInput);
            dequeuedInput = null;
        }
        while (!inputs.isEmpty()) {
            recycleInput(inputs.remove());
        }
        while (!outputs.isEmpty()) {
            releaseOutput(outputs.remove());
        }
    }

    @Override
    public void flush() {
        synchronized (lock) {
            ++generation;
            error = null;
            clearQueues();
            lock.notifyAll();
        }
    }

    @Override
    public void release() {
        synchronized (lock) {
            if (released) {
                return;
            }
            released = true;
            lock.notifyAll();
        }
        boolean interrupted = false;
        for (; ; ) {
            try {
                worker.join();
                break;
            } catch (InterruptedException e) {
                interrupted = true;
            }
        }
        synchronized (lock) {
            clearQueues();
        }
        backend.close();
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    private void run() {
        DecoderInputBuffer active = null;
        int currentGeneration = 0;
        int skipped = 0;
        boolean endSignaled = false;
        boolean endDelivered = false;
        boolean haveNativeWork = false;
        final int[] info = new int[3];
        final long[] pts = new long[1];
        try {
            for (; ; ) {
                boolean reset;
                synchronized (lock) {
                    while (!released
                            && generation == currentGeneration
                            && (error != null
                                    || endDelivered
                                    || freeOutputs.isEmpty()
                                    || (!haveNativeWork && active == null && inputs.isEmpty()))) {
                        lock.wait();
                    }
                    if (released) {
                        break;
                    }
                    reset = generation != currentGeneration;
                    if (reset) {
                        currentGeneration = generation;
                        if (active != null) {
                            recycleInput(active);
                            active = null;
                        }
                    } else if (active == null && !endSignaled) {
                        active = inputs.poll();
                    }
                }
                try {
                    if (reset) {
                        backend.flush();
                        skipped = 0;
                        endSignaled = endDelivered = haveNativeWork = false;
                        continue;
                    }
                    boolean progress = false;
                    if (active != null) {
                        int rc;
                        if (active.isEndOfStream()) {
                            backend.eof();
                            endSignaled = true;
                            rc = NativeLcevcVvdec.RESULT_OK;
                        } else {
                            if (active.data == null) {
                                throw new LcevcVvdecDecoderException("Missing input data");
                            }
                            rc = backend.queue(active.data, active.timeUs, active.isDecodeOnly());
                        }
                        if (rc != NativeLcevcVvdec.RESULT_OK
                                && rc != NativeLcevcVvdec.ERROR_AGAIN) {
                            throw new LcevcVvdecDecoderException("Decode failed: " + rc);
                        }
                        if (rc == NativeLcevcVvdec.RESULT_OK) {
                            synchronized (lock) {
                                recycleInput(active);
                            }
                            active = null;
                            progress = haveNativeWork = true;
                        }
                    }
                    long picture = backend.dequeue(info, pts);
                    if (info[0] == NativeLcevcVvdec.DEQUEUE_ERROR) {
                        throw new LcevcVvdecDecoderException("Drain failed: " + info[1]);
                    }
                    synchronized (lock) {
                        if (released || generation != currentGeneration) {
                            if (picture != 0) {
                                backend.releasePicture(picture);
                            }
                            continue;
                        }
                        if (picture != 0 && info[2] != 0) {
                            backend.releasePicture(picture);
                            ++skipped;
                            progress = true;
                        } else if (picture != 0
                                || info[0] == NativeLcevcVvdec.DEQUEUE_END_OF_STREAM) {
                            LcevcVvdecOutputBuffer output = freeOutputs.remove();
                            output.nativePic = picture;
                            output.timeUs = pts[0];
                            output.mode = C.VIDEO_OUTPUT_MODE_SURFACE_YUV;
                            output.width = info[0];
                            output.height = info[1];
                            output.format = inputFormat;
                            output.skippedOutputBufferCount = skipped;
                            skipped = 0;
                            if (info[0] == NativeLcevcVvdec.DEQUEUE_END_OF_STREAM) {
                                output.addFlag(C.BUFFER_FLAG_END_OF_STREAM);
                                endDelivered = true;
                            }
                            outputs.add(output);
                            progress = true;
                        }
                        // Only poll when native work is pending. Input/output releases wake us
                        // early.
                        if (!progress) {
                            lock.wait(2);
                        }
                    }
                } catch (Exception e) {
                    synchronized (lock) {
                        if (generation == currentGeneration) {
                            error =
                                    e instanceof LcevcVvdecDecoderException
                                            ? (LcevcVvdecDecoderException) e
                                            : new LcevcVvdecDecoderException(e);
                        }
                    }
                }
            }
        } catch (InterruptedException e) {
            synchronized (lock) {
                error = new LcevcVvdecDecoderException(e);
            }
            Thread.currentThread().interrupt();
        } finally {
            synchronized (lock) {
                if (active != null) {
                    recycleInput(active);
                }
            }
        }
    }

    private static final class NativeBackend implements Backend {
        private final ReentrantReadWriteLock lifecycleLock = new ReentrantReadWriteLock();
        private final Lock lifecycleRead = lifecycleLock.readLock();
        private final Lock lifecycleWrite = lifecycleLock.writeLock();
        private final Object picturesLock = new Object();
        private final Object renderReleaseLock = new Object();
        private long context;
        private final java.util.Set<Long> pictures = new java.util.LinkedHashSet<>();

        NativeBackend(int threads, int lcevcThreads, boolean gpu)
                throws LcevcVvdecDecoderException {
            context = NativeLcevcVvdec.nativeCreate(threads, lcevcThreads, gpu ? 1 : 0);
            if (context == 0) {
                throw new LcevcVvdecDecoderException("Native decoder initialization failed");
            }
        }

        @Override
        public int queue(ByteBuffer data, long pts, boolean decodeOnly) {
            lifecycleRead.lock();
            try {
                return context == 0
                        ? NativeLcevcVvdec.ERROR_NO_DEVICE
                        : NativeLcevcVvdec.nativeQueueInput(
                                context, data, data.position(), data.remaining(), pts, decodeOnly);
            } finally {
                lifecycleRead.unlock();
            }
        }

        @Override
        public long dequeue(int[] info, long[] pts) {
            lifecycleRead.lock();
            try {
                if (context == 0) {
                    return 0;
                }
                long picture = NativeLcevcVvdec.nativeDequeueFrame(context, info, pts);
                if (picture != 0) {
                    synchronized (picturesLock) {
                        pictures.add(picture);
                    }
                }
                return picture;
            } finally {
                lifecycleRead.unlock();
            }
        }

        @Override
        public void eof() {
            lifecycleRead.lock();
            try {
                if (context != 0) {
                    NativeLcevcVvdec.nativeSignalEof(context);
                }
            } finally {
                lifecycleRead.unlock();
            }
        }

        @Override
        public void flush() {
            lifecycleRead.lock();
            try {
                if (context != 0) {
                    NativeLcevcVvdec.nativeFlush(context);
                }
            } finally {
                lifecycleRead.unlock();
            }
        }

        @Override
        public void releasePicture(long picture) {
            lifecycleRead.lock();
            try {
                synchronized (renderReleaseLock) {
                    boolean owned;
                    synchronized (picturesLock) {
                        owned = pictures.remove(picture);
                    }
                    if (owned && context != 0) {
                        NativeLcevcVvdec.nativeReleasePicture(context, picture);
                    }
                }
            } finally {
                lifecycleRead.unlock();
            }
        }

        @Override
        public void close() {
            lifecycleWrite.lock();
            try {
                if (context == 0) {
                    return;
                }
                synchronized (picturesLock) {
                    for (long picture : pictures) {
                        NativeLcevcVvdec.nativeReleasePicture(context, picture);
                    }
                    pictures.clear();
                }
                NativeLcevcVvdec.nativeClose(context);
                context = 0;
            } finally {
                lifecycleWrite.unlock();
            }
        }

        @Override
        public void setSurface(Surface surface) {
            lifecycleRead.lock();
            try {
                if (context != 0) {
                    NativeLcevcVvdec.nativeSetSurface(context, surface);
                }
            } finally {
                lifecycleRead.unlock();
            }
        }

        @Override
        public int render(long picture) {
            lifecycleRead.lock();
            try {
                if (context == 0) {
                    return NativeLcevcVvdec.ERROR_NO_DEVICE;
                }
                synchronized (renderReleaseLock) {
                    synchronized (picturesLock) {
                        if (!pictures.contains(picture)) {
                            return NativeLcevcVvdec.RESULT_OK;
                        }
                    }
                    return NativeLcevcVvdec.nativeRenderToSurface(context, picture);
                }
            } finally {
                lifecycleRead.unlock();
            }
        }
    }
}
