#include "sekiro_haptics/signals/ReplaySignalSource.hpp"

namespace sekiro_haptics {

ReplaySignalSource::ReplaySignalSource(const std::string& tracePath) : reader_(tracePath) {}

bool ReplaySignalSource::IsOpen() const {
    return reader_.IsOpen();
}

SignalSourceResult ReplaySignalSource::Next(GameSignal& outSignal, std::string* outError) {
    trace::TraceReadResult result = reader_.ReadNext(outSignal, outError);
    switch (result) {
        case trace::TraceReadResult::Success:
            return SignalSourceResult::Success;
        case trace::TraceReadResult::EndOfTrace:
            return SignalSourceResult::EndOfSource;
        case trace::TraceReadResult::MalformedJson:
        case trace::TraceReadResult::MissingTimestamp:
        case trace::TraceReadResult::MissingSignal:
        case trace::TraceReadResult::OutOfOrderTimestamp:
        case trace::TraceReadResult::IoError:
            return SignalSourceResult::MalformedInput;
    }
    return SignalSourceResult::MalformedInput;
}

bool ReplaySignalSource::Reset() {
    return reader_.Rewind();
}

} // namespace sekiro_haptics
