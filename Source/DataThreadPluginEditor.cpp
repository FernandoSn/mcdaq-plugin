#include "DataThreadPluginEditor.h"

#include <algorithm>

namespace
{
// NI-DAQmx-style channel control. The small round lamp shows whether an
// analog / digital input is selected. DIO is software-polled (not paced by ADC).
class MCDAQChannelButton final : public juce::TextButton
{
public:
    explicit MCDAQChannelButton (const juce::String& label)
        : juce::TextButton (label)
    {
        setButtonText (label);
        setClickingTogglesState (true);
    }

    void paintButton (juce::Graphics& g,
                      bool highlighted,
                      bool pressed) override
    {
        const bool available = isEnabled();
        const auto bounds = getLocalBounds().toFloat().reduced (0.6f);

        g.setColour (available
            ? (pressed ? juce::Colour (0xffadadad)
               : highlighted ? juce::Colour (0xffdedede)
                             : juce::Colour (0xffc9c9c9))
            : juce::Colour (0xff8f8f8f));
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (available ? juce::Colour (0xff4a4a4a)
                               : juce::Colour (0xff777777));
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

        g.setFont (juce::Font (11.0f));
        g.setColour (available ? juce::Colours::black
                               : juce::Colour (0xff555555));
        g.drawFittedText (getButtonText(),
                          7, 1, getWidth() - 27, getHeight() - 2,
                          juce::Justification::centredLeft, 1);

        constexpr float diameter = 12.0f;
        const float left = static_cast<float> (getWidth()) - diameter - 7.0f;
        const float top = (static_cast<float> (getHeight()) - diameter) * 0.5f;
        g.setColour (available && getToggleState()
            ? juce::Colour (0xff168b37) : juce::Colour (0xff777777));
        g.fillEllipse (left, top, diameter, diameter);
        g.setColour (available ? juce::Colour (0xff545454)
                               : juce::Colour (0xff858585));
        g.drawEllipse (left, top, diameter, diameter, 1.0f);
    }
};

constexpr int editorWidth = 445;
constexpr int firstRowY = 30;
constexpr int channelRowPitch = 23;
constexpr int channelButtonHeight = 19;

// AI0..AI3, AI4..AI7 | DI0..DI3, DI4..DI7 | device settings.
constexpr int analogFirstX = 9;
constexpr int analogSecondX = 68;
constexpr int digitalFirstX = 130;
constexpr int digitalSecondX = 182;
constexpr int analogButtonWidth = 54;
constexpr int digitalButtonWidth = 46;
constexpr int settingsX = 241;
constexpr int settingsWidth = 195;
// Keep the controls genuinely narrow: the exact-rate readout is separate
// from the requested-rate ComboBox, rather than covering its arrow.
constexpr int rateDropDownWidth = 108;
constexpr int voltageDropDownWidth = 149;
constexpr int diTimeOffsetX = 155;
constexpr int diTimeControlWidth = 40;
constexpr int exactRateOffsetX = 116;
constexpr int exactRateWidth = 77;
} // namespace

DataThreadPluginEditor::DataThreadPluginEditor (GenericProcessor* parentNode,
                                                DataThreadPlugin* plugin)
    : GenericEditor (parentNode), thread (plugin)
{
    // Compact NI-style control grouping; all components stay clear of the
    // orange title bar and the bottom edge of the GenericEditor.
    desiredWidth = editorWidth;

    for (int channel = 0; channel < 8; ++channel)
    {
        auto analog = std::make_unique<MCDAQChannelButton> (
            "AI" + juce::String (channel));
        analog->setBounds (channel < 4 ? analogFirstX : analogSecondX,
                           firstRowY + (channel % 4) * channelRowPitch,
                           analogButtonWidth, channelButtonHeight);
        analog->setTooltip ("Enable/disable analog input AI" +
                            juce::String (channel));
        analog->addListener (this);
        addAndMakeVisible (*analog);
        analogButtons[static_cast<std::size_t> (channel)] = std::move (analog);

        auto digital = std::make_unique<MCDAQChannelButton> (
            "DI" + juce::String (channel));
        digital->setBounds (channel < 4 ? digitalFirstX : digitalSecondX,
                            firstRowY + (channel % 4) * channelRowPitch,
                            digitalButtonWidth, channelButtonHeight);
        digital->setTooltip ("Enable/disable TTL input DI" +
            juce::String (channel) +
            ". SOFTWARE-POLLED, not synchronized to the analog clock; "
            "short pulses can be missed and event timestamps are approximate.");
        digital->addListener (this);
        addAndMakeVisible (*digital);
        digitalButtons[static_cast<std::size_t> (channel)] = std::move (digital);
    }

    boardLabel = std::make_unique<juce::Label>();
    boardLabel->setBounds (settingsX, 25, settingsWidth, 16);
    boardLabel->setFont (juce::Font (12.0f, juce::Font::bold));
    boardLabel->setColour (juce::Label::textColourId, juce::Colours::black);
    boardLabel->setMinimumHorizontalScale (0.85f);
    addAndMakeVisible (*boardLabel);

    sampleRateLabel = std::make_unique<juce::Label>();
    sampleRateLabel->setText ("SAMPLE RATE / CHANNEL", juce::dontSendNotification);
    sampleRateLabel->setBounds (settingsX, 41, settingsWidth, 13);
    sampleRateLabel->setFont (juce::Font (10.0f));
    addAndMakeVisible (*sampleRateLabel);

    sampleRateSelector = std::make_unique<juce::ComboBox> ("MCDAQ sample rate");
    sampleRateSelector->setBounds (settingsX + 2, 55, rateDropDownWidth, 22);
    sampleRateSelector->addListener (this);
    addAndMakeVisible (*sampleRateSelector);

    // Keep the requested rate in a genuinely narrower ComboBox and show the
    // divider-derived clock just to its right, e.g. 30000 Hz (30007.50).
    // This avoids covering the ComboBox arrow and lets the parentheses remain
    // easily readable at a smaller font than the selected rate.
    exactRateLabel = std::make_unique<juce::Label>();
    exactRateLabel->setBounds (settingsX + exactRateOffsetX, 57, exactRateWidth, 18);
    exactRateLabel->setFont (juce::Font (12.0f));
    exactRateLabel->setBorderSize (juce::BorderSize<int> (0));
    exactRateLabel->setColour (juce::Label::textColourId,
                               juce::Colour (0xff404040));
    exactRateLabel->setJustificationType (juce::Justification::centredLeft);
    exactRateLabel->setInterceptsMouseClicks (false, false);
    addAndMakeVisible (*exactRateLabel);

    voltageRangeLabel = std::make_unique<juce::Label>();
    voltageRangeLabel->setText ("AI VOLTAGE RANGE", juce::dontSendNotification);
    voltageRangeLabel->setBounds (settingsX, 78, settingsWidth, 12);
    voltageRangeLabel->setFont (juce::Font (10.0f));
    addAndMakeVisible (*voltageRangeLabel);

    voltageRangeSelector = std::make_unique<juce::ComboBox> ("MCDAQ voltage range");
    voltageRangeSelector->setBounds (settingsX + 2, 91, voltageDropDownWidth, 21);
    for (const int range : { 10, 5, 2, 1 })
        voltageRangeSelector->addItem (
            "-" + juce::String (range) + " to +" + juce::String (range) + " V", range);
    voltageRangeSelector->addListener (this);
    addAndMakeVisible (*voltageRangeSelector);

    // The existing compact settings row has 43 free pixels at its right edge.
    // A digital-only experiment: never changes the analog stream or USB calls.
    diTimeLabel = std::make_unique<juce::Label>();
    diTimeLabel->setText ("DI TIME", juce::dontSendNotification);
    diTimeLabel->setBounds (settingsX + diTimeOffsetX, 78, diTimeControlWidth, 12);
    diTimeLabel->setFont (juce::Font (8.0f));
    addAndMakeVisible (*diTimeLabel);

    diTimeButton = std::make_unique<juce::TextButton> ("MID");
    diTimeButton->setBounds (settingsX + diTimeOffsetX, 91, diTimeControlWidth, 21);
    diTimeButton->setTooltip (
        "MID: original v3 cbDIn call midpoint. END: experimental cbDIn "
        "completion time. Both use the midpoint BETWEEN digital observations. "
        "Select only while stopped; not a hardcoded millisecond correction.");
    diTimeButton->addListener (this);
    addAndMakeVisible (*diTimeButton);

    // Independent HOLD control on the architecture row; no overlap with
    // 40px DI TIME selector above, nor the channel buttons to the left.
    digitalHoldbackButton = std::make_unique<juce::TextButton> ("HOLD OFF");
    digitalHoldbackButton->setBounds (351, 112, 85, 13);
    digitalHoldbackButton->setTooltip (
        "EXPERIMENTAL: HOLD OFF sends ADC blocks directly to Open Ephys. "
        "HOLD 1ms queues native ADC blocks before delivery to give digital "
        "polls time to arrive, potentially eliminating late TTL placement. "
        "ADC sampling and timestamp mapping remain unchanged. Adds stream "
        "latency; select only while acquisition is stopped.");
    digitalHoldbackButton->addListener (this);
    addAndMakeVisible (*digitalHoldbackButton);

    architectureLabel = std::make_unique<juce::Label>();
    architectureLabel->setBounds (settingsX, 112, 106, 12);
    architectureLabel->setFont (juce::Font (9.0f));
    architectureLabel->setTooltip (
        "SIM = simultaneous ADCs; MUX = multiplexed ADC; DI is software-polled.");
    addAndMakeVisible (*architectureLabel);

    refreshControls();
}

DataThreadPluginEditor::~DataThreadPluginEditor()
{
    sampleRateSelector->removeListener (this);
    voltageRangeSelector->removeListener (this);
    diTimeButton->removeListener (this);
    digitalHoldbackButton->removeListener (this);
    for (auto& button : analogButtons)
        if (button) button->removeListener (this);
    for (auto& button : digitalButtons)
        if (button) button->removeListener (this);
}

void DataThreadPluginEditor::refreshControls()
{
    diTimeButton->setButtonText (
        thread->getDIObservationPhase() == DataThreadPlugin::DIObservationPhase::Midpoint
            ? "MID" : "END");
    diTimeButton->setEnabled (! CoreServices::getAcquisitionStatus());
    digitalHoldbackButton->setButtonText (
        thread->getDigitalHoldbackMs() == 0 ? "HOLD OFF" : "HOLD 1ms");
    digitalHoldbackButton->setEnabled (! CoreServices::getAcquisitionStatus());
    const int analogCount = (std::max) (0, (std::min) (8, thread->getAnalogInputCount()));
    const int digitalCount = (std::max) (0, (std::min) (8, thread->getDigitalInputCount()));
    for (int channel = 0; channel < 8; ++channel)
    {
        auto& analog = analogButtons[static_cast<std::size_t> (channel)];
        analog->setVisible (channel < analogCount);
        analog->setToggleState (channel < analogCount && thread->isChannelEnabled (channel),
                                juce::dontSendNotification);

        auto& digital = digitalButtons[static_cast<std::size_t> (channel)];
        digital->setVisible (channel < digitalCount);
        digital->setToggleState (channel < digitalCount && thread->isDigitalEnabled (channel),
                                 juce::dontSendNotification);
        digital->setEnabled (channel < digitalCount &&
                             ! CoreServices::getAcquisitionStatus());
    }

    sampleRateSelector->clear (juce::dontSendNotification);
    for (const long rate : thread->getAvailableRates())
        sampleRateSelector->addItem (juce::String (rate) + " Hz", static_cast<int> (rate));
    sampleRateSelector->setSelectedId (
        static_cast<int> (thread->getSampleRateHz()), juce::dontSendNotification);

    const double preciseRate = thread->getDeclaredSampleRateHz();
    const bool haveClock = preciseRate > 0.0;
    exactRateLabel->setText (haveClock ? "(" + juce::String (preciseRate, 2) + ")"
                                       : juce::String(),
                             juce::dontSendNotification);
    exactRateLabel->setTooltip (haveClock
        ? "Exact nominal clock from the hardware divider: " +
          juce::String (preciseRate, 6) + " Hz per channel"
        : "Hardware sampling-rate preflight has not completed");

    voltageRangeSelector->setSelectedId (
        thread->getVoltageRangeVolts(), juce::dontSendNotification);
    voltageRangeSelector->setEnabled (thread->canSelectVoltageRange());
    voltageRangeSelector->setTooltip (thread->canSelectVoltageRange()
        ? "Select the common range for all enabled USB-1608FS-Plus analog inputs"
        : "USB-204 analog input range is fixed at -10 to +10 V");

    const juce::String product (thread->getBoardName().c_str());
    boardLabel->setText (product, juce::dontSendNotification);
    architectureLabel->setText (
        juce::String (thread->isSimultaneousSampling() ? "SIM" : "MUX") +
        " | " + juce::String (analogCount) + "AI / " +
        juce::String (digitalCount) + "DI",
        juce::dontSendNotification);
}

void DataThreadPluginEditor::comboBoxChanged (juce::ComboBox* box)
{
    bool accepted = false;
    if (box == sampleRateSelector.get())
        accepted = thread->setSampleRateHz (sampleRateSelector->getSelectedId());
    else if (box == voltageRangeSelector.get())
        accepted = thread->setVoltageRangeVolts (voltageRangeSelector->getSelectedId());
    else
        return;

    refreshControls();
    if (accepted)
    {
        CoreServices::updateSignalChain (this);
        refreshControls(); // Display the negotiated precise clock.
    }
}

void DataThreadPluginEditor::buttonClicked (juce::Button* button)
{
    if (button == digitalHoldbackButton.get())
    {
        const int next = thread->getDigitalHoldbackMs() == 0
            ? MCDAQDigitalHoldback::experimentalDelayMs : 0;
        thread->setDigitalHoldbackMs (next);
        refreshControls(); // No signal-chain rebuild: output-only option.
        return;
    }
    if (button == diTimeButton.get())
    {
        const auto next = thread->getDIObservationPhase() ==
            DataThreadPlugin::DIObservationPhase::Midpoint
            ? DataThreadPlugin::DIObservationPhase::ReadEnd
            : DataThreadPlugin::DIObservationPhase::Midpoint;
        thread->setDIObservationPhase (next);
        refreshControls(); // No signal-chain rebuild for this digital-only choice.
        return;
    }
    for (int channel = 0; channel < 8; ++channel)
    {
        if (button != analogButtons[static_cast<std::size_t> (channel)].get())
            continue;
        const auto nextMask = static_cast<std::uint8_t> (
            thread->getChannelMask() ^ static_cast<std::uint8_t> (1u << channel));
        const bool accepted = thread->setChannelMask (nextMask);
        refreshControls(); // Cannot disable the final AI channel.
        if (accepted)
        {
            CoreServices::updateSignalChain (this);
            refreshControls();
        }
        return;
    }
    for (int channel = 0; channel < 8; ++channel)
    {
        if (button != digitalButtons[static_cast<std::size_t> (channel)].get())
            continue;
        const auto nextMask = static_cast<std::uint8_t> (
            thread->getDigitalMask() ^ static_cast<std::uint8_t> (1u << channel));
        const bool accepted = thread->setDigitalMask (nextMask);
        refreshControls();
        if (accepted)
        {
            CoreServices::updateSignalChain (this);
            refreshControls();
        }
        return;
    }
}

void DataThreadPluginEditor::saveCustomParametersToXml (XmlElement* xml)
{
    if (! xml) return;
    xml->setAttribute ("sample_rate_hz", static_cast<int> (thread->getSampleRateHz()));
    xml->setAttribute ("ai_channel_mask", static_cast<int> (thread->getChannelMask()));
    xml->setAttribute ("di_channel_mask", static_cast<int> (thread->getDigitalMask()));
    xml->setAttribute ("di_observation_phase", static_cast<int> (thread->getDIObservationPhase()));
    xml->setAttribute ("di_holdback_ms", thread->getDigitalHoldbackMs());
    xml->setAttribute ("input_range_volts", thread->getVoltageRangeVolts());
}

void DataThreadPluginEditor::loadCustomParametersFromXml (XmlElement* xml)
{
    if (! xml) return;
    // Backward compatibility with older configs containing channel_count.
    int channelMask = xml->getIntAttribute ("ai_channel_mask", -1);
    if (channelMask < 0)
    {
        const int previousCount = xml->getIntAttribute ("channel_count", 1);
        channelMask = (previousCount >= 1 && previousCount <= 8)
            ? ((1 << previousCount) - 1) : 1;
    }
    thread->setChannelMask (static_cast<std::uint8_t> (channelMask));
    thread->setDigitalMask (static_cast<std::uint8_t> (
        xml->getIntAttribute ("di_channel_mask", 0)));
    const int holdbackMs = xml->getIntAttribute ("di_holdback_ms", 0);
    thread->setDigitalHoldbackMs (
        holdbackMs == MCDAQDigitalHoldback::experimentalDelayMs ? holdbackMs : 0);
    const int phase = xml->getIntAttribute ("di_observation_phase", 0);
    thread->setDIObservationPhase (
        phase == 1 ? DataThreadPlugin::DIObservationPhase::ReadEnd
                   : DataThreadPlugin::DIObservationPhase::Midpoint);
    thread->setSampleRateHz (xml->getIntAttribute ("sample_rate_hz", 1000));
    thread->setVoltageRangeVolts (xml->getIntAttribute ("input_range_volts", 10));
    refreshControls();
}
