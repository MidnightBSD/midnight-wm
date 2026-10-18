// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

namespace midnight {
// Reuse a reachable bus or re-exec this complete argument list through
// dbus-run-session. The wrapper owns the newly created bus and recursion guard.
void ensure_session_bus(int argc, char** argv);
}
