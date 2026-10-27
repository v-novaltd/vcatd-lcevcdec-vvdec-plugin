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

import com.google.android.exoplayer2.util.ParsableBitArray;
import com.roncatech.vcat.decoder_plugin_api.VideoConfiguration;

import java.util.ArrayList;
import java.util.List;

public final class VvcVideoCfgParser {

    private static final int SPS_NAL_UNIT_TYPE = 15;

    private VvcVideoCfgParser() {}

    public static VideoConfiguration parseStsd(byte[] data) {

        ParsableBitArray reader = new ParsableBitArray(data);

        // Box size and type.
        reader.readBits(32);
        reader.readBits(32);

        // full box
        reader.readBits(8);
        reader.readBits(24);
        reader.readBits(5);

        int lengthSizeMinusOne = reader.readBits(2);
        boolean ptlPresentFlag = reader.readBit();

        if (ptlPresentFlag) {
            reader.readBits(9);
            int numSubLayers = reader.readBits(3);
            reader.readBits(2);
            reader.readBits(2);
            reader.readBits(3);
            reader.readBits(5);

            // VvcPTLRecord
            reader.readBits(2);
            int numBytesConstraintInfo = reader.readBits(6);
            reader.readBits(7);
            reader.readBit();
            reader.readBits(8);
            reader.readBit();
            reader.readBit();
            reader.readBits(8 * numBytesConstraintInfo - 2);

            List<Boolean> ptlSubLayerLevelPresentFlag = new ArrayList<>();
            for (int i = numSubLayers - 2; i >= 0; i--) {
                ptlSubLayerLevelPresentFlag.add(reader.readBit());
            }

            for (int j = numSubLayers; j <= 8 && numSubLayers > 1; j++) {
                reader.readBits(1);
            }

            for (int i = numSubLayers - 2; i >= 0; i--) {
                if (ptlSubLayerLevelPresentFlag.get(i)) {
                    reader.readBits(8);
                }
            }

            int ptlNumSubProfiles = reader.readBits(8);
            for (int j = 0; j < ptlNumSubProfiles; j++) {
                reader.readBits(32);
            }

            // finished with VvcPTLRecord
            reader.readBits(16);
            reader.readBits(16);
            reader.readBits(16);
        }

        // Calculate per-array sizes for all VPS/SPS/PPS bitstreams.
        final int DCI_NUT = 13;
        final int OPI_NUT = 12;
        int numberOfArrays = reader.readBits(8);
        int csdStartPosition = reader.getBytePosition();
        int[] arraySizes = new int[numberOfArrays];

        for (int i = 0; i < numberOfArrays; i++) {
            reader.readBit(); // arrayCompleteness
            reader.readBits(2);
            int nalUnitType = reader.readBits(5);
            int numNals = reader.readBits(16);

            for (int j = 0; j < numNals; j++) {
                int nalUnitLength = reader.readBits(16);
                if (nalUnitType != DCI_NUT && nalUnitType != OPI_NUT) {
                    arraySizes[i] += 4 + nalUnitLength; // Start code + NAL unit.
                }
                reader.skipBytes(nalUnitLength);
            }
        }

        VideoConfiguration.Builder builder = new VideoConfiguration.Builder();
        builder.mimeType = VcatLcevcVvdecPlugin.mimeType;

        builder.nalUnitLengthFieldLength = lengthSizeMinusOne + 1;

        // Second pass: copy NALs into per-array buffers with start codes, and parse SPS if present
        reader.setPosition(csdStartPosition * 8);
        List<byte[]> initDataList = new ArrayList<>();

        for (int i = 0; i < numberOfArrays; i++) {
            reader.readBit();
            reader.readBits(2);
            int nalUnitType = reader.readBits(5);
            int numNals = reader.readBits(16);

            if (nalUnitType == DCI_NUT || nalUnitType == OPI_NUT) {
                for (int j = 0; j < numNals; j++) {
                    int nalUnitLength = reader.readBits(16);
                    reader.skipBytes(nalUnitLength);
                }
                continue;
            }

            byte[] buffer = new byte[arraySizes[i]];
            int bufferPosition = 0;

            for (int j = 0; j < numNals; j++) {
                int nalUnitLength = reader.readBits(16);

                System.arraycopy(
                        VvcNalUnitUtil.NAL_START_CODE,
                        0,
                        buffer,
                        bufferPosition,
                        VvcNalUnitUtil.NAL_START_CODE.length);
                bufferPosition += VvcNalUnitUtil.NAL_START_CODE.length;
                reader.readBytes(buffer, bufferPosition, nalUnitLength);

                if (nalUnitType == SPS_NAL_UNIT_TYPE && j == 0) {
                    VvcNalUnitUtil.H266SpsData spsData =
                            VvcNalUnitUtil.parseH266SpsNalUnit(
                                    buffer, bufferPosition, bufferPosition + nalUnitLength);
                    builder.width = spsData.width;
                    builder.height = spsData.height;
                }
                bufferPosition += nalUnitLength;
            }

            if (arraySizes[i] > 0) {
                initDataList.add(buffer);
            }
        }

        builder.initializationData = initDataList;

        return builder.build();
    }
}
