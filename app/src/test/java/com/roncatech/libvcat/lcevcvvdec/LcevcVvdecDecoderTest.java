package com.roncatech.libvcat.lcevcvvdec;

import static org.junit.Assert.*;

import android.view.Surface;

import com.google.android.exoplayer2.C;
import com.google.android.exoplayer2.decoder.DecoderInputBuffer;

import org.junit.Test;

import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public class LcevcVvdecDecoderTest {
    private static final class Frame {
        final long pts;
        final boolean decodeOnly;

        Frame(long pts, boolean decodeOnly) {
            this.pts = pts;
            this.decodeOnly = decodeOnly;
        }
    }

    private static final class Fake implements LcevcVvdecDecoder.Backend {
        final List<Long> accepted = new ArrayList<>();
        final List<Long> released = new ArrayList<>();
        final ArrayDeque<Frame> frames = new ArrayDeque<>();
        boolean backpressure, draining, closed;
        int attempts, flushes;
        int queueError;
        CountDownLatch entered, resume;

        @Override
        public synchronized int queue(ByteBuffer data, long pts, boolean decodeOnly) {
            attempts++;
            if (queueError != NativeLcevcVvdec.RESULT_OK) {
                return queueError;
            }
            if (backpressure) {
                return NativeLcevcVvdec.ERROR_AGAIN;
            }
            accepted.add(pts);
            return NativeLcevcVvdec.RESULT_OK;
        }

        @Override
        public long dequeue(int[] info, long[] pts) {
            if (entered != null) {
                entered.countDown();
                try {
                    if (!resume.await(5, TimeUnit.SECONDS)) {
                        throw new AssertionError("test blocked");
                    }
                } catch (InterruptedException e) {
                    throw new AssertionError(e);
                }
                entered = null;
            }
            synchronized (this) {
                info[0] = NativeLcevcVvdec.DEQUEUE_PENDING;
                info[1] = 0;
                info[2] = 0;
                Frame frame = frames.poll();
                if (frame != null) {
                    info[0] = 640;
                    info[1] = 360;
                    info[2] = frame.decodeOnly ? 1 : 0;
                    pts[0] = frame.pts;
                    return frame.pts + 100;
                }
                if (draining) {
                    info[0] = NativeLcevcVvdec.DEQUEUE_END_OF_STREAM;
                }
                return 0;
            }
        }

        @Override
        public synchronized void eof() {
            draining = true;
        }

        @Override
        public synchronized void flush() {
            flushes++;
            frames.clear();
            draining = false;
        }

        @Override
        public synchronized void releasePicture(long picture) {
            assertFalse("Picture released twice", released.contains(picture));
            released.add(picture);
        }

        @Override
        public synchronized void close() {
            closed = true;
        }

        @Override
        public void setSurface(Surface surface) {}

        @Override
        public int render(long picture) {
            return NativeLcevcVvdec.RESULT_OK;
        }
    }

    private interface Condition {
        boolean get() throws Exception;
    }

    private static void await(Condition condition) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!condition.get()) {
            if (System.nanoTime() > deadline) {
                fail("Timed out");
            }
            Thread.sleep(1);
        }
    }

    private static void queue(LcevcVvdecDecoder decoder, long pts, boolean eos) throws Exception {
        DecoderInputBuffer[] input = new DecoderInputBuffer[1];
        await(() -> (input[0] = decoder.dequeueInputBuffer()) != null);
        input[0].timeUs = pts;
        if (eos) {
            input[0].addFlag(C.BUFFER_FLAG_END_OF_STREAM);
        } else {
            input[0].data = ByteBuffer.allocateDirect(1);
            input[0].data.put((byte) 1).flip();
        }
        decoder.queueInputBuffer(input[0]);
    }

    private static LcevcVvdecOutputBuffer output(LcevcVvdecDecoder decoder) throws Exception {
        LcevcVvdecOutputBuffer[] result = new LcevcVvdecOutputBuffer[1];
        await(() -> (result[0] = decoder.dequeueOutputBuffer()) != null);
        return result[0];
    }

    @Test
    public void backpressureRetainsInputAndConsumesExactlyOnce() throws Exception {
        Fake fake = new Fake();
        fake.backpressure = true;
        LcevcVvdecDecoder decoder = new LcevcVvdecDecoder(fake);
        try {
            queue(decoder, 10, false);
            await(
                    () -> {
                        synchronized (fake) {
                            return fake.attempts >= 2;
                        }
                    });
            synchronized (fake) {
                assertTrue(fake.accepted.isEmpty());
                fake.backpressure = false;
            }
            queue(decoder, 20, false);
            await(
                    () -> {
                        synchronized (fake) {
                            return fake.accepted.size() == 2;
                        }
                    });
            synchronized (fake) {
                assertEquals(List.of(10L, 20L), fake.accepted);
            }
        } finally {
            decoder.release();
        }
    }

    @Test
    public void delayedOutputArrivesWithoutAnotherInputAndNoEmptyOutputIsEmitted()
            throws Exception {
        Fake fake = new Fake();
        LcevcVvdecDecoder decoder = new LcevcVvdecDecoder(fake);
        try {
            queue(decoder, 10, false);
            await(
                    () -> {
                        synchronized (fake) {
                            return fake.accepted.size() == 1;
                        }
                    });
            assertNull(decoder.dequeueOutputBuffer());
            synchronized (fake) {
                fake.frames.add(new Frame(10, false));
            }
            LcevcVvdecOutputBuffer out = output(decoder);
            assertEquals(10, out.timeUs);
            assertFalse(out.isEndOfStream());
            out.release();
        } finally {
            decoder.release();
        }
    }

    @Test
    public void eosFollowsAllReorderedFramesEvenWhenOutputPoolFills() throws Exception {
        Fake fake = new Fake();
        LcevcVvdecDecoder decoder = new LcevcVvdecDecoder(fake);
        try {
            synchronized (fake) {
                for (int i = 1; i <= 7; i++) {
                    fake.frames.add(new Frame(i, false));
                }
            }
            queue(decoder, 0, true);
            for (int i = 1; i <= 7; i++) {
                LcevcVvdecOutputBuffer out = output(decoder);
                assertFalse(out.isEndOfStream());
                assertEquals(i, out.timeUs);
                out.release();
            }
            LcevcVvdecOutputBuffer eos = output(decoder);
            assertTrue(eos.isEndOfStream());
            assertEquals(0, eos.nativePic);
            eos.release();
            assertNull(decoder.dequeueOutputBuffer());
        } finally {
            decoder.release();
        }
        assertEquals(7, fake.released.size());
    }

    @Test
    public void decodeOnlyFollowsReturnedFrameNotCurrentInput() throws Exception {
        Fake fake = new Fake();
        LcevcVvdecDecoder decoder = new LcevcVvdecDecoder(fake);
        try {
            synchronized (fake) {
                fake.frames.add(new Frame(10, true));
                fake.frames.add(new Frame(20, false));
            }
            queue(decoder, 30, false);
            LcevcVvdecOutputBuffer out = output(decoder);
            assertEquals(20, out.timeUs);
            assertEquals(1, out.skippedOutputBufferCount);
            out.release();
            assertEquals(List.of(110L, 120L), fake.released);
        } finally {
            decoder.release();
        }
    }

    @Test
    public void flushDiscardsInFlightResultAndAllowsNewStream() throws Exception {
        Fake fake = new Fake();
        fake.entered = new CountDownLatch(1);
        fake.resume = new CountDownLatch(1);
        CountDownLatch entered = fake.entered;
        LcevcVvdecDecoder decoder = new LcevcVvdecDecoder(fake);
        try {
            synchronized (fake) {
                fake.frames.add(new Frame(10, false));
            }
            queue(decoder, 10, false);
            assertTrue(entered.await(5, TimeUnit.SECONDS));
            decoder.flush();
            fake.resume.countDown();
            await(
                    () -> {
                        synchronized (fake) {
                            return fake.flushes == 1;
                        }
                    });
            synchronized (fake) {
                assertEquals(List.of(110L), fake.released);
                fake.frames.add(new Frame(20, false));
            }
            queue(decoder, 20, false);
            LcevcVvdecOutputBuffer out = output(decoder);
            assertEquals(20, out.timeUs);
            out.release();
        } finally {
            fake.resume.countDown();
            decoder.release();
        }
    }

    @Test
    public void nativeFailureIsPropagatedAndFlushRecovers() throws Exception {
        Fake fake = new Fake();
        fake.queueError = NativeLcevcVvdec.ERROR_IO;
        LcevcVvdecDecoder decoder = new LcevcVvdecDecoder(fake);
        try {
            queue(decoder, 10, false);
            await(
                    () -> {
                        try {
                            decoder.dequeueOutputBuffer();
                            return false;
                        } catch (LcevcVvdecDecoderException e) {
                            assertTrue(
                                    e.getMessage()
                                            .contains(Integer.toString(NativeLcevcVvdec.ERROR_IO)));
                            return true;
                        }
                    });
            synchronized (fake) {
                fake.queueError = NativeLcevcVvdec.RESULT_OK;
            }
            decoder.flush();
            queue(decoder, 20, false);
            await(
                    () -> {
                        synchronized (fake) {
                            return fake.accepted.equals(List.of(20L));
                        }
                    });
        } finally {
            decoder.release();
        }
        assertTrue(fake.closed);
    }
}
