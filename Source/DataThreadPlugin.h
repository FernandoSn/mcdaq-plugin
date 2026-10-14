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

#ifndef DATATHREADPLUGIN_H_DEFINED
#define DATATHREADPLUGIN_H_DEFINED

#include <DataThreadHeaders.h>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include "MCDAQDigitalTimeline.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class MCDAQDeviceManager;

class MCDAQAcquisition;

class DataThreadPlugin : public DataThread
{
public:
    /** The class constructor, used to initialize any members. */
    DataThreadPlugin (SourceNode* sn);

    /** The class destructor, used to deallocate memory */
    ~DataThreadPlugin();

    // ------------------------------------------------------------
    //                  PURE VIRTUAL METHODS
    //     (must be implemented by all DataThreads)
    // ------------------------------------------------------------

    /** Returns true if the data source is connected, false otherwise.*/
    bool foundInputSource();

    // Sampling rate is per channel; hardware limits depend on board and N.
    long getSampleRateHz() const noexcept { return selectedSampleRateHz; }
    // Precise nominal programmed hardware clock (Hz/channel).
    // Valid only after successful UL preflight for the selected configuration.
    double getDeclaredSampleRateHz() const;
    bool setSampleRateHz (long rateHz);
    int getChannelCount() const noexcept { return selectedChannelCount; }
    // Capabilities of the currently selected board (not enabled-channel counts).
    int getAnalogInputCount() const;
    int getDigitalInputCount() const;
    std::uint8_t getChannelMask() const noexcept { return selectedChannelMask; }
    std::uint8_t getDigitalMask() const noexcept { return selectedDigitalMask; }
    bool isDigitalEnabled (int channel) const noexcept;
    bool setDigitalMask (std::uint8_t mask);
    bool isChannelEnabled (int channel) const noexcept;
    std::vector<int> getSelectedChannels() const;
    bool setChannelMask (std::uint8_t mask);
    // Input range is one shared setting, except USB-204 is fixed at +/-10 V.
    int getVoltageRangeVolts() const noexcept { return selectedRangeVolts; }
    bool canSelectVoltageRange() const;
    bool setVoltageRangeVolts (int fullScaleVolts);
    std::string getBoardName() const;
    std::vector<long> getAvailableRates() const;
    bool isSimultaneousSampling() const;
    std::string getSamplingModeDescription() const;

    /** Passes the processor's info objects to DataThread, to allow them to be configured */
    void updateSettings (OwnedArray<ContinuousChannel>* continuousChannels,
                         OwnedArray<EventChannel>* eventChannels,
                         OwnedArray<SpikeChannel>* spikeChannels,
                         OwnedArray<DataStream>* sourceStreams,
                         OwnedArray<DeviceInfo>* devices,
                         OwnedArray<ConfigurationObject>* configurationObjects);

    /** Initializes data transfer.*/
    bool startAcquisition();

    /** Called repeatedly to add any available data to the buffer */
    bool updateBuffer();

    /** Stops data transfer.*/
    bool stopAcquisition();

    // ------------------------------------------------------------
    //                   VIRTUAL METHODS
    //       (can optionally be overriden by sub-classes)
    // ------------------------------------------------------------

    /** Called when the chain updates, to add, remove or resize the sourceBuffers' DataBuffers as needed */
    void resizeBuffers() override;

    /** Create the DataThread custom editor */
    std::unique_ptr<GenericEditor> createEditor (SourceNode* sn) override;

    /** Allows the DataThread plugin to respond to messages sent by other processors */
    void handleBroadcastMessage (const String& msg, const int64 messageTimeMilliseconds) override;

    /** Allows the DataThread plugin to handle a config message while acquisition is not active */
    String handleConfigMessage (const String& msg) override;

    /** Registers parameters to the DataThread */
    void registerParameters() override;

    /** Called when a parameter value is updated, to allow plugin-specific responses */    
    void parameterValueChanged (Parameter* parameter) override;

private:
    std::unique_ptr<MCDAQDeviceManager> deviceManager;
    std::unique_ptr<MCDAQAcquisition> acquisition;
    unsigned int diagnosticTicks = 0;
    long selectedSampleRateHz = 1000;
    int selectedChannelCount = 1;
    std::uint8_t selectedChannelMask = 0x01; // AI0 initially enabled.
    std::uint8_t selectedDigitalMask = 0x00; // All DI inputs opt-in.
    int selectedRangeVolts = 10;
    double activeSampleRateHz = 1000.0;

    // Cache both the MCC-reported integer rate and the divider-derived
    // nominal clock for the exact current board, queue, range and request.
    // The integer UL rate validates preflight vs. actual scan; the precise
    // rate is used for OE metadata and sample timestamps.
    bool rateNegotiated = false;
    long negotiatedRateHz = 0; // UL-reported, for start consistency check
    double negotiatedPreciseRateHz = 0.0; // nominal timer-derived
    std::uint32_t negotiatedClockDivider = 0;
    long negotiatedRequestHz = 0;
    std::uint8_t negotiatedChannelMask = 0;
    int negotiatedRangeVolts = 0;
    int negotiatedBoardNum = -1;
    bool negotiateSelectedRate();


    static constexpr std::size_t BLOCK_SIZE = 4096;

    std::array<float, BLOCK_SIZE> voltageBlock {};
    std::array<int64_t, BLOCK_SIZE> sampleNumbers {};
    std::array<double, BLOCK_SIZE> timestamps {};
    std::array<uint64_t, BLOCK_SIZE> eventCodes {};

    // DIO is software-polled, NOT ADC-clock-synchronous on these two boards.
    // The dedicated worker never touches the analog ring buffers.
    MCDAQDigitalTimeline digitalTimeline;
    std::thread digitalPollThread;
    std::atomic<bool> digitalPollStop { true };
    std::atomic<int> digitalPollError { 0 };
    std::atomic<std::uint64_t> digitalPollCount { 0 };
    std::atomic<std::uint64_t> digitalMaxPollGapUs { 0 };
    void runDigitalPoller() noexcept;
    void stopDigitalPoller() noexcept;

    // Layout: consecutive frames for AI0, then AI1, etc. The output
    // array is 4096 ADC points regardless of selected channel count.
    // Called by MCDAQAcquisition's consumer,
    // not directly by the MCC event callback.
    static std::size_t receiveRawBlock (
        const uint16_t* samples,
        std::size_t count,
        uint64_t firstSample,
        void* userData);

    std::size_t forwardRawBlock (
        const uint16_t* samples,
        std::size_t count,
        uint64_t firstSample);


};

#endif