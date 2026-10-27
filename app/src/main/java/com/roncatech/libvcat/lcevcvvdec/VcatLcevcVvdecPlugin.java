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

import android.content.Context;
import android.os.Handler;

import com.google.android.exoplayer2.Renderer;
import com.google.android.exoplayer2.decoder.DecoderException;
import com.google.android.exoplayer2.util.Util;
import com.google.android.exoplayer2.video.VideoRendererEventListener;
import com.roncatech.vcat.decoder_plugin_api.NonStdDecoderStsdParser;
import com.roncatech.vcat.decoder_plugin_api.VcatDecoderPlugin;
import com.roncatech.vcat.decoder_plugin_api.VideoConfiguration;

import java.util.Collections;
import java.util.List;

public abstract class VcatLcevcVvdecPlugin implements VcatDecoderPlugin, NonStdDecoderStsdParser {
    public static final String mimeType = "video/vvc";

    @Override
    public int sampleEntry4ccCode() {
        return Util.getIntegerCodeForString("vvc1");
    }

    @Override
    public int codecConfiguration4ccCode() {
        return Util.getIntegerCodeForString("vvcC");
    }

    @Override
    public String mimeType() {
        return mimeType;
    }

    @Override
    public VideoConfiguration parseStsd(byte[] data) {
        return VvcVideoCfgParser.parseStsd(data);
    }

    @Override
    public String getDisplayName() {
        return getId() + "-" + getVersion();
    }

    @Override
    public String getVersion() {
        return NativeLcevcVvdec.vvdecGetVersion();
    }

    @Override
    public String getMimeType() {
        return mimeType;
    }

    @Override
    public List<String> getSupportedProfiles() {
        return Collections.singletonList("main");
    }

    @Override
    public Renderer createVideoRenderer(
            Context context,
            long allowedJoiningTimeMs,
            Handler eventHandler,
            VideoRendererEventListener eventListener,
            int threads)
            throws DecoderException {
        return createVideoRenderer(
                context,
                allowedJoiningTimeMs,
                eventHandler,
                eventListener,
                threads,
                /* lcevcThreads= */ 2);
    }

    public Renderer createVideoRenderer(
            Context context,
            long allowedJoiningTimeMs,
            Handler eventHandler,
            VideoRendererEventListener eventListener,
            int threads,
            int lcevcThreads)
            throws DecoderException {
        try {
            LcevcVvdecLibrary.load();
        } catch (UnsatisfiedLinkError e) {
            throw new DecoderException("Unable to load LCEVC-VVDEC", e);
        }
        return new LcevcVvdecVideoRenderer(
                allowedJoiningTimeMs,
                eventHandler,
                eventListener,
                threads,
                lcevcThreads,
                useVulkanPipeline());
    }

    protected boolean useVulkanPipeline() {
        return false;
    }
}
