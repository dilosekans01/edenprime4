// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace Core::Kledd {

/// eden-kledd diagnostic (Windows only, no-op elsewhere): every 2 seconds, logs how much CPU time
/// each emulated core thread and the busiest other Eden threads used, and on which kind of host
/// core (performance or efficiency) the emulated core threads were seen running.
void StartCpuMonitor();
void StopCpuMonitor();

/// Called on an emulated core's host thread from time to time (every interrupt): records the kind
/// of host core it is on right now.
void SampleCoreThread(std::size_t core);

} // namespace Core::Kledd
