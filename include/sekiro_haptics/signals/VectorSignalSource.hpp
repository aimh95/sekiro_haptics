#pragma once

#include "sekiro_haptics/signals/IGameSignalSource.hpp"

#include <cstddef>
#include <vector>

namespace sekiro_haptics {

/// IGameSignalSource backed by an in-memory vector, for tests that want to
/// hand a pipeline a specific sequence of signals without writing a JSONL
/// fixture file.
class VectorSignalSource final : public IGameSignalSource {
public:
    explicit VectorSignalSource(std::vector<GameSignal> signals);

    /// Never returns MalformedInput -- every element was already a valid
    /// GameSignal when constructed.
    SignalSourceResult Next(GameSignal& outSignal, std::string* outError = nullptr) override;

    /// Always succeeds.
    bool Reset() override;

private:
    std::vector<GameSignal> signals_;
    std::size_t index_ = 0;
};

} // namespace sekiro_haptics
