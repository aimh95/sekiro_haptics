#include "sekiro_haptics/signals/VectorSignalSource.hpp"

namespace sekiro_haptics {

VectorSignalSource::VectorSignalSource(std::vector<GameSignal> signals) : signals_(std::move(signals)) {}

SignalSourceResult VectorSignalSource::Next(GameSignal& outSignal, std::string* outError) {
    (void)outError;
    if (index_ >= signals_.size()) {
        return SignalSourceResult::EndOfSource;
    }
    outSignal = signals_[index_++];
    return SignalSourceResult::Success;
}

bool VectorSignalSource::Reset() {
    index_ = 0;
    return true;
}

} // namespace sekiro_haptics
