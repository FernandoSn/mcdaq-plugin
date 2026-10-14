#pragma once

#include <cstdint>
#include <string>

// Nominal sample-clock model validated against USBPcap scan-start commands
// captured with MCC Universal Library for these two specific devices.
//
// USB-204: one multiplexed ADC, 70 MHz pacer for ALL enabled channels;
//          integer divider = floor(70 MHz / (requested rate * channel count)).
// USB-1608FS-Plus: simultaneous ADCs, 40 MHz scan pacer;
//          integer divider = floor(40 MHz / requested per-channel rate).
//
// The physical oscillator may not be exactly 70/40 MHz. This predicts the
// NOMINAL programmed hardware rate, not a calibrated absolute timebase.
// It also assumes UL's scan-command generation stays as observed in the
// verified October 2026 USB captures (a changed driver needs revalidation).
struct MCDAQClockPrediction
{
    double nominalRateHz = 0.0;       // nominal samples/s PER CHANNEL
    std::uint32_t timerDivider = 0;  // pacer period = timerDivider ticks
    std::uint32_t registerValue = 0; // USB pacer register = divider - 1
    std::uint64_t oscillatorHz = 0;
    bool valid = false;
};

inline MCDAQClockPrediction predictMCDAQClock (
    const std::string& productName, long requestedRateHz,
    int enabledChannels) noexcept
{
    MCDAQClockPrediction result;
    if (requestedRateHz <= 0 || enabledChannels < 1 || enabledChannels > 8)
        return result;

    const bool usb204 = productName == "USB-204";
    const bool usb1608 = productName == "USB-1608FS-Plus";
    if (! usb204 && ! usb1608)
        return result;

    result.oscillatorHz = usb204 ? 70'000'000ULL : 40'000'000ULL;
    const auto rate = static_cast<std::uint64_t> (requestedRateHz);
    const auto conversionsPerFrame = static_cast<std::uint64_t> (
        usb204 ? enabledChannels : 1);

    // Overflow-safe guard; a timer divider < 1 is not possible.
    if (rate > result.oscillatorHz / conversionsPerFrame)
        return result;

    const auto divider = result.oscillatorHz / (rate * conversionsPerFrame);
    if (divider == 0 || divider > 0xFFFFFFFFULL)
        return result;

    result.timerDivider = static_cast<std::uint32_t> (divider);
    result.registerValue = static_cast<std::uint32_t> (divider - 1);
    result.nominalRateHz = static_cast<double> (result.oscillatorHz) /
        (static_cast<double> (divider) * static_cast<double> (conversionsPerFrame));
    result.valid = true;
    return result;
}
