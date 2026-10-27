/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.roncatech.libvcat.lcevcvvdec;

import com.google.android.exoplayer2.C;
import com.google.android.exoplayer2.Format;
import com.google.android.exoplayer2.util.ParsableNalUnitBitArray;

/** VVC SPS parsing used by the vvcC configuration reader. */
final class VvcNalUnitUtil {
    static final byte[] NAL_START_CODE = {0, 0, 0, 1};

    public static final class H266SpsData {

        public final int seqParameterSetId;
        public final int videoParameterSetId;
        public final int maxSubLayersMinus1;
        public final int chromaFormatIdc;
        public final int bitDepthLumaMinus8;
        public final int bitDepthChromaMinus8;
        public final int log2CtuSizeMinus5;
        public final int log2MinLumaCodingBlockSizeMinus2;
        public final int log2DiffMinQtMinCbIntraLuma;
        public final int maxMttHierarchyDepthIntraLuma;
        public final int picWidthMaxInLumaSamples;
        public final int picHeightMaxInLumaSamples;
        public final int confWinLeftOffset;
        public final int confWinRightOffset;
        public final int confWinTopOffset;
        public final int confWinBottomOffset;
        public final boolean conformanceWindowFlag;
        public final boolean gdrEnabledFlag;
        public final boolean refPicResamplingEnabledFlag;
        public final boolean resChangeInClvsAllowedFlag;
        // Derived from bitDepthLumaMinus8.
        public final int bitDepth;

        // Dimensions after applying the conformance window.
        public final int width;
        public final int height;
        public final float pixelWidthHeightRatio;
        public final @C.ColorSpace int colorSpace;
        public final @C.ColorRange int colorRange;
        public final @C.ColorTransfer int colorTransfer;

        public H266SpsData(
                int seqParameterSetId,
                int videoParameterSetId,
                int maxSubLayersMinus1,
                int chromaFormatIdc,
                int bitDepthLumaMinus8,
                int bitDepthChromaMinus8,
                int log2CtuSizeMinus5,
                int log2MinLumaCodingBlockSizeMinus2,
                int log2DiffMinQtMinCbIntraLuma,
                int maxMttHierarchyDepthIntraLuma,
                int picWidthMaxInLumaSamples,
                int picHeightMaxInLumaSamples,
                boolean conformanceWindowFlag,
                int confWinLeftOffset,
                int confWinRightOffset,
                int confWinTopOffset,
                int confWinBottomOffset,
                boolean gdrEnabledFlag,
                boolean refPicResamplingEnabledFlag,
                boolean resChangeInClvsAllowedFlag,
                float pixelWidthHeightRatio,
                @C.ColorSpace int colorSpace,
                @C.ColorRange int colorRange,
                @C.ColorTransfer int colorTransfer) {

            this.seqParameterSetId = seqParameterSetId;
            this.videoParameterSetId = videoParameterSetId;
            this.maxSubLayersMinus1 = maxSubLayersMinus1;

            this.chromaFormatIdc = chromaFormatIdc;
            this.bitDepthLumaMinus8 = bitDepthLumaMinus8;
            this.bitDepthChromaMinus8 = bitDepthChromaMinus8;
            this.log2CtuSizeMinus5 = log2CtuSizeMinus5;
            this.log2MinLumaCodingBlockSizeMinus2 = log2MinLumaCodingBlockSizeMinus2;
            this.log2DiffMinQtMinCbIntraLuma = log2DiffMinQtMinCbIntraLuma;
            this.maxMttHierarchyDepthIntraLuma = maxMttHierarchyDepthIntraLuma;

            this.picWidthMaxInLumaSamples = picWidthMaxInLumaSamples;
            this.picHeightMaxInLumaSamples = picHeightMaxInLumaSamples;
            this.conformanceWindowFlag = conformanceWindowFlag;
            this.confWinLeftOffset = confWinLeftOffset;
            this.confWinRightOffset = confWinRightOffset;
            this.confWinTopOffset = confWinTopOffset;
            this.confWinBottomOffset = confWinBottomOffset;

            this.gdrEnabledFlag = gdrEnabledFlag;
            this.refPicResamplingEnabledFlag = refPicResamplingEnabledFlag;
            this.resChangeInClvsAllowedFlag = resChangeInClvsAllowedFlag;

            this.bitDepth = 8 + bitDepthLumaMinus8;
            this.width =
                    picWidthMaxInLumaSamples
                            - (conformanceWindowFlag
                                    ? (confWinLeftOffset + confWinRightOffset)
                                    : 0);
            this.height =
                    picHeightMaxInLumaSamples
                            - (conformanceWindowFlag
                                    ? (confWinTopOffset + confWinBottomOffset)
                                    : 0);
            this.pixelWidthHeightRatio = pixelWidthHeightRatio;

            this.colorSpace = colorSpace;
            this.colorRange = colorRange;
            this.colorTransfer = colorTransfer;
        }
    }

    public static H266SpsData parseH266SpsNalUnit(byte[] nalData, int nalOffset, int nalLimit) {
        return parseH266SpsNalUnitPayload(nalData, nalOffset + 2, nalLimit);
    }

    public static H266SpsData parseH266SpsNalUnitPayload(
            byte[] nalData, int nalOffset, int nalLimit) {

        ParsableNalUnitBitArray data = new ParsableNalUnitBitArray(nalData, nalOffset, nalLimit);

        // --- Sequence Parameter Set core syntax ---
        int spsSeqParameterSetId = data.readBits(4);
        int spsVideoParameterSetId = data.readBits(4);
        int spsMaxSubLayersMinus1 = data.readBits(3);
        int spsChromaFormatIdc = data.readBits(2);
        int spsLog2CtuSizeMinus5 = data.readBits(2);

        boolean spsPtlDpbHrdParamsPresentFlag = data.readBit();
        if (spsPtlDpbHrdParamsPresentFlag) {
            // profile_tier_level(1, spsMaxSubLayersMinus1)
            data.skipBits(8); // general_profile_idc (7 bits) + general_tier_flag (1 bit)
            data.skipBits(8); // general_level_idc u(8)
            data.skipBit(); // ptl_frame_only_constraint_flag u(1)
            data.skipBit(); // ptl_multilayer_enabled_flag u(1)
            skipVvcGeneralConstraintsInfo(data);

            // ptl_sublayer_level_present_flag[i]
            boolean[] subLayerLevelPresentFlags = new boolean[spsMaxSubLayersMinus1];
            for (int i = spsMaxSubLayersMinus1 - 1; i >= 0; i--) {
                subLayerLevelPresentFlags[i] = data.readBit();
            }

            // while (!byte_aligned()) ptl_reserved_zero_bit u(1)
            // Safely skip up to 7 bits to reach next byte boundary
            for (int i = 0; i < 7; i++) {
                if ((i & 7) == 0) {
                    break;
                }
                data.skipBit();
            }

            // for each sublayer, if ptl_sublayer_level_present_flag[i]
            for (int i = spsMaxSubLayersMinus1 - 1; i >= 0; i--) {
                if (subLayerLevelPresentFlags[i]) {
                    // sublayer_level_idc u(8)
                    data.skipBits(8);
                }
            }
            // ptl_num_sub_profiles u(8)
            int numSubProfiles = data.readBits(8);
            for (int i = 0; i < numSubProfiles; i++) {
                // general_sub_profile_idc[i] u(32)
                data.skipBits(32);
            }
        }

        boolean spsGdrEnabledFlag = data.readBit();
        boolean spsRefPicResamplingEnabledFlag = data.readBit();
        boolean spsResChangeInClvsAllowedFlag = false;
        if (spsRefPicResamplingEnabledFlag) {
            spsResChangeInClvsAllowedFlag = data.readBit();
        }

        int spsPicWidthMaxInLumaSamples = data.readUnsignedExpGolombCodedInt();
        int spsPicHeightMaxInLumaSamples = data.readUnsignedExpGolombCodedInt();

        int confWinLeftOffset = 0;
        int confWinRightOffset = 0;
        int confWinTopOffset = 0;
        int confWinBottomOffset = 0;
        boolean spsConformanceWindowFlag = data.readBit();
        if (spsConformanceWindowFlag) {
            confWinLeftOffset = data.readUnsignedExpGolombCodedInt();
            confWinRightOffset = data.readUnsignedExpGolombCodedInt();
            confWinTopOffset = data.readUnsignedExpGolombCodedInt();
            confWinBottomOffset = data.readUnsignedExpGolombCodedInt();
        }

        // Skip subpicture info if present
        boolean spsSubpicInfoPresentFlag = data.readBit();
        if (spsSubpicInfoPresentFlag) {
            int numSubpicsMinus1 = data.readUnsignedExpGolombCodedInt();
            if (numSubpicsMinus1 > 0) {
                data.skipBits(2); // sps_independent_subpics_flag, sps_subpic_same_size_flag
            }
            // skip rest of subpicture structure per spec
            data.skipBits(22);
        }

        int spsBitdepthMinus8 = data.readUnsignedExpGolombCodedInt();

        // skip entropy coding flags, poc info, etc.
        data.skipBits(1); // sps_entropy_coding_sync_enabled_flag
        data.skipBits(1); // sps_entry_point_offsets_present_flag
        int spsLog2MaxPicOrderCountLsbMinus4 = data.readBits(4);
        boolean spsPocMsbCycleFlag = data.readBit();
        if (spsPocMsbCycleFlag) {
            data.readUnsignedExpGolombCodedInt(); // sps_poc_msb_cycle_len_minus1
        }

        // skip extra PH/SH bytes flags
        int spsNumExtraPhBytes = data.readBits(2);
        for (int i = 0; i < spsNumExtraPhBytes * 8; i++) {
            data.skipBit();
        }
        int spsNumExtraShBytes = data.readBits(2);
        for (int i = 0; i < spsNumExtraShBytes * 8; i++) {
            data.skipBit();
        }
        // skip dpb_parameters etc.
        boolean spsSublayerDpbParamsFlag = false;
        if (spsPtlDpbHrdParamsPresentFlag) {
            if (spsMaxSubLayersMinus1 > 0) {
                data.skipBit();
            }
            int startLayer = spsSublayerDpbParamsFlag ? 0 : spsMaxSubLayersMinus1;
            for (int i = startLayer; i <= spsMaxSubLayersMinus1; i++) {
                data.readUnsignedExpGolombCodedInt(); // dpb_max_dec_pic_buffering_minus1[i]
                data.readUnsignedExpGolombCodedInt(); // dpb_max_num_reorder_pics[i]
                data.readUnsignedExpGolombCodedInt(); // dpb_max_latency_increase_plus1[i]
            }
        }

        int spsLog2MinLumaCbSizeMinus2 = data.readUnsignedExpGolombCodedInt();

        // Skip a lot of intra/inter partition flags
        data.skipBits(1); // sps_partition_constraints_override_enabled_flag
        data.readUnsignedExpGolombCodedInt(); // sps_log2_diff_min_qt_min_cb_intra_slice_luma
        if (data.readUnsignedExpGolombCodedInt()
                != 0) { // sps_max_mtt_hierarchy_depth_intra_slice_luma
            data.readUnsignedExpGolombCodedInt(); // sps_log2_diff_max_bt_min_qt_intra_slice_luma
            data.readUnsignedExpGolombCodedInt(); // sps_log2_diff_max_tt_min_qt_intra_slice_luma
        }
        boolean spsQtbttDualTreeIntraFlag = false;
        if (spsChromaFormatIdc != 0) {
            spsQtbttDualTreeIntraFlag = data.readBit();
        }
        if (spsQtbttDualTreeIntraFlag) {
            // sps_log2_diff_min_qt_min_cb_intra_slice_chroma
            data.readUnsignedExpGolombCodedInt();
            // sps_max_mtt_hierarchy_depth_intra_slice_chroma
            int maxMttHierarchyDepthIntraSliceChroma = data.readUnsignedExpGolombCodedInt();
            if (maxMttHierarchyDepthIntraSliceChroma != 0) {
                // sps_log2_diff_max_bt_min_qt_intra_slice_chroma
                data.readUnsignedExpGolombCodedInt();
                // sps_log2_diff_max_tt_min_qt_intra_slice_chroma
                data.readUnsignedExpGolombCodedInt();
            }
        }

        // sps_log2_diff_min_qt_min_cb_inter_slice
        data.readUnsignedExpGolombCodedInt();
        // sps_max_mtt_hierarchy_depth_inter_slice
        int maxMttHierarchyDepthInterSlice = data.readUnsignedExpGolombCodedInt();
        if (maxMttHierarchyDepthInterSlice != 0) {
            // sps_log2_diff_max_bt_min_qt_inter_slice
            data.readUnsignedExpGolombCodedInt();
            // sps_log2_diff_max_tt_min_qt_inter_slice
            data.readUnsignedExpGolombCodedInt();
        }

        // read bit depth and chroma again (redundant but safe)
        int bitDepthLumaMinus8 = spsBitdepthMinus8;
        int bitDepthChromaMinus8 = spsBitdepthMinus8;

        // compute dimensions considering conformance window and subsampling
        int subWidthC = (spsChromaFormatIdc == 1 || spsChromaFormatIdc == 2) ? 2 : 1;
        int subHeightC = (spsChromaFormatIdc == 1) ? 2 : 1;
        int frameWidth = spsPicWidthMaxInLumaSamples;
        int frameHeight = spsPicHeightMaxInLumaSamples;
        if (spsConformanceWindowFlag) {
            frameWidth -= subWidthC * (confWinLeftOffset + confWinRightOffset);
            frameHeight -= subHeightC * (confWinTopOffset + confWinBottomOffset);
        }

        float pixelWidthHeightRatio = 1f;
        @C.ColorSpace int colorSpace = Format.NO_VALUE;
        @C.ColorRange int colorRange = Format.NO_VALUE;
        @C.ColorTransfer int colorTransfer = Format.NO_VALUE;

        return new H266SpsData(
                spsSeqParameterSetId,
                spsVideoParameterSetId,
                spsMaxSubLayersMinus1,
                spsChromaFormatIdc,
                bitDepthLumaMinus8,
                bitDepthChromaMinus8,
                spsLog2CtuSizeMinus5,
                spsLog2MinLumaCbSizeMinus2,
                /* log2DiffMinQtMinCbIntraLuma */ 0,
                /* maxMttHierarchyDepthIntraLuma */ 0,
                frameWidth,
                frameHeight,
                spsConformanceWindowFlag,
                confWinLeftOffset,
                confWinRightOffset,
                confWinTopOffset,
                confWinBottomOffset,
                spsGdrEnabledFlag,
                spsRefPicResamplingEnabledFlag,
                spsResChangeInClvsAllowedFlag,
                pixelWidthHeightRatio,
                colorSpace,
                colorRange,
                colorTransfer);
    }

    private static void skipVvcGeneralConstraintsInfo(ParsableNalUnitBitArray data) {
        boolean gciPresent = data.readBit();
        int bitsRead = 1; // we already read 1 bit

        if (!gciPresent) {
            // manually align to next byte boundary if needed
            int alignBits = bitsRead % 8 == 0 ? 0 : (8 - (bitsRead % 8));
            data.skipBits(alignBits);
            return;
        }

        // --- general ---
        data.skipBits(3);
        bitsRead += 3;

        // --- picture format ---
        data.skipBits(6);
        bitsRead += 6;

        // --- NAL unit type related ---
        data.skipBits(10);
        bitsRead += 10;

        // --- tile/slice/subpic ---
        data.skipBits(6);
        bitsRead += 6;

        // --- CTU/block partitioning ---
        data.skipBits(5);
        bitsRead += 5;

        // --- intra ---
        data.skipBits(6);
        bitsRead += 6;

        // --- inter ---
        data.skipBits(16);
        bitsRead += 16;

        // --- transform, quantization, residual ---
        data.skipBits(13);
        bitsRead += 13;

        // --- loop filter ---
        data.skipBits(6);
        bitsRead += 6;

        // --- gci_num_additional_bits ---
        int gciNumAdditionalBits = data.readBits(8);
        bitsRead += 8;

        int numAdditionalBitsUsed = 0;
        if (gciNumAdditionalBits > 5) {
            data.skipBits(6);
            bitsRead += 6;
            numAdditionalBitsUsed = 6;
        }

        int remaining = gciNumAdditionalBits - numAdditionalBitsUsed;
        if (remaining > 0) {
            data.skipBits(remaining);
            bitsRead += remaining;
        }

        // --- manual byte alignment ---
        int alignBits = bitsRead % 8 == 0 ? 0 : (8 - (bitsRead % 8));
        if (alignBits > 0) {
            data.skipBits(alignBits);
        }
    }

    private VvcNalUnitUtil() {}
}
