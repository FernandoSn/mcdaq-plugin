#pragma once

// Stages *copies* of already-acquired native ADC samples for a
// bounded number of hardware-clock frames, giving the independent USB DI poller
// time to observe transitions before SourceNode converts per-sample TTL levels
// to events. NEVER changes the ADC clock, original sample numbers, or raw data.
//
// In particular, this is NOT a retroactive Open Ephys event injection API:
// SourceNode::process() constructs events from TTL level words while reading
// the corresponding DataBuffer samples, so events cannot be backdated after
// that point. Fixed 1 ms holdback is used whenever DI channels are selected.
// With no DI channels selected, direct output bypasses this queue.
//
// Header-only, independent of JUCE and MCC, tested with a fake OE sink.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <deque>
#include <limits>
#include <new>
#include <vector>

class MCDAQDigitalHoldback
{
public:
    static constexpr int digitalDelayMs = 1;
    static constexpr std::size_t maxStagedPoints = 262144;

    struct Diagnostics
    {
        std::uint64_t stagedPoints = 0;
        std::uint64_t committedPoints = 0;
        std::uint64_t pendingPoints = 0;
        std::uint64_t maximumPendingPoints = 0;
        std::uint64_t stagedBlocks = 0;
        std::uint64_t committedBlocks = 0;
        std::uint64_t flushes = 0;
        std::uint64_t failures = 0;
        std::size_t pendingBlocks = 0;
        std::uint64_t delayFrames = 0;
        bool failed = false;
    };

    void reset (int channels, double framesPerSecond, int delayMs) noexcept
    {
        queue_.clear();
        diagnostics_ = {};
        channels_ = channels;
        haveNextPoint_ = false;
        expectedNextPoint_ = 0;
        newestFrameEnd_ = 0;
        enabled_ = delayMs == digitalDelayMs && channels > 0 &&
                   std::isfinite (framesPerSecond) && framesPerSecond > 0.0;
        if (enabled_)
        {
            const double desired = std::ceil (framesPerSecond * delayMs / 1000.0);
            if (! std::isfinite (desired) || desired > 100000000.0)
            {
                enabled_ = false;
                fail();
            }
            else
                diagnostics_.delayFrames = static_cast<std::uint64_t> (desired);
        }
        else if (delayMs != 0)
            fail();
    }

    bool enabled() const noexcept { return enabled_; }

    template<typename Emitter>
    bool push (const std::uint16_t* raw, std::size_t points,
               std::uint64_t firstPoint, Emitter emit)
    {
        if (! enabled_ || diagnostics_.failed || ! raw || channels_ <= 0 ||
            points == 0 || points % static_cast<std::size_t> (channels_) != 0 ||
            firstPoint % static_cast<std::uint64_t> (channels_) != 0 ||
            (haveNextPoint_ && firstPoint != expectedNextPoint_) ||
            points > maxStagedPoints ||
            diagnostics_.pendingPoints > maxStagedPoints - points ||
            firstPoint > std::numeric_limits<std::uint64_t>::max() - points)
        {
            fail();
            return false;
        }

        try
        {
            Block next;
            next.firstPoint = firstPoint;
            next.samples.assign (raw, raw + points);
            queue_.push_back (std::move (next));
        }
        catch (const std::bad_alloc&)
        {
            fail();
            return false;
        }

        haveNextPoint_ = true;
        expectedNextPoint_ = firstPoint + points;
        newestFrameEnd_ = expectedNextPoint_ / static_cast<std::uint64_t> (channels_);
        diagnostics_.stagedPoints += points;
        diagnostics_.pendingPoints += points;
        diagnostics_.maximumPendingPoints = (std::max) (
            diagnostics_.maximumPendingPoints, diagnostics_.pendingPoints);
        ++diagnostics_.stagedBlocks;
        diagnostics_.pendingBlocks = queue_.size();
        return drain (false, emit);
    }

    template<typename Emitter>
    bool flush (Emitter emit)
    {
        ++diagnostics_.flushes;
        return drain (true, emit);
    }

    Diagnostics diagnostics() const noexcept { return diagnostics_; }

private:
    struct Block
    {
        std::uint64_t firstPoint = 0;
        std::vector<std::uint16_t> samples;
    };

    void fail() noexcept
    {
        diagnostics_.failed = true;
        ++diagnostics_.failures;
    }

    template<typename Emitter>
    bool drain (bool flushEverything, Emitter emit)
    {
        if (diagnostics_.failed)
            return false;
        while (! queue_.empty())
        {
            const auto& item = queue_.front();
            const std::uint64_t endFrame =
                (item.firstPoint + item.samples.size()) /
                static_cast<std::uint64_t> (channels_);
            if (! flushEverything &&
                (endFrame > newestFrameEnd_ ||
                 newestFrameEnd_ - endFrame < diagnostics_.delayFrames))
                break;

            // The emitter must add EXACTLY this block, in its original order,
            // to OE's DataBuffer. Otherwise abort acquisition, never discard.
            if (emit (item.samples.data(), item.samples.size(), item.firstPoint)
                != item.samples.size())
            {
                fail();
                return false;
            }
            diagnostics_.committedPoints += item.samples.size();
            diagnostics_.pendingPoints -= item.samples.size();
            ++diagnostics_.committedBlocks;
            queue_.pop_front();
            diagnostics_.pendingBlocks = queue_.size();
        }
        return true;
    }

    bool enabled_ = false;
    bool haveNextPoint_ = false;
    int channels_ = 0;
    std::uint64_t expectedNextPoint_ = 0;
    std::uint64_t newestFrameEnd_ = 0;
    std::deque<Block> queue_;
    Diagnostics diagnostics_ {};
};
