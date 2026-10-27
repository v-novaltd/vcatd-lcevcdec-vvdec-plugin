package com.roncatech.libvcat.lcevcvvdec;

import static org.junit.Assert.*;

import com.roncatech.vcat.decoder_plugin_api.VideoConfiguration;

import org.junit.Test;

public class VvcVideoCfgParserTest {
    private static final byte[] TEST_VVC_C =
            new byte[] {
                (byte) 0x00, (byte) 0x00, (byte) 0x00, (byte) 0xE1, (byte) 0x76, (byte) 0x76,
                        (byte) 0x63, (byte) 0x43,
                (byte) 0x00, (byte) 0x00, (byte) 0x00, (byte) 0x00, (byte) 0xFF, (byte) 0x00,
                        (byte) 0x65, (byte) 0x5F,
                (byte) 0x01, (byte) 0x02, (byte) 0x40, (byte) 0x80, (byte) 0x00, (byte) 0x00,
                        (byte) 0x07, (byte) 0x80,
                (byte) 0x04, (byte) 0x38, (byte) 0x00, (byte) 0x00, (byte) 0x02, (byte) 0x8F,
                        (byte) 0x00, (byte) 0x01,
                (byte) 0x00, (byte) 0xAC, (byte) 0x00, (byte) 0x79, (byte) 0x00, (byte) 0xAD,
                        (byte) 0x02, (byte) 0x40,
                (byte) 0x80, (byte) 0x00, (byte) 0x00, (byte) 0x80, (byte) 0x0F, (byte) 0x02,
                        (byte) 0x00, (byte) 0x43,
                (byte) 0x91, (byte) 0xA8, (byte) 0x01, (byte) 0xCD, (byte) 0xD3, (byte) 0x6E,
                        (byte) 0x99, (byte) 0x1A,
                (byte) 0x45, (byte) 0x26, (byte) 0xF3, (byte) 0x84, (byte) 0xD9, (byte) 0x58,
                        (byte) 0xC1, (byte) 0x02,
                (byte) 0x09, (byte) 0xC0, (byte) 0x0C, (byte) 0xA0, (byte) 0xC5, (byte) 0x84,
                        (byte) 0x21, (byte) 0x83,
                (byte) 0x11, (byte) 0x08, (byte) 0x19, (byte) 0x22, (byte) 0x0D, (byte) 0x48,
                        (byte) 0x7A, (byte) 0x3D,
                (byte) 0x5A, (byte) 0x92, (byte) 0xF2, (byte) 0x49, (byte) 0xA9, (byte) 0x2C,
                        (byte) 0x91, (byte) 0x16,
                (byte) 0xA2, (byte) 0x2F, (byte) 0x11, (byte) 0x26, (byte) 0xA2, (byte) 0x24,
                        (byte) 0x52, (byte) 0x44,
                (byte) 0x49, (byte) 0x92, (byte) 0x22, (byte) 0x4D, (byte) 0x46, (byte) 0x59,
                        (byte) 0x21, (byte) 0x0B,
                (byte) 0x50, (byte) 0x85, (byte) 0xE1, (byte) 0x0B, (byte) 0xD5, (byte) 0xA9,
                        (byte) 0x2F, (byte) 0x25,
                (byte) 0xEA, (byte) 0x19, (byte) 0x41, (byte) 0x8B, (byte) 0x10, (byte) 0x0C,
                        (byte) 0x58, (byte) 0x80,
                (byte) 0x81, (byte) 0x08, (byte) 0x10, (byte) 0x10, (byte) 0x2A, (byte) 0x10,
                        (byte) 0x20, (byte) 0x20,
                (byte) 0x69, (byte) 0x08, (byte) 0x10, (byte) 0x10, (byte) 0x2C, (byte) 0x40,
                        (byte) 0x80, (byte) 0x81,
                (byte) 0x10, (byte) 0x40, (byte) 0x40, (byte) 0xB2, (byte) 0x08, (byte) 0x08,
                        (byte) 0x12, (byte) 0x10,
                (byte) 0x10, (byte) 0x64, (byte) 0x08, (byte) 0x11, (byte) 0x08, (byte) 0x10,
                        (byte) 0x2C, (byte) 0x84,
                (byte) 0x08, (byte) 0x12, (byte) 0x20, (byte) 0x41, (byte) 0xA0, (byte) 0x81,
                        (byte) 0x24, (byte) 0x10,
                (byte) 0x70, (byte) 0x83, (byte) 0x20, (byte) 0x45, (byte) 0xA1, (byte) 0x04,
                        (byte) 0x90, (byte) 0x87,
                (byte) 0x10, (byte) 0xD0, (byte) 0x97, (byte) 0x23, (byte) 0x95, (byte) 0xFF,
                        (byte) 0xA5, (byte) 0xD9,
                (byte) 0x7F, (byte) 0x8F, (byte) 0x24, (byte) 0x00, (byte) 0x00, (byte) 0x03,
                        (byte) 0x00, (byte) 0x04,
                (byte) 0x00, (byte) 0x00, (byte) 0x03, (byte) 0x00, (byte) 0x63, (byte) 0x89,
                        (byte) 0x2C, (byte) 0x00,
                (byte) 0x1E, (byte) 0x84, (byte) 0x80, (byte) 0x00, (byte) 0xB7, (byte) 0x1B,
                        (byte) 0x00, (byte) 0x03,
                (byte) 0xD0, (byte) 0x90, (byte) 0x00, (byte) 0x16, (byte) 0xE3, (byte) 0x61,
                        (byte) 0x90, (byte) 0x00,
                (byte) 0x01, (byte) 0x00, (byte) 0x0E, (byte) 0x00, (byte) 0x81, (byte) 0x00,
                        (byte) 0x00, (byte) 0x07,
                (byte) 0x81, (byte) 0x00, (byte) 0x21, (byte) 0xC8, (byte) 0x89, (byte) 0x07,
                        (byte) 0xEE, (byte) 0x08,
                (byte) 0x00
            };

    @Test
    public void parseVvcC() {

        VideoConfiguration cfg = VvcVideoCfgParser.parseStsd(TEST_VVC_C);

        assertEquals(2, cfg.initializationData.size());
    }
}
