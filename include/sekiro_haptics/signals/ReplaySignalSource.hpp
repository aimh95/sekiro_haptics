#pragma once

#include "sekiro_haptics/replay/TraceJsonl.hpp"
#include "sekiro_haptics/signals/IGameSignalSource.hpp"

#include <string>

namespace sekiro_haptics {

/// IGameSignalSource backed by a recorded JSONL trace file
/// (sekiro_haptics::trace::TraceReader). This is the source the replay CLI
/// and pipeline tests use -- it never touches a real game or hardware.
class ReplaySignalSource final : public IGameSignalSource {
public:
    explicit ReplaySignalSource(const std::string& tracePath);

    SignalSourceResult Next(GameSignal& outSignal, std::string* outError = nullptr) override;
    bool Reset() override;

    /// Whether the underlying trace file was successfully opened.
    bool IsOpen() const;

private:
    trace::TraceReader reader_;
};

} // namespace sekiro_haptics
