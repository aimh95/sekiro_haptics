#pragma once

// Windows USB-audio access to a DualSense controller's render endpoint, with
// the format actually verified rather than assumed.
//
// WHY THIS EXISTS SEPARATELY FROM WasapiSpeakerOutput
// ---------------------------------------------------
// WasapiSpeakerOutput mixes every cue into the front stereo pair and leaves
// any additional channels silent. That is correct for "play a sound", but it
// cannot answer the question this project actually needs answered: WHICH
// channel of this endpoint drives the built-in speaker and which (if any)
// drives the voice-coil haptics. Those channel indices are NOT hardcoded
// here -- see ProbeChannel(), which drives exactly one channel at a time so a
// human can identify it by ear/touch, and DualSenseChannelMap, which the
// caller fills in from that observation or from a config file.
//
// Everything here is explicit: the endpoint is selected by id (never a
// default-device fallback), GetMixFormat() reports what shared mode will
// use, and IsFormatSupported() is called and its result reported rather than
// silently converting to something that happens to work.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sekiro_haptics {

struct AudioEndpointInfo {
    std::wstring id;
    std::wstring name;
    /// True when the friendly name looks like a DualSense render endpoint.
    /// A hint for ordering the list, never a substitute for an explicit
    /// selection by the caller.
    bool looksLikeDualSense = false;
};

/// What the endpoint actually reports, as opposed to what we hoped for.
struct AudioFormatReport {
    bool queried = false;
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t validBitsPerSample = 0;
    std::uint32_t channelMask = 0;
    std::string subFormat;            // "PCM", "IEEE_FLOAT", or a raw GUID
    std::string formatTag;            // "EXTENSIBLE", "PCM", "IEEE_FLOAT", ...
    /// IsFormatSupported(SHARED, mixFormat) -- expected to be S_OK, recorded
    /// so a surprise shows up instead of being assumed.
    std::string sharedSupport;
    /// IsFormatSupported(EXCLUSIVE, mixFormat).
    std::string exclusiveSupport;
    /// Windows' own master level for this endpoint, and its mute flag.
    /// `endpointVolume` is the SCALAR, which is NOT a linear amplitude: 0.8
    /// scalar is not "80% of the sample values". `endpointVolumeDb` is the
    /// same setting expressed in dB, which is what actually predicts loudness.
    bool volumeKnown = false;
    float endpointVolume = 0.0f;
    float endpointVolumeDb = 0.0f;
    float endpointVolumeMinDb = 0.0f;
    float endpointVolumeMaxDb = 0.0f;
    bool endpointMuted = false;
    /// Per-channel endpoint levels, if the endpoint exposes them.
    std::vector<float> endpointChannelVolumes;
    /// This application's own session, which is separate from the device.
    bool sessionVolumeKnown = false;
    float sessionVolume = 0.0f;
    bool sessionMuted = false;
    /// This stream's per-channel levels (IChannelAudioVolume).
    std::vector<float> streamChannelVolumes;
    std::string error;
};

/// Result of an explicit volume write: the HRESULT and what the device
/// reported when read back, so "I set it" and "it took" stay separate.
struct VolumeSetResult {
    std::string hresult;
    bool readBackOk = false;
    float readBackScalar = 0.0f;
    float readBackDb = 0.0f;
    bool readBackMuted = false;
};

/// Which channel index of the render endpoint carries what. Nothing here has
/// a default that claims to know the DualSense layout: -1 means "not
/// identified yet", and the owning app refuses to send haptic PCM to an
/// unidentified channel.
struct DualSenseChannelMap {
    int speaker = -1;        // built-in speaker (or headphone left if that is all there is)
    int speakerRight = -1;   // optional second speaker/headphone channel
    int hapticLeft = -1;     // voice-coil actuator, if this endpoint exposes one
    int hapticRight = -1;
    bool HasSpeaker() const { return speaker >= 0; }
    bool HasHaptic() const { return hapticLeft >= 0 || hapticRight >= 0; }
};

/// One scheduled mono clip and where it goes.
///
/// A voice owns its OWN read position and its own lifetime. Nothing shared is
/// rewound, reset or truncated when another voice starts: a new impact adds a
/// voice, it never touches the ones already sounding. That is what makes a new
/// clang ring over the tail of the previous one instead of replacing it.
struct AudioVoice {
    const std::vector<float>* clip = nullptr;
    int channel = -1;
    float gain = 1.0f;
    std::size_t position = 0;
    /// Monotonic id, so "the oldest" is a fact rather than a guess about deque
    /// order.
    std::uint64_t sequence = 0;
    /// Only ever moved by RETIREMENT -- that is, by the voice cap being hit.
    /// A normal new event does NOT duck anything. When it is used it is a
    /// ramp, never a jump: a step in a waveform is a click.
    float duck = 1.0f;
    float duckTarget = 1.0f;
    float duckStep = 0.0f;
    bool retiring = false;
};

/// Summed-overlap protection. Deliberately NOT "divide by the number of active
/// voices": that would make every hit quieter the moment a second one starts,
/// which is audible as pumping and is exactly the artefact being removed.
/// Instead each channel keeps a gain that only moves when the sum actually
/// exceeds the threshold, and a soft knee catches the first sample before the
/// smoothed gain has had time to respond.
struct LimiterSettings {
    /// Level the limiter holds the channel at. Below this, gain is exactly 1.
    float threshold = 0.95f;
    /// Fixed headroom applied to the speaker channel only, so two overlapping
    /// clangs have somewhere to go before the limiter has to work at all.
    float speakerHeadroom = 1.0f;
    float hapticHeadroom = 1.0f;
    float attackMs = 1.0f;
    float releaseMs = 120.0f;
};

/// What the render path actually did, so "it glitched" can be pinned on a
/// measurement instead of guessed at.
struct AudioRenderStats {
    std::uint64_t pumps = 0;
    std::uint64_t framesSubmitted = 0;
    /// Gap between consecutive pumps. A gap longer than the buffer IS a
    /// dropout; that comparison is the point, not the raw number.
    std::int64_t worstFeedGapUs = 0;
    std::int64_t lastFeedGapUs = 0;
    std::uint32_t maxPadding = 0;
    /// Padding reached zero WHILE something was sounding. Padding sits at zero
    /// for the whole of a silent wait and that is not an underrun, so the two
    /// cases are counted separately and only this one means anything.
    std::uint64_t starvedWhileActive = 0;
    std::uint64_t idleZeroPadding = 0;
    std::uint64_t voicesStarted = 0;
    std::uint64_t voicesFinished = 0;   // reached the end of the clip -- the normal case
    std::uint64_t voicesEvicted = 0;    // hard-dropped at the absolute cap
    std::uint64_t voicesRetired = 0;    // faded out because the soft cap was reached
    /// Most voices alive at once. Compared against the cap, this is what says
    /// whether normal repeated parries ever come near the limit.
    std::size_t maxConcurrentVoices = 0;
    /// How many frames the limiter actually reduced gain on, and by how much.
    /// Zero means the summed mix never needed it.
    std::uint64_t limitedSpeakerFrames = 0;
    std::uint64_t limitedHapticFrames = 0;
    float worstSpeakerReductionDb = 0.0f;
    float worstHapticReductionDb = 0.0f;
    /// Peak of the SUM before any limiting, per group. This is the number that
    /// says whether overlap was actually about to clip.
    float peakSpeakerBeforeLimit = 0.0f;
    float peakHapticBeforeLimit = 0.0f;
    /// Samples the final clamp actually had to cut, kept apart so speaker
    /// clipping is never inferred from haptic clipping or the reverse.
    std::uint64_t clippedSpeakerSamples = 0;
    std::uint64_t clippedHapticSamples = 0;
    std::uint32_t bufferFrames = 0;
    float bufferMs = 0.0f;
    /// True when the audio engine drives the feed. False means the fallback
    /// timed feeder is running, which is worth saying out loud because it is
    /// the configuration that measured a 31 ms gap.
    bool eventDriven = false;
    /// Worst-case added latency from the buffer depth: a voice queued just
    /// after a pump waits for whatever has already been written.
    float writeAheadMs = 0.0f;
};

class DualSenseAudioDevice {
public:
    DualSenseAudioDevice();
    ~DualSenseAudioDevice();
    DualSenseAudioDevice(const DualSenseAudioDevice&) = delete;
    DualSenseAudioDevice& operator=(const DualSenseAudioDevice&) = delete;

    static std::vector<AudioEndpointInfo> Enumerate();

    /// Opens the endpoint, reports what it found, and does NOT start a
    /// stream. Safe to call for inspection only.
    static AudioFormatReport Describe(const std::wstring& endpointId);

    /// Raises (or lowers) Windows' master level for THIS endpoint only. This
    /// is an explicit, opt-in change to a per-device Windows setting -- it is
    /// never done implicitly, and it never touches the default playback
    /// device or system-wide volume. Returns false and leaves the device
    /// alone on failure.
    static VolumeSetResult SetEndpointVolume(const std::wstring& endpointId, float scalar);

    /// Reads this application's own session volume and the stream's per-channel
    /// levels from an OPEN device. Endpoint-wide values come from Describe().
    void RefreshSessionVolumes();

    /// Captures every frame handed to WASAPI so the exact submitted signal can
    /// be written out and inspected offline. Off by default.
    void BeginSubmitCapture(std::size_t maxFrames);
    /// Interleaved capture of what was submitted, plus the channel count.
    const std::vector<float>& SubmitCapture() const;
    unsigned SubmitCaptureChannels() const;
    void EndSubmitCapture();

    /// `bufferMs` is the shared-mode buffer asked for. It is both the dropout
    /// margin and the worst-case extra latency, so it is reported back through
    /// RenderStats() instead of being an invisible constant.
    bool Open(const std::wstring& endpointId, float bufferMs = 20.0f);

    /// Runs Pump() on a dedicated thread. This exists because the caller's
    /// loop also does ReadProcessMemory graph walks and console writes, either
    /// of which can stall well past the buffer length; audio must not share a
    /// thread with them.
    bool StartRenderThread();
    void StopRenderThread();
    bool RenderThreadRunning() const;

    AudioRenderStats RenderStats() const;
    void ResetRenderStats();

    /// Which channel is the speaker, for clipping accounting only. Never
    /// changes routing or gain.
    void SetSpeakerChannel(int channel);
    void Close();
    bool IsOpen() const;
    const AudioFormatReport& Format() const;
    const std::string& Error() const;

    /// Queue a mono clip onto one channel. Returns false if the channel is
    /// outside the endpoint's channel count -- it never silently retargets.
    bool Queue(const std::vector<float>& clip, int channel, float gain);

    /// Queue onto a pair with an optional left/right balance in [-1, 1].
    bool QueuePair(const std::vector<float>& clip, int leftChannel, int rightChannel,
                   float gain, float balance);

    /// Fill the shared-mode buffer. Call at least every few milliseconds on
    /// the thread that opened the device. Never blocks on playback finishing.
    bool Pump();

    /// Drop everything still queued (device loss, object replacement, exit)
    /// so stale cues can never be flushed out later.
    void DropPending();

    /// Number of voices still sounding.
    std::size_t ActiveVoices() const;

    /// Shortens (rather than cuts) whatever is already sounding, so a new
    /// impact's attack stays clean without a click. `factor` scales the
    /// remaining tail's amplitude.
    /// Fades whatever is already sounding down to `factor` over `fadeMs`.
    ///
    /// This is NOT called when a new event arrives. Overlapping tails is the
    /// point; a new clang rings over the previous one. It exists for the cases
    /// where something really must stop -- device loss, the player object
    /// being replaced, shutdown -- and for internal retirement at the cap.
    void DuckActive(float factor, float fadeMs = 5.0f);

    /// Soft cap: at this many voices the quietest, most-finished tail starts
    /// fading out over `retireFadeMs` instead of being cut. Nothing is dropped
    /// outright until `HardVoiceLimit()`, which sits above it.
    void SetVoiceLimit(std::size_t limit, float retireFadeMs);
    std::size_t HardVoiceLimit() const;

    void SetLimiter(const LimiterSettings& settings);
    const LimiterSettings& Limiter() const;

    std::size_t VoiceLimit() const;
    void SetVoiceLimit(std::size_t limit);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sekiro_haptics
