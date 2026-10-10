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
#include <array>
#include <cstddef>
#include <cstdint>

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

    // Single-channel test rates, set only while acquisition is stopped.
    long getSampleRateHz() const noexcept { return selectedSampleRateHz; }
    // Hardware clock rate advertised to Open Ephys before streaming begins.
    long getDeclaredSampleRateHz() const;
    bool setSampleRateHz (long rateHz);

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
    double activeSampleRateHz = 1000.0;

    
    static constexpr std::size_t BLOCK_SIZE = 4096;

    std::array<float, BLOCK_SIZE> voltageBlock {};
    std::array<int64_t, BLOCK_SIZE> sampleNumbers {};
    std::array<double, BLOCK_SIZE> timestamps {};
    std::array<uint64_t, BLOCK_SIZE> eventCodes {};

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