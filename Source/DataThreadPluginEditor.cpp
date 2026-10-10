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

#include "DataThreadPluginEditor.h"

DataThreadPluginEditor::DataThreadPluginEditor (GenericProcessor* parentNode, DataThreadPlugin* plugin)
    : GenericEditor (parentNode)
{
    desiredWidth = 170;
    this->thread = plugin;

    sampleRateLabel = std::make_unique<juce::Label>();
    sampleRateLabel->setText ("Rate (Hz)", juce::dontSendNotification);
    sampleRateLabel->setBounds (12, 28, 145, 18);
    addAndMakeVisible (*sampleRateLabel);

    sampleRateSelector = std::make_unique<juce::ComboBox> ("MCDAQ sample rate");
    for (const int rate : { 1000, 2000, 5000, 10000 })
        sampleRateSelector->addItem (juce::String (rate), rate);
    sampleRateSelector->addItem ("30000 (nominal)", 30000);
    sampleRateSelector->setSelectedId (
        static_cast<int> (thread->getSampleRateHz()), juce::dontSendNotification);
    sampleRateSelector->setBounds (12, 50, 140, 22);
    sampleRateSelector->addListener (this);
    addAndMakeVisible (*sampleRateSelector);
}

DataThreadPluginEditor::~DataThreadPluginEditor()
{
    sampleRateSelector->removeListener (this);
}

void DataThreadPluginEditor::comboBoxChanged (juce::ComboBox* box)
{
    if (box != sampleRateSelector.get())
        return;

    if (thread->setSampleRateHz (sampleRateSelector->getSelectedId()))
    {
        // Propagate the new rate to the Record Node, viewer, and buffers.
        CoreServices::updateSignalChain (this);
    }
    else
    {
        // Don't show a clock setting that MCDAQ refused to apply.
        sampleRateSelector->setSelectedId (
            static_cast<int> (thread->getSampleRateHz()),
            juce::dontSendNotification);
    }
}

void DataThreadPluginEditor::saveCustomParametersToXml (XmlElement* xml)
{
    if (xml != nullptr)
        xml->setAttribute ("sample_rate_hz", static_cast<int> (thread->getSampleRateHz()));
}

void DataThreadPluginEditor::loadCustomParametersFromXml (XmlElement* xml)
{
    if (xml == nullptr)
        return;

    const int rate = xml->getIntAttribute ("sample_rate_hz", 1000);
    thread->setSampleRateHz (rate);
    sampleRateSelector->setSelectedId (
        static_cast<int> (thread->getSampleRateHz()), juce::dontSendNotification);
}