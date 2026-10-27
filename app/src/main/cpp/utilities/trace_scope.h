/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 V-Nova International Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
*/

// RAII wrapper for NDK atrace sections, visible in Perfetto captures.

#ifndef VCAT_TRACE_SCOPE_H
#define VCAT_TRACE_SCOPE_H

#include <android/trace.h>

class ScopedTrace
{
public:
    explicit ScopedTrace(const char* name)
    {
        ATrace_beginSection(name);
    }
    ~ScopedTrace()
    {
        ATrace_endSection();
    }
    ScopedTrace(const ScopedTrace&) = delete;
    ScopedTrace& operator=(const ScopedTrace&) = delete;
};

#define VCAT_CONCAT_INNER(a, b) a##b
#define VCAT_CONCAT(a, b) VCAT_CONCAT_INNER(a, b)
#define TRACE_SCOPE(name) ScopedTrace VCAT_CONCAT(_trace_scope_, __LINE__)(name)

#endif // VCAT_TRACE_SCOPE_H
