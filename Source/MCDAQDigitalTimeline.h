#pragma once

// Host-polled MCC AUXPORT digital inputs, mapped approximately onto an analog
// sample timeline. Digital reads are NOT paced by the ADC clock on the USB-204
// or USB-1608FS-Plus. This class makes no hardware-sync accuracy claim.
// Header-only and independent of JUCE / MCC so it can be regression-tested.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>

class MCDAQDigitalTimeline
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Diagnostics
    {
        std::uint64_t observations = 0;
        std::uint64_t transitionsObserved = 0;
        std::uint64_t transitionsEmitted = 0;
        std::uint64_t lateTransitions = 0;
        std::uint64_t unrepresentableTransitions = 0;
        std::uint64_t queueOverflows = 0;
        std::size_t queuedTransitions = 0;
        // Per-transition interval between the two reads bracketing a change.
        // This represents software timing uncertainty, not a hardware guarantee.
        std::uint64_t maxEdgeObservationWindowUs = 0;
        double meanEdgeObservationWindowUs = 0.0;
    };

    void reset (std::uint8_t selectedMask,
                std::uint8_t initialPortValue,
                double rateHz,
                TimePoint approximateAnalogStart)
    {
        std::lock_guard<std::mutex> lock (mutex_);
        enabledMask_ = selectedMask;
        observedState_ = initialPortValue & enabledMask_;
        emittedState_ = observedState_;
        sampleRateHz_ = rateHz;
        analogStart_ = approximateAnalogStart;
        previousPollTime_ = approximateAnalogStart;
        nextFrame_ = 0;
        queue_.clear();
        diagnostics_ = {};
    }

    // A digital transition occurred between two observed port states.
    // Assign the midpoint between their observed times (read-call midpoints),
    // rather than the *completion* of the detecting USB call. The actual
    // electrical edge remains unlatched and cannot be reconstructed exactly.
    void observe (std::uint8_t rawPortValue, TimePoint readBegin, TimePoint readEnd)
    {
        std::lock_guard<std::mutex> lock (mutex_);
        ++diagnostics_.observations;
        if (readEnd < readBegin)
            return;
        const TimePoint readMidpoint = readBegin + (readEnd - readBegin) / 2;
        const TimePoint previousTime = previousPollTime_;
        previousPollTime_ = readMidpoint;
        const std::uint8_t next = rawPortValue & enabledMask_;
        if (next == observedState_)
            return;
        observedState_ = next;
        ++diagnostics_.transitionsObserved;

        if (queue_.size() >= maxQueuedTransitions)
        {
            ++diagnostics_.queueOverflows;
            return; // The caller must stop the stream rather than lose edges silently.
        }
        const TimePoint estimatedTransition = previousTime + (readMidpoint - previousTime) / 2;
        const auto windowUs = std::chrono::duration_cast<std::chrono::microseconds> (
            readMidpoint - previousTime).count();
        if (windowUs > 0)
        {
            const auto gapUs = static_cast<std::uint64_t> (windowUs);
            diagnostics_.maxEdgeObservationWindowUs =
                (std::max) (diagnostics_.maxEdgeObservationWindowUs, gapUs);
            const double count = static_cast<double> (diagnostics_.transitionsObserved);
            diagnostics_.meanEdgeObservationWindowUs +=
                (static_cast<double> (windowUs) - diagnostics_.meanEdgeObservationWindowUs) / count;
        }
        const auto seconds = std::chrono::duration<double> (
            estimatedTransition - analogStart_).count();
        const double frame = std::max (0.0, seconds * sampleRateHz_);
        const auto index = static_cast<std::uint64_t> (std::min (
            frame, static_cast<double> (std::numeric_limits<std::uint64_t>::max() / 2)));
        queue_.push_back ({ index, next });
    }

    // Call ONLY from the analog data-delivery thread; the poller only calls
    // observe(). The word is a LEVEL bitmask; SourceNode synthesizes edges by
    // comparing consecutive samples (not by interpreting these as edge codes).
    void fill (std::uint64_t firstFrame, std::size_t frames, std::uint64_t* words)
    {
        if (words == nullptr || frames == 0)
            return;
        std::lock_guard<std::mutex> lock (mutex_);
        for (std::size_t i = 0; i < frames; ++i)
        {
            const auto frame = firstFrame + static_cast<std::uint64_t> (i);
            unsigned changesAtFrame = 0;
            while (! queue_.empty() && queue_.front().frame <= frame)
            {
                const auto next = queue_.front();
                queue_.pop_front();
                if (next.frame < firstFrame || next.frame < nextFrame_)
                    ++diagnostics_.lateTransitions;
                emittedState_ = next.state;
                ++diagnostics_.transitionsEmitted;
                ++changesAtFrame;
            }
            // Two or more distinct full-port transitions at one ADC frame
            // cannot be encoded as two edges in one level word. Flag them.
            if (changesAtFrame > 1)
                diagnostics_.unrepresentableTransitions += changesAtFrame - 1;
            words[i] = static_cast<std::uint64_t> (emittedState_);
        }
        nextFrame_ = firstFrame + static_cast<std::uint64_t> (frames);
    }

    Diagnostics getDiagnostics() const
    {
        std::lock_guard<std::mutex> lock (mutex_);
        Diagnostics d = diagnostics_;
        d.queuedTransitions = queue_.size();
        return d;
    }

private:
    struct Change { std::uint64_t frame; std::uint8_t state; };
    static constexpr std::size_t maxQueuedTransitions = 8192;
    mutable std::mutex mutex_;
    std::deque<Change> queue_;
    std::uint8_t enabledMask_ = 0;
    std::uint8_t observedState_ = 0;
    std::uint8_t emittedState_ = 0;
    double sampleRateHz_ = 1000.0;
    TimePoint analogStart_ {};
    TimePoint previousPollTime_ {};
    std::uint64_t nextFrame_ = 0;
    Diagnostics diagnostics_ {};
};
