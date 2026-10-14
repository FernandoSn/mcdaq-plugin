/*
 ------------------------------------------------------------------

 This file is part of the Open Ephys GUI
 Copyright (C) 2022 Open Ephys

 ------------------------------------------------------------------

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

#include "DataThreadPlugin.h"
#include "DataThreadPluginEditor.h"

// Include MCC-specific definitions after Open Ephys headers
#include "MCDAQDeviceManager.h"
#include "MCDAQAcquisition.h"
#include "MCDAQClock.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>
#include <system_error>

// Set to 1 when detailed acquisition diagnostics are needed.
#define MCDAQ_VERBOSE_ACQUISITION_LOGS 0

struct PluginSettingsObject
{
    // Store settings for the plugin here
};


DataThreadPlugin::DataThreadPlugin (SourceNode* sn)
    : DataThread (sn),
      deviceManager (std::make_unique<MCDAQDeviceManager>())
{
    LOGC ("MCDAQ: Scanning for MCC USB devices...");

    const int error = deviceManager->refresh();

    if (error != 0)
    {
        LOGE ("MCDAQ: Device discovery failed. Error: ", error);
        return;
    }

    const auto& devices = deviceManager->getDevices();

    LOGC ("MCDAQ: Found ", (int) devices.size(), " device(s).");

    for (size_t i = 0; i < devices.size(); ++i)
    {
        const auto& device = devices[i];

        LOGC ("MCDAQ: [", (int) i, "] ", device.productName.c_str(), " | Unique ID: ", device.uniqueId.c_str(), " | Board number: ", device.boardNumber);
    }

    // Automatically reserve one available board.
    if (deviceManager->selectFirstAvailableBoard())
    {
        const auto name =
            deviceManager->getSelectedProductName();

        const auto id =
            deviceManager->getSelectedUniqueId();

        LOGC ("MCDAQ: Selected ",
              name.c_str(),
              " | Board number: ",
              deviceManager->getSelectedBoardNumber(),
              " | Unique ID: ",
              id.c_str());

        // Diagnostic clock-output experiment. Keep this separate from the
        // normal acquisition path; it only configures the 1608 SYNC pin.
        // Change this to false after the timing experiment if SYNC output
        // is not needed for routine recordings.
        constexpr bool enable1608SyncForClockTest = true;
        if (enable1608SyncForClockTest && name == "USB-1608FS-Plus")
        {
            const auto sync = deviceManager->enableSelectedSyncOutput();

            if (sync.enabled)
            {
                LOGC ("MCDAQ: USB-1608FS-Plus SYNC output ENABLED and "
                      "verified (BIINPUTPACEROUT=", sync.readback,
                      ", board ", deviceManager->getSelectedBoardNumber(),
                      "). Internal pacer should appear on SYNC while scanning.");
            }
            else
            {
                LOGE ("MCDAQ: USB-1608FS-Plus SYNC output NOT verified. "
                      "cbSetConfig error=", sync.setError,
                      ", cbGetConfig error=", sync.getError,
                      ", readback=", sync.readback,
                      ". Analog acquisition remains available.");
            }
        }

        float voltage = 0.0f;

        const int readError =
            deviceManager->readAnalogVoltage (0, voltage);

        if (readError == 0)
        {
            LOGC ("MCDAQ: AI0 voltage = ", voltage, " V");
        }
        else
        {
            LOGE ("MCDAQ: Analog input test failed. UL error: ",
                  readError);
        }

    }
    else
    {
        LOGE ("MCDAQ: No unassigned, registered MCC "
              "board is available.");
    }
}


DataThreadPlugin::~DataThreadPlugin()
{
    stopDigitalPoller();
}

bool DataThreadPlugin::foundInputSource()
{
    return deviceManager && deviceManager->getSelectedBoardNumber() >= 0 &&
           deviceManager->getSelectedScanCapabilities().supported;
}

namespace
{
constexpr long allowedRates[] = { 500, 1000, 2000, 5000, 10000, 25000, 30000 };
bool allowedRate (long hz)
{
    for (const long rate : allowedRates)
        if (rate == hz) return true;
    return false;
}
}

std::vector<long> DataThreadPlugin::getAvailableRates() const
{
    std::vector<long> result;
    if (! deviceManager) return result;
    const auto cap = deviceManager->getSelectedScanCapabilities();
    if (! cap.supported) return result;
    for (const long rate : allowedRates)
        if (rate <= cap.maxPerChannelHz &&
            static_cast<int64_t> (rate) * selectedChannelCount <= cap.maxAggregateHz)
            result.push_back (rate);
    return result;
}

bool DataThreadPlugin::isChannelEnabled (int channel) const noexcept
{
    return channel >= 0 && channel < 8 &&
           (selectedChannelMask & static_cast<std::uint8_t> (1u << channel)) != 0;
}

bool DataThreadPlugin::isDigitalEnabled (int channel) const noexcept
{
    return channel >= 0 && channel < 8 &&
           (selectedDigitalMask & static_cast<std::uint8_t> (1u << channel)) != 0;
}

bool DataThreadPlugin::setDigitalMask (std::uint8_t mask)
{
    if (CoreServices::getAcquisitionStatus() || acquisition || isThreadRunning() || ! deviceManager)
        return false;

    const auto cap = deviceManager->getSelectedScanCapabilities();
    if (! cap.supported || cap.digitalChannels < 0 ||
        (static_cast<unsigned> (mask) >> cap.digitalChannels) != 0u)
        return false;

    selectedDigitalMask = mask; // zero disables digital polling entirely.
    return true;
}

std::vector<int> DataThreadPlugin::getSelectedChannels() const
{
    std::vector<int> result;
    for (int channel = 0; channel < 8; ++channel)
        if (isChannelEnabled (channel))
            result.push_back (channel);
    return result;
}

bool DataThreadPlugin::setChannelMask (std::uint8_t mask)
{
    if (CoreServices::getAcquisitionStatus() || acquisition || isThreadRunning() || mask == 0 || ! deviceManager)
        return false;

    const auto cap = deviceManager->getSelectedScanCapabilities();
    if (! cap.supported || (mask >> cap.analogChannels) != 0)
        return false;

    int count = 0;
    for (int channel = 0; channel < 8; ++channel)
        if (mask & (1u << channel)) ++count;

    if (selectedSampleRateHz > cap.maxPerChannelHz ||
        static_cast<int64_t> (selectedSampleRateHz) * count > cap.maxAggregateHz)
        return false;

    selectedChannelMask = mask;
    selectedChannelCount = count;
    rateNegotiated = false;
    return true;
}

int DataThreadPlugin::getAnalogInputCount() const
{
    return deviceManager ? deviceManager->getSelectedScanCapabilities().analogChannels : 0;
}

int DataThreadPlugin::getDigitalInputCount() const
{
    return deviceManager ? deviceManager->getSelectedScanCapabilities().digitalChannels : 0;
}

std::string DataThreadPlugin::getBoardName() const
{
    return deviceManager ? deviceManager->getSelectedProductName() : std::string();
}

bool DataThreadPlugin::canSelectVoltageRange() const
{
    return getBoardName() == "USB-1608FS-Plus";
}

bool DataThreadPlugin::setVoltageRangeVolts (int fullScaleVolts)
{
    if (CoreServices::getAcquisitionStatus() || acquisition || isThreadRunning())
        return false;

    if (fullScaleVolts != 10 && fullScaleVolts != 5 &&
        fullScaleVolts != 2 && fullScaleVolts != 1)
        return false;

    if (! canSelectVoltageRange() && fullScaleVolts != 10)
        return false;

    selectedRangeVolts = fullScaleVolts;
    rateNegotiated = false;
    return true;
}

bool DataThreadPlugin::setSampleRateHz (long rateHz)
{
    if (CoreServices::getAcquisitionStatus() || acquisition || isThreadRunning() ||
        ! allowedRate (rateHz))
        return false;
    const auto cap = deviceManager->getSelectedScanCapabilities();
    if (! cap.supported || rateHz > cap.maxPerChannelHz ||
        static_cast<int64_t> (rateHz) * selectedChannelCount > cap.maxAggregateHz)
        return false;
    selectedSampleRateHz = rateHz;
    rateNegotiated = false;
    return true;
}

bool DataThreadPlugin::isSimultaneousSampling() const
{
    return deviceManager && deviceManager->getSelectedScanCapabilities().simultaneous;
}

std::string DataThreadPlugin::getSamplingModeDescription() const
{
    const auto cap = deviceManager->getSelectedScanCapabilities();
    if (! cap.supported) return "Unknown board: unsupported";
    return cap.simultaneous ? "Simultaneous ADCs" : "Multiplexed (sequential ADC)";
}

double DataThreadPlugin::getDeclaredSampleRateHz() const
{
    if (! deviceManager || ! rateNegotiated ||
        negotiatedRequestHz != selectedSampleRateHz ||
        negotiatedChannelMask != selectedChannelMask ||
        negotiatedRangeVolts != selectedRangeVolts ||
        negotiatedBoardNum != deviceManager->getSelectedBoardNumber())
        return 0.0; // Unknown: never advertise an unvalidated stream rate.

    return negotiatedPreciseRateHz;
}

bool DataThreadPlugin::negotiateSelectedRate()
{
    if (getDeclaredSampleRateHz() > 0.0)
        return true;
    if (! deviceManager || ! foundInputSource() ||
        CoreServices::getAcquisitionStatus() || acquisition || isThreadRunning())
        return false;

    const int boardNum = deviceManager->getSelectedBoardNumber();
    long actualRate = 0;
    const int error = MCDAQAcquisition::probeActualRate (
        boardNum, selectedSampleRateHz, getSelectedChannels(),
        selectedRangeVolts, actualRate);

    if (error != 0 || actualRate <= 0)
    {
        rateNegotiated = false;
        negotiatedRateHz = 0;
        negotiatedPreciseRateHz = 0.0;
        negotiatedClockDivider = 0;
        LOGE ("MCDAQ: Hardware clock preflight failed for ",
              getBoardName().c_str(), " [", selectedChannelCount,
              " channels] at nominal ", selectedSampleRateHz,
              " Hz/channel. UL error: ", error,
              ". Stream registration blocked to protect timestamps.");
        return false;
    }

    // MCC's Rate parameter is an integer, and its USB-204 return values do
    // not reproduce the hardware's programmed timer. Preserve that integer
    // for the preflight/start consistency check, but register a precise
    // hardware-derived nominal rate in Open Ephys instead.
    const auto clock = predictMCDAQClock (
        getBoardName(), selectedSampleRateHz, selectedChannelCount);

    if (! clock.valid)
    {
        rateNegotiated = false;
        negotiatedRateHz = 0;
        negotiatedPreciseRateHz = 0.0;
        negotiatedClockDivider = 0;
        LOGE ("MCDAQ: No supported internal-clock model for ",
              getBoardName().c_str(), " (", selectedChannelCount,
              " channels, request ", selectedSampleRateHz, " Hz).");
        return false;
    }

    // Guard against a future UL version using substantially different scan
    // command generation. This does not read the timer register at runtime:
    // a new driver still requires USB capture validation of the divider model.
    constexpr double maxRelativeULDisagreement = 0.005; // 0.5%
    const double permittedDifferenceHz = (std::max) (
        2.0, maxRelativeULDisagreement * clock.nominalRateHz);

    if (std::abs (static_cast<double> (actualRate) - clock.nominalRateHz) >
            permittedDifferenceHz)
    {
        rateNegotiated = false;
        negotiatedRateHz = 0;
        negotiatedPreciseRateHz = 0.0;
        negotiatedClockDivider = 0;
        LOGE ("MCDAQ: UL-reported rate ", actualRate,
              " Hz is incompatible with the nominal timer prediction for ",
              getBoardName().c_str(), " at ", selectedSampleRateHz,
              " Hz/channel (", selectedChannelCount,
              " channels). Refusing to register an unverified clock.");
        return false;
    }

    negotiatedBoardNum = boardNum;
    negotiatedRequestHz = selectedSampleRateHz;
    negotiatedChannelMask = selectedChannelMask;
    negotiatedRangeVolts = selectedRangeVolts;
    negotiatedRateHz = actualRate;
    negotiatedPreciseRateHz = clock.nominalRateHz;
    negotiatedClockDivider = clock.timerDivider;
    rateNegotiated = true;

    LOGC ("MCDAQ: Clock preflight: requested ", selectedSampleRateHz,
          " Hz/channel | MCC returned ", actualRate,
          " Hz/channel | Nominal pacer ",
          String (clock.nominalRateHz, 6), " Hz/channel | Divider ",
          clock.timerDivider, " (register ", clock.registerValue,
          ") | Oscillator ", static_cast<int64_t> (clock.oscillatorHz),
          " Hz | Channels ", selectedChannelCount,
          " | Board ", getBoardName().c_str());
    return true;
}

void DataThreadPlugin::updateSettings (
    OwnedArray<ContinuousChannel>* continuousChannels,
    OwnedArray<EventChannel>* eventChannels,
    OwnedArray<SpikeChannel>* spikeChannels,
    OwnedArray<DataStream>* dataStreams,
    OwnedArray<DeviceInfo>* devices,
    OwnedArray<ConfigurationObject>* configurationObjects)
{
    continuousChannels->clear();
    eventChannels->clear();
    spikeChannels->clear();
    dataStreams->clear();
    devices->clear();
    configurationObjects->clear();

    if (! foundInputSource() || ! negotiateSelectedRate())
        return;

    const std::string product =
        deviceManager->getSelectedProductName();

    const std::string uniqueId =
        deviceManager->getSelectedUniqueId();

    const String streamName =
        "MCDAQ " + String (product.c_str());

    const String streamIdentifier =
        "mcdaq-" + String (uniqueId.c_str());

    // One synchronous stream; channel order follows the ascending MCC queue.
    // Open Ephys DataStream::Settings stores float; sample timestamps below
    // use the corresponding full-precision double nominal pacer rate.
    const float sampleRate = static_cast<float> (getDeclaredSampleRateHz());

    DataStream::Settings streamSettings {
        streamName,
        "Analog input from MCC Universal Library",
        streamIdentifier,
        sampleRate,
        true
    };

    auto* stream = new DataStream (streamSettings);
    dataStreams->add (stream);

    // Nominal voltage resolution for the two boards.
    // We will validate scaling before recording.
    const float bitVolts =
        product == "USB-204"
            ? 20.0f / 4096.0f
            : (2.0f * selectedRangeVolts) / 65536.0f;

    for (const int channel : getSelectedChannels())
    {
        const String number (channel);
        ContinuousChannel::Settings channelSettings {
            ContinuousChannel::Type::ADC,
            "AI" + number,
            "MCC analog input channel " + number,
            "mcdaq-ai" + number,
            bitVolts,
            stream
        };
        continuousChannels->add (new ContinuousChannel (channelSettings));
    }

    // SourceNode converts changes in the sample-level TTL state word into
    // edge events. These events are from SOFTWARE-POLLED AUXPORT reads, not
    // hardware-synchronous reads. The unselected DI lines are forced low.
    EventChannel::Settings eventSettings {
        EventChannel::Type::TTL,
        "MCDAQ Digital Inputs (polled)",
        "Software-polled MCC AUXPORT inputs; approximate event timing",
        "mcdaq-ttl",
        stream,
        8
    };

    eventChannels->add (
        new EventChannel (eventSettings));

    juce::String channelNames;
    for (const int channel : getSelectedChannels())
    {
        if (channelNames.isNotEmpty()) channelNames += ",";
        channelNames += "AI" + String (channel);
    }
    LOGC ("MCDAQ: Registered [", channelNames,
          "] (", selectedChannelCount, " channels) at ", sampleRate,
          " Hz/channel | Aggregate nominal: ",
          sampleRate * selectedChannelCount,
          " points/s | Sampling: ", getSamplingModeDescription().c_str(),
          " | Input range: +/-", selectedRangeVolts,
          " V | Nominal bitVolts: ", bitVolts,
          " | DI selected mask: 0x",
          String::toHexString (static_cast<int> (selectedDigitalMask)),
          " (software-polled; not ADC-synchronous)");
}

bool DataThreadPlugin::startAcquisition()
{
    LOGC ("MCDAQ DEBUG 1: Entered startAcquisition()");

    if (! foundInputSource() || getDeclaredSampleRateHz() <= 0.0)
    {
        LOGE ("MCDAQ: No supported board or validated stream rate. "
              "Change a setting to retry hardware clock negotiation.");
        return false;
    }

    const int boardNum =
        deviceManager->getSelectedBoardNumber();

    LOGC ("MCDAQ DEBUG 2: Creating acquisition object");

    acquisition =
        std::make_unique<MCDAQAcquisition> (boardNum);

    acquisition->setRawBlockCallback (
        &DataThreadPlugin::receiveRawBlock,
        this);
    acquisition->enableTimingObservations (selectedDigitalMask != 0);

    // Digital input selection is opt-in. Configure the selected AUXPORT bits
    // and read the baseline BEFORE starting analog acquisition. This avoids
    // USB register configuration calls on the analog worker.
    std::uint8_t initialDigitalPort = 0;
    if (selectedDigitalMask != 0)
    {
        const int configError = deviceManager->configureSelectedDigitalInputs (selectedDigitalMask);
        const int readError = configError == 0
            ? deviceManager->readSelectedDigitalPort (initialDigitalPort)
            : configError;
        if (readError != 0)
        {
            LOGE ("MCDAQ: Cannot enable selected DI channels; UL error ", readError,
                  ". Analog acquisition was not started. Disable DI or investigate the port.");
            acquisition.reset();
            return false;
        }
    }

    LOGC ("MCDAQ DEBUG 3: Calling MCC scan start");

    // Approximate host-clock anchor only. MCC does not provide a digital
    // latch timestamp, so DI->AI timing MUST NOT be treated as sample-accurate.
    const auto analogStartBegin = MCDAQDigitalTimeline::Clock::now();
    const int error = acquisition->start (selectedSampleRateHz, getSelectedChannels(), selectedRangeVolts);
    const auto analogStartEnd = MCDAQDigitalTimeline::Clock::now();

    LOGC ("MCDAQ DEBUG 4: MCC returned ", error);

    if (error != 0)
    {
        LOGE ("MCDAQ: Failed to start scan. UL error: ",
              error);

        acquisition.reset();
        return false;
    }

    // Confirm that AUXPORT reads also work with the analog scan already
    // running. A few device/driver combinations may refuse concurrent reads.
    if (selectedDigitalMask != 0)
    {
        std::uint8_t livePort = 0;
        const int concurrentReadError = deviceManager->readSelectedDigitalPort (livePort);
        if (concurrentReadError != 0)
        {
            LOGE ("MCDAQ: AUXPORT read failed during analog scan (UL ",
                  concurrentReadError, "). Stopping: DI cannot be monitored safely.");
            acquisition->stop();
            const auto stopped = acquisition->getStopDiagnostics();
            if (stopped.stopError == 0 && stopped.disableEventError == 0)
                acquisition.reset();
            return false;
        }
    }

    diagnosticTicks = 0;

    const auto stats = acquisition->getStatistics();

    // Metadata has already been registered with Open Ephys at the
    // predicted precise nominal rate. The preflight and real scan must
    // still return IDENTICAL UL integer rates for this same request.
    // Never compare UL's rounded rate directly with our precise rate.
    const double declaredRate = getDeclaredSampleRateHz();
    if (stats.actualRate != negotiatedRateHz || declaredRate <= 0.0)
    {
        LOGE ("MCDAQ: UL rate changed between preflight and start: ",
              "preflight ", negotiatedRateHz, " Hz, scan ", stats.actualRate,
              " Hz. Stream nominal pacer rate ", String (declaredRate, 6),
              " Hz/channel. Acquisition cancelled to protect timestamps.");
        acquisition->stop();
        const auto stopped = acquisition->getStopDiagnostics();
        if (stopped.stopError == 0 && stopped.disableEventError == 0)
            acquisition.reset();
        else
            LOGE ("MCDAQ: Shutdown incomplete after clock mismatch.");
        return false;
    }

    // Must agree with updateSettings(): the same precise rate controls the
    // DataStream metadata and every sample timestamp sent to Open Ephys.
    activeSampleRateHz = declaredRate;

    LOGC ("MCDAQ: Scan clock validated. Requested ", selectedSampleRateHz,
          " Hz/channel | MCC reported ", stats.actualRate,
          " Hz/channel | Nominal pacer / OE timestamps ",
          String (activeSampleRateHz, 6),
          " Hz/channel | Divider ", negotiatedClockDivider);

    LOGC ("MCDAQ: Continuous scan started. Board: ",
          boardNum,
          " | Nominal hardware rate: ",
          String (activeSampleRateHz, 6),
          " Hz/channel | Channels: ", stats.channelCount,
          " | Aggregate: ", String (activeSampleRateHz * stats.channelCount, 3),
          " ADC points/s | Mode: ", getSamplingModeDescription().c_str(),
          " | UL buffer: ", stats.bufferPoints,
          " points | MCC event: ", stats.notificationPoints,
          " points");

    LOGC ("MCDAQ DEBUG 5: Starting DataThread");

    if (sourceBuffers.size() > 0)
        sourceBuffers[0]->clear();

    // Digital-only host time anchor: use the completion of the existing
    // cbAInScan start call. Do not issue extra cbGetStatus queries at startup;
    // the analog acquisition/event path must remain untouched. This is an
    // approximate software reference, NOT an ADC-latched frame-zero time.
    digitalTimeline.reset (selectedDigitalMask, initialDigitalPort,
                           activeSampleRateHz, analogStartEnd);
    digitalClockEstimator.reset (activeSampleRateHz, selectedChannelCount,
                                 analogStartEnd);
    digitalPollError.store (0);
    digitalPollCount.store (0);
    digitalMaxPollGapUs.store (0);
    if (selectedDigitalMask != 0)
    {
        digitalPollStop.store (false);
        try
        {
            digitalPollThread = std::thread (&DataThreadPlugin::runDigitalPoller, this);
        }
        catch (const std::system_error& e)
        {
            digitalPollStop.store (true);
            LOGE ("MCDAQ: Cannot start digital input polling thread: ", e.what());
            acquisition->stop();
            acquisition.reset();
            return false;
        }
        const auto ulStartCallMs = std::chrono::duration<double, std::milli> (
            analogStartEnd - analogStartBegin).count();
        LOGC ("MCDAQ: DI0-DI7 input monitoring ACTIVE (mask=0x",
              String::toHexString (static_cast<int> (selectedDigitalMask)),
              "). Target poll interval 2 ms, SYSTEM-PACED, NOT synchronous ",
              "with analog. Short pulses may be missed; event times are approximate. ",
              "DI host/ADC anchor=scan-start return; start-call duration=",
              String (ulStartCallMs, 3), " ms; no extra startup UL status calls.");
    }

    startThread();

    LOGC ("MCDAQ DEBUG 6: DataThread started");

    return true;
}


bool DataThreadPlugin::updateBuffer()
{
    if (! acquisition)
        return false;

    // Wait for an MCC notification instead of polling every 20 ms.
    // Timeout also allows the DataThread to recheck its exit condition.
    acquisition->waitForData (100.0);

    if (threadShouldExit())
        return true;

    // Do not continue silently if an enabled digital channel's USB read
    // fails, if the poller is scheduled out for too long, or if transitions
    // cannot be represented in the stream.
    if (selectedDigitalMask != 0)
    {
        const auto d = digitalTimeline.getDiagnostics();
        const int readError = digitalPollError.load();
        // A 50ms blind spot is unacceptable even for our explicitly
        // low-precision, software-polled TTL mode. The ordinary poll
        // interval may still miss sub-interval pulses: never claim otherwise.
        if (readError != 0 || d.queueOverflows != 0 ||
            d.unrepresentableTransitions != 0 ||
            digitalMaxPollGapUs.load() > 50000)
        {
            LOGE ("MCDAQ: Digital input integrity failure: UL error=", readError,
                  " | Queue overruns=", d.queueOverflows,
                  " | Unrepresentable edges=", d.unrepresentableTransitions,
                  " | Max poll gap (us)=", digitalMaxPollGapUs.load(),
                  ". Stop acquisition and inspect wiring/poll settings.");
            return false;
        }
    }

    if (! acquisition->consumeAvailable() || ! acquisition->checkWatchdog())
    {
        const auto stats =
            acquisition->getStatistics();

        LOGE ("MCDAQ: Acquisition integrity failure.");
        LOGE ("MCDAQ: Scan error: ", stats.scanError);
        LOGE ("MCDAQ: Read error: ", stats.readError);
        LOGE ("MCDAQ: Possible data loss: ",
              stats.possibleDataLoss);
        LOGE ("MCDAQ: Watchdog fault: ",
              static_cast<int> (stats.watchdogFault),
              " | Scan error event: ", stats.scanErrorEventSeen,
              " | Unexpected scan end: ", stats.unexpectedEndEventSeen);
        LOGE ("MCDAQ: Last watchdog driver status/count: ",
              stats.watchdogDriverStatus, " / ", stats.watchdogDriverCount,
              " | UL query error: ", stats.watchdogStatusQueryError,
              " | Driver-callback lag: ", stats.watchdogNotificationLag);

        return false;
    }

    #if MCDAQ_VERBOSE_ACQUISITION_LOGS
    // Approximately once per second.
    if (++diagnosticTicks >= 50)
    {
        diagnosticTicks = 0;

        const auto stats =
            acquisition->getStatistics();

        const auto driver =
            acquisition->getDriverStatus();

        LOGC ("MCDAQ: Produced: ",
              stats.totalSamplesProduced,
              " | Consumed: ",
              stats.totalSamplesConsumed,
              " | Delivered: ",
              stats.totalSamplesDelivered,
              " | Callback count: ",
              stats.lastSampleCount);

        if (driver.error == 0)
        {
            LOGC ("MCDAQ: UL status: ",
                  driver.status,
                  " | CurCount: ",
                  driver.currentCount,
                  " | CurIndex: ",
                  driver.currentIndex);
        }
        else
        {
            LOGE ("MCDAQ: cbGetStatus failed: ",
                  driver.error);
        }
    }
    #endif

    return true;
}

bool DataThreadPlugin::stopAcquisition()
{
    if (isThreadRunning())
    {
        signalThreadShouldExit();

        // Do not wait for the next MCC notification to exit promptly.
        if (acquisition)
            acquisition->wakeWorker();

        if (! waitForThreadToExit (2000))
        {
            LOGE ("MCDAQ: Acquisition worker did not exit.");
            return false;
        }
    }


    // Stop the independent AUXPORT USB reader before the analog final drain.
    stopDigitalPoller();
    if (selectedDigitalMask != 0)
    {
        const auto clock = digitalClockEstimator.diagnostics();
        LOGC ("MCDAQ: DI online clock fit: callback samples=", clock.callbacksReceived,
              " | Decimated observations=", clock.acceptedObservations,
              " | Accepted fits=", clock.acceptedFits,
              " | Rejected fits=", clock.rejectedFits,
              " | Fit span (s)=", String (clock.fitSpanSeconds, 1),
              " | Fitted correction (ppm)=", String (clock.estimatedCorrectionPpm, 3),
              " | Applied correction (ppm)=", String (clock.appliedCorrectionPpm, 3),
              " | Slope uncertainty (ppm)=", String (clock.fitUncertaintyPpm, 3),
              " | Calibrated=", clock.calibrated,
              " | Fallbacks=", clock.fallbackCount,
              ". Analog timestamps and metadata UNCHANGED.");
        const auto d = digitalTimeline.getDiagnostics();
        LOGC ("MCDAQ: DI polling: reads=", digitalPollCount.load(),
              " | Max inter-read gap (us)=", digitalMaxPollGapUs.load(),
              " | Transitions observed=", d.transitionsObserved,
              " | Transitions mapped=", d.transitionsEmitted,
              " | Late placements=", d.lateTransitions,
              " | Pending at stop=", static_cast<int> (d.queuedTransitions),
              " | Collisions=", d.unrepresentableTransitions,
              " | Overflows=", d.queueOverflows,
              " | UL read error=", digitalPollError.load(),
              " | Mean edge observation window (us)=",
              String (d.meanEdgeObservationWindowUs, 1),
              " | Max edge observation window (us)=",
              d.maxEdgeObservationWindowUs);
    }

    if (acquisition)
    {
        acquisition->stop();

        // Retrieve statistics AFTER the final drain.
        const auto stopped =
            acquisition->getStopDiagnostics();

        const auto stats =
            acquisition->getStatistics();

        LOGC ("MCDAQ: Stop error: ",
              stopped.stopError,
              " | Disable-event error: ",
              stopped.disableEventError);

        LOGC ("MCDAQ: Post-stop CurCount: ",
              stopped.currentCount,
              " | CurIndex: ",
              stopped.currentIndex);

        LOGC ("MCDAQ: Final Produced: ",
              stopped.finalProduced,
              " | Final Consumed: ",
              stopped.finalConsumed,
              " | Final Delivered: ",
              stats.totalSamplesDelivered);

        LOGC ("MCDAQ: Final drain attempted: ",
              stopped.drainAttempted,
              " | Successful: ",
              stopped.drainSucceeded);

        LOGC ("MCDAQ: Scan error: ",
              stats.scanError,
              " | Read error: ",
              stats.readError,
              " | Possible data loss: ",
              stats.possibleDataLoss);

        LOGC ("MCDAQ: Channels: ", stats.channelCount,
              " | Final frames delivered: ",
              stats.channelCount > 0 ? stats.totalSamplesDelivered / stats.channelCount : 0,
              " | Trailing partial ADC points: ", stats.trailingPartialPoints);

        if (stats.trailingPartialPoints > 0)
            LOGC ("MCDAQ WARNING: UL stopped with ", stats.trailingPartialPoints,
                  " point(s) in an incomplete terminal ", stats.channelCount,
                  "-channel scan. These points were NOT written as a frame; ",
                  "complete-frame delivery is checked separately.");

        LOGC ("MCDAQ: Maximum unread backlog: ",
              stats.maxUnreadSamples,
              " ADC sample points");

        LOGC ("MCDAQ: Watchdog checks: ",
              stats.watchdogChecks,
              " | Fault: ", static_cast<int> (stats.watchdogFault),
              " | Driver status/count: ",
              stats.watchdogDriverStatus, " / ",
              stats.watchdogDriverCount);

        // If UL could not stop or disconnect callbacks,
        // keep the acquisition object for error recovery.
        if (stopped.stopError != 0 || stopped.disableEventError != 0)
        {
            LOGE ("MCDAQ: Hardware shutdown incomplete.");
            return false;
        }

        // A valid trailing terminal fragment is NOT an incomplete delivered
        // frame. Reconcile every ADC point and verify the complete frames
        // separately. Do not require the hardware to stop on a frame edge.
        const auto digitalSummary = digitalTimeline.getDiagnostics();
        const bool digitalIntegrityVerified = selectedDigitalMask == 0 ||
            (digitalPollError.load() == 0 &&
             digitalSummary.queueOverflows == 0 &&
             digitalSummary.unrepresentableTransitions == 0 &&
             digitalMaxPollGapUs.load() <= 50000);
        const bool completeFramesVerified =
            digitalIntegrityVerified &&
            stopped.statusError == 0 && stopped.status == 0 &&
            stopped.drainSucceeded &&
            stats.channelCount > 0 &&
            stats.trailingPartialPoints < static_cast<uint64_t> (stats.channelCount) &&
            stopped.finalConsumed % static_cast<uint64_t> (stats.channelCount) == 0 &&
            stopped.finalProduced == stopped.finalConsumed + stats.trailingPartialPoints &&
            stopped.finalConsumed == stats.totalSamplesDelivered &&
            stats.scanError == 0 && stats.readError == 0 &&
            ! stats.possibleDataLoss &&
            stats.watchdogFault == MCDAQAcquisition::WatchdogFault::None;

        if (! completeFramesVerified)
        {
            LOGC ("MCDAQ: Acquisition integrity not verified (analog and/or DI).");
        }
        else if (stats.trailingPartialPoints > 0)
        {
            LOGC ("MCDAQ: All complete frames reconciled; terminal partial scan separately accounted for.");
        }

        acquisition.reset();

        return completeFramesVerified;
    }

    return true;
}

void DataThreadPlugin::resizeBuffers()
{
    sourceBuffers.clear();

    if (foundInputSource())
    {
        // A minimum of 10,000 frames, or four seconds at the chosen rate.
        // Separate from the MCC driver-owned circular buffer.
        const int frames = (std::max) (
            10000, static_cast<int> (getDeclaredSampleRateHz() * 4));
        sourceBuffers.add (new DataBuffer (selectedChannelCount, frames));
    }
}

std::unique_ptr<GenericEditor> DataThreadPlugin::createEditor (SourceNode* sn)
{
    std::unique_ptr<DataThreadPluginEditor> editor = std::make_unique<DataThreadPluginEditor> (sn, this);

    return editor;
}

void DataThreadPlugin::handleBroadcastMessage (const String& msg, const int64 messageTimestmpMilliseconds)
{
}

String DataThreadPlugin::handleConfigMessage (const String& msg)
{
    return "";
}

void DataThreadPlugin::registerParameters()
{
    // Register parameters for the plugin here (e.g. addParameter())
}

void DataThreadPlugin::parameterValueChanged (Parameter* parameter)
{
    // Handle parameter value changes here (e.g. update settings)
}


std::size_t DataThreadPlugin::receiveRawBlock (
    const uint16_t* samples,
    std::size_t count,
    uint64_t firstSample,
    void* userData)
{
    auto* plugin =
        static_cast<DataThreadPlugin*> (userData);

    if (plugin == nullptr)
        return 0;

    return plugin->forwardRawBlock (
        samples,
        count,
        firstSample);
}

std::size_t DataThreadPlugin::forwardRawBlock (
    const uint16_t* samples,
    std::size_t count,
    uint64_t firstSample)
{
    const size_t n = static_cast<size_t> (selectedChannelCount);
    if (samples == nullptr || n == 0 || count == 0 || count > BLOCK_SIZE ||
        count % n != 0 || firstSample % n != 0 || sourceBuffers.size() == 0)
        return 0;

    // UL layout follows the configured ascending channel queue.
    // Example mask AI0+AI6+AI7 -> [AI0,AI6,AI7,AI0,AI6,AI7,...].
    // Open Ephys v1.x expects channel-major blocks:
    // [all AI0 frames, all AI1 frames, ...].
    const size_t frames = count / n;
    const uint64_t firstFrame = firstSample / n;
    const bool is12Bit = deviceManager->getSelectedProductName() == "USB-204";
    const int resolution = is12Bit ? 4096 : 65536;
    const int midpoint = resolution / 2;
    const float voltsPerCount =
        (2.0f * (is12Bit ? 10.0f : static_cast<float> (selectedRangeVolts))) /
        static_cast<float> (resolution);
    const double sampleRate = activeSampleRateHz;

    // Fill the TTL state word from separately captured AUXPORT transitions.
    // SourceNode generates actual TTL edge events from changes in this word.
    // Mask 0 retains exactly the previous all-zero analog-only behavior.
    if (selectedDigitalMask != 0)
        digitalTimeline.fill (firstFrame, frames, eventCodes.data());
    else
        std::fill_n (eventCodes.begin(), frames, std::uint64_t { 0 });

    for (size_t frame = 0; frame < frames; ++frame)
    {
        const int64_t sampleNumber = static_cast<int64_t> (firstFrame + frame);
        sampleNumbers[frame] = sampleNumber;
        timestamps[frame] = static_cast<double> (sampleNumber) / sampleRate;

        for (size_t channel = 0; channel < n; ++channel)
        {
            const int centeredCode =
                static_cast<int> (samples[frame * n + channel]) - midpoint;
            voltageBlock[channel * frames + frame] =
                static_cast<float> (centeredCode) * voltsPerCount;
        }
    }

    const auto accepted = sourceBuffers[0]->addToBuffer (
        voltageBlock.data(), sampleNumbers.data(), timestamps.data(),
        eventCodes.data(), static_cast<int> (frames));

    if (accepted < 0 || static_cast<size_t> (accepted) != frames)
        return 0; // The acquisition engine latches this as a delivery failure.

    return count; // Report accepted ADC points, not frames.
}


void DataThreadPlugin::runDigitalPoller() noexcept
{
    using Clock = MCDAQDigitalTimeline::Clock;
    auto previousReadTime = Clock::now();
    std::uint64_t lastClockObservationSeq = 0;
    bool firstRead = true;
    while (! digitalPollStop.load())
    {
        // Read ONLY the atomic callback mailbox: no MCC calls, no locks,
        // no work in the analog consumer, no changes to ADC samples.
        MCDAQAcquisition::TimingObservation clockObservation;
        if (acquisition != nullptr &&
            acquisition->latestTimingObservation (clockObservation) &&
            clockObservation.sequence != lastClockObservationSeq)
        {
            lastClockObservationSeq = clockObservation.sequence;
            double correctedDigitalRate = 0.0;
            if (digitalClockEstimator.observeCallback (
                    clockObservation.aggregatePointCount,
                    clockObservation.receivedAt, correctedDigitalRate))
                digitalTimeline.updateRelativeClockRate (
                    Clock::now(), correctedDigitalRate);
        }
        double fallbackRate = 0.0;
        const auto now = Clock::now();
        if (digitalClockEstimator.maintainFallback (now, fallbackRate))
            digitalTimeline.updateRelativeClockRate (now, fallbackRate);

        std::uint8_t port = 0;
        const auto readBegin = Clock::now();
        const int err = deviceManager->readSelectedDigitalPort (port);
        const auto readTime = Clock::now();
        if (err != 0)
        {
            digitalPollError.store (err);
            break; // An enabled DI channel must never fail silently.
        }

        if (! firstRead)
        {
            const auto gap = std::chrono::duration_cast<std::chrono::microseconds> (
                readTime - previousReadTime).count();
            if (gap > 0)
            {
                const auto gapUs = static_cast<std::uint64_t> (gap);
                auto currentMax = digitalMaxPollGapUs.load();
                while (gapUs > currentMax &&
                       ! digitalMaxPollGapUs.compare_exchange_weak (currentMax, gapUs)) {}
            }
        }
        firstRead = false;
        previousReadTime = readTime;
        ++digitalPollCount;
        digitalTimeline.observe (port, readBegin, readTime);

        // Windows and USB round-trip latency determine the actual poll rate.
        // This target is not a hardware acquisition clock.
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
}

void DataThreadPlugin::stopDigitalPoller() noexcept
{
    digitalPollStop.store (true);
    if (digitalPollThread.joinable())
        digitalPollThread.join();
}
