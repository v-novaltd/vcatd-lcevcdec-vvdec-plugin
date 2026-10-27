/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 V-Nova International Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of the VCAT LCEVCdec plugin.
 */

package com.roncatech.libvcat.lcevcvvdec;

/** CPU variant of the LCEVC + vvdec decoder. */
public final class VcatLcevcVvdecCpuPlugin extends VcatLcevcVvdecPlugin {
    @Override
    public String getId() {
        return "vcat.lcevcdec-vvdec-cpu";
    }
}
