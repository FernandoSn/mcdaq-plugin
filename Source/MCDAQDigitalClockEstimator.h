#pragma once

// Online relative-clock estimator for software-polled MCC digital inputs.
// Takes (cumulative hardware ADC count, host callback receipt time) pairs.
// NO MCC calls, JUCE dependency, absolute-start synchronization, or
// change to the analog sampling rate / samples / metadata.
// Driver callback transport latency is *not* an edge timestamp. We fit only
// the long-term relative clock *rate*, after robust outlier rejection.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

class MCDAQDigitalClockEstimator
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Diagnostics
    {
        std::uint64_t callbacksReceived = 0;
        std::uint64_t acceptedObservations = 0;
        std::uint64_t acceptedFits = 0;
        std::uint64_t rejectedFits = 0;
        double fitSpanSeconds = 0.0;
        double estimatedCorrectionPpm = 0.0;
        double appliedCorrectionPpm = 0.0;
        double fitUncertaintyPpm = 0.0;
        std::uint64_t fallbackCount = 0;
        bool calibrated = false;
    };

    void reset (double nominalFramesPerSecond, int analogChannelCount,
                TimePoint origin)
    {
        nominalRate_ = nominalFramesPerSecond;
        channelCount_ = analogChannelCount;
        origin_ = origin;
        previousRawCount_ = 0;
        accumulatedPoints_ = 0;
        havePreviousCount_ = false;
        lastAcceptedTime_ = -1e12;
        lastFitTime_ = -1e12;
        lastGoodFitTime_ = -1e12;
        appliedRate_ = nominalFramesPerSecond;
        points_.clear();
        diagnostics_ = {};
    }

    // Returns true only when a new, vetted slope should be applied. Note:
    // the returned rate is a *digital-only* mapping and must NEVER replace
    // the nominal ADC rate used by Open Ephys.
    bool observeCallback (std::uint32_t rawAggregateCount,
                          TimePoint observedAt, double& proposedRate)
    {
        ++diagnostics_.callbacksReceived;
        if (!(nominalRate_ > 0.0) || channelCount_ <= 0)
            return false;
        const double seconds = std::chrono::duration<double> (observedAt - origin_).count();
        if (!std::isfinite(seconds) || seconds < 0.0)
            return false;

        if (!havePreviousCount_)
        {
            // MCC eventData is the aggregate ADC-point count since start.
            accumulatedPoints_ = rawAggregateCount;
            havePreviousCount_ = true;
        }
        else
        {
            // Unsigned subtraction naturally handles the 32-bit MCC count wrap.
            const std::uint32_t delta = rawAggregateCount - previousRawCount_;
            // A huge jump within one callback observation is not credible;
            // reject rather than infer a clock change from a counter reset.
            if (delta > static_cast<std::uint32_t> (nominalRate_ * channelCount_ * 10.0))
            {
                previousRawCount_ = rawAggregateCount;
                return false;
            }
            accumulatedPoints_ += delta;
        }
        previousRawCount_ = rawAggregateCount;

        // Decimate callback arrivals to one observation every 0.25 s.
        // This also reduces serial correlation from USB transfer bursts.
        if (seconds - lastAcceptedTime_ < 0.25)
            return false;
        lastAcceptedTime_ = seconds;
        const double frame = static_cast<double> (accumulatedPoints_) / channelCount_;
        points_.push_back ({ seconds, frame - nominalRate_ * seconds });
        ++diagnostics_.acceptedObservations;

        // A rolling two-minute history supports slow relative drift while
        // not assuming the CPU clock and MCC oscillator have equal speeds.
        while (!points_.empty() && seconds - points_.front().seconds > 120.0)
            points_.pop_front();
        if (seconds - lastFitTime_ < 5.0)
            return false;
        lastFitTime_ = seconds;
        if (points_.size() < 45 || seconds - points_.front().seconds < 20.0)
            return false;

        const double span = points_.back().seconds - points_.front().seconds;
        double slope = 0.0, uncertaintyPpm = 0.0;
        if (!fitRobust(slope, uncertaintyPpm))
        {
            ++diagnostics_.rejectedFits;
            return false;
        }
        const double correctionPpm = slope / nominalRate_ * 1e6;
        // General safety envelope, not a fitted board/PC offset. No
        // correction is applied if callback timing produces an implausible
        // oscillator discrepancy or a poorly constrained estimate.
        if (!std::isfinite(correctionPpm) || std::abs(correctionPpm) > 1000.0 ||
            !std::isfinite(uncertaintyPpm) || uncertaintyPpm > 30.0)
        {
            ++diagnostics_.rejectedFits;
            return false;
        }

        // Smooth noisy fits and cap the change in clock rate per update.
        // A continuous timebase change (handled by the timeline) prevents
        // timestamp discontinuities or queued-edge reordering.
        const double targetRate = nominalRate_ + slope;
        const double blendedRate = appliedRate_ + 0.30 * (targetRate - appliedRate_);
        const double maxStep = nominalRate_ * (15.0 / 1e6);
        appliedRate_ += (std::max) (-maxStep, (std::min) (maxStep, blendedRate - appliedRate_));
        proposedRate = appliedRate_;
        lastGoodFitTime_ = seconds;
        ++diagnostics_.acceptedFits;
        diagnostics_.fitSpanSeconds = span;
        diagnostics_.fitUncertaintyPpm = uncertaintyPpm;
        diagnostics_.estimatedCorrectionPpm = correctionPpm;
        diagnostics_.appliedCorrectionPpm = (appliedRate_ / nominalRate_ - 1.0) * 1e6;
        diagnostics_.calibrated = true;
        return true;
    }

    // If callback observations are absent / chronically unreliable, abandon
    // relative-rate correction and revert to the nominal v2.1 slope.
    // The timeline preserves phase continuity when this rate is applied.
    bool maintainFallback (TimePoint now, double& fallbackRate)
    {
        if (!diagnostics_.calibrated) return false;
        const double secs = std::chrono::duration<double> (now - origin_).count();
        if (secs - lastGoodFitTime_ < 45.0) return false;
        appliedRate_ = nominalRate_;
        diagnostics_.appliedCorrectionPpm = 0.0;
        diagnostics_.calibrated = false;
        ++diagnostics_.fallbackCount;
        fallbackRate = nominalRate_;
        return true;
    }

    Diagnostics diagnostics() const { return diagnostics_; }

private:
    struct Observation { double seconds; double residualFrames; };
    static double median (std::vector<double>& values)
    {
        const auto half = values.begin() + static_cast<std::ptrdiff_t> (values.size() / 2);
        std::nth_element (values.begin(), half, values.end());
        return *half;
    }

    bool fitRobust (double& slope, double& uncertaintyPpm) const
    {
        // Residuals are in FRAME units. Center t first to avoid catastrophic
        // cancellation for long recording sessions.
        auto fit = [this] (const std::vector<bool>* accept,
                           double& intercept, double& gradient,
                           double& tMean, double& sumX2, int& count) -> bool
        {
            double tSum = 0.0, ySum = 0.0;
            count = 0;
            for (std::size_t i = 0; i < points_.size(); ++i)
            {
                if (accept && !(*accept)[i]) continue;
                tSum += points_[i].seconds;
                ySum += points_[i].residualFrames;
                ++count;
            }
            if (count < 20) return false;
            tMean = tSum / count;
            const double yMean = ySum / count;
            double numerator = 0.0;
            sumX2 = 0.0;
            for (std::size_t i = 0; i < points_.size(); ++i)
            {
                if (accept && !(*accept)[i]) continue;
                const double x = points_[i].seconds - tMean;
                numerator += x * (points_[i].residualFrames - yMean);
                sumX2 += x * x;
            }
            if (sumX2 < 1.0) return false;
            gradient = numerator / sumX2;
            intercept = yMean - gradient * tMean;
            return true;
        };

        double a = 0.0, b = 0.0, xbar = 0.0, sxx = 0.0;
        int count = 0;
        if (!fit(nullptr, a, b, xbar, sxx, count)) return false;
        std::vector<double> residuals;
        residuals.reserve(points_.size());
        for (const auto& p : points_)
            residuals.push_back(p.residualFrames - (a + b * p.seconds));
        std::vector<double> medianWork = residuals;
        const double center = median(medianWork);
        for (auto& value : residuals) value = std::abs(value - center);
        const double mad = median(residuals);
        // Automatic robust scatter threshold (floor=1 ms hardware time).
        const double threshold = (std::max) (nominalRate_ * 0.001, 4.5 * 1.4826 * mad);
        std::vector<bool> keep(points_.size());
        int kept = 0;
        for (std::size_t i = 0; i < points_.size(); ++i)
        {
            keep[i] = std::abs(points_[i].residualFrames - (a + b * points_[i].seconds) - center) <= threshold;
            if (keep[i]) ++kept;
        }
        if (kept * 2 < static_cast<int> (points_.size())) return false;
        if (!fit(&keep, a, b, xbar, sxx, count)) return false;
        double rss = 0.0;
        for (std::size_t i = 0; i < points_.size(); ++i)
        {
            if (!keep[i]) continue;
            const double r = points_[i].residualFrames - (a + b * points_[i].seconds);
            rss += r * r;
        }
        slope = b;
        uncertaintyPpm = std::sqrt((rss / (count - 2)) / sxx) / nominalRate_ * 1e6;
        return std::isfinite(slope) && std::isfinite(uncertaintyPpm);
    }

    double nominalRate_ = 0.0;
    double appliedRate_ = 0.0;
    int channelCount_ = 0;
    TimePoint origin_ {};
    std::uint32_t previousRawCount_ = 0;
    std::uint64_t accumulatedPoints_ = 0;
    bool havePreviousCount_ = false;
    double lastAcceptedTime_ = -1e12;
    double lastFitTime_ = -1e12;
    double lastGoodFitTime_ = -1e12;
    std::deque<Observation> points_;
    Diagnostics diagnostics_ {};
};
