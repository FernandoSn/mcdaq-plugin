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
        const auto lampColour = getToggleState()
            ? (available ? juce::Colour (0xff168b37)
                         : juce::Colour (0xff8fcf9f))
            : juce::Colour (0xff777777);
        g.setColour (lampColour);
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
constexpr int settingsTopOffset = 3;
// Keep the controls genuinely narrow: the exact-rate readout is separate
// from the requested-rate ComboBox, rather than covering its arrow.
constexpr int rateDropDownWidth = 104;
constexpr int voltageDropDownWidth = 145;
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
                             juce::String (channel));
        digital->addListener (this);
        addAndMakeVisible (*digital);
        digitalButtons[static_cast<std::size_t> (channel)] = std::move (digital);
    }

    boardSelector = std::make_unique<juce::ComboBox> ("MCDAQ physical board");
    boardSelector->setBounds (settingsX, 25 + settingsTopOffset, settingsWidth, 18);
    boardSelector->setTextWhenNothingSelected ("Select MCC board...");
    boardSelector->setTooltip ("Choose the physical MCC USB device. "
        "Only change it while acquisition is stopped; the unique ID is saved with the configuration.");
    boardSelector->addListener (this);
    addAndMakeVisible (*boardSelector);

    sampleRateLabel = std::make_unique<juce::Label>();
    sampleRateLabel->setText ("SAMPLE RATE / CHANNEL", juce::dontSendNotification);
    sampleRateLabel->setBounds (settingsX, 46 + settingsTopOffset, settingsWidth, 13);
    sampleRateLabel->setFont (juce::Font (10.0f));
    addAndMakeVisible (*sampleRateLabel);

    sampleRateSelector = std::make_unique<juce::ComboBox> ("MCDAQ sample rate");
    sampleRateSelector->setBounds (settingsX + 2, 58 + settingsTopOffset, rateDropDownWidth, 20);
    sampleRateSelector->addListener (this);
    addAndMakeVisible (*sampleRateSelector);

    // Keep the requested rate in a genuinely narrower ComboBox and show the
    // divider-derived clock just to its right, e.g. 30000 Hz (30007.50).
    // This avoids covering the ComboBox arrow and lets the parentheses remain
    // easily readable at a smaller font than the selected rate.
    exactRateLabel = std::make_unique<juce::Label>();
    exactRateLabel->setBounds (settingsX + exactRateOffsetX, 59 + settingsTopOffset, exactRateWidth, 17);
    exactRateLabel->setFont (juce::Font (12.0f));
    exactRateLabel->setBorderSize (juce::BorderSize<int> (0));
    exactRateLabel->setColour (juce::Label::textColourId,
                               juce::Colour (0xff404040));
    exactRateLabel->setJustificationType (juce::Justification::centredLeft);
    exactRateLabel->setInterceptsMouseClicks (false, false);
    addAndMakeVisible (*exactRateLabel);

    voltageRangeLabel = std::make_unique<juce::Label>();
    voltageRangeLabel->setText ("AI VOLTAGE RANGE", juce::dontSendNotification);
    voltageRangeLabel->setBounds (settingsX, 82 + settingsTopOffset, settingsWidth, 12);
    voltageRangeLabel->setFont (juce::Font (10.0f));
    addAndMakeVisible (*voltageRangeLabel);

    voltageRangeSelector = std::make_unique<juce::ComboBox> ("MCDAQ voltage range");
    voltageRangeSelector->setBounds (settingsX + 2, 93 + settingsTopOffset, voltageDropDownWidth, 20);
    for (const int range : { 10, 5, 2, 1 })
        voltageRangeSelector->addItem (
            "-" + juce::String (range) + " to +" + juce::String (range) + " V", range);
    voltageRangeSelector->addListener (this);
    addAndMakeVisible (*voltageRangeSelector);

    architectureLabel = std::make_unique<juce::Label>();
    architectureLabel->setBounds (settingsX, 113 + settingsTopOffset, 106, 12);
    architectureLabel->setFont (juce::Font (10.0f));
    architectureLabel->setTooltip (
        "SIM = simultaneous ADCs; MUX = multiplexed ADC.");
    addAndMakeVisible (*architectureLabel);

    refreshControls();
    activityTimer.start(); // React to Open Ephys Play/Record state changes.
}

DataThreadPluginEditor::~DataThreadPluginEditor()
{
    activityTimer.stop();
    boardSelector->removeListener (this);
    sampleRateSelector->removeListener (this);
    voltageRangeSelector->removeListener (this);
    for (auto& button : analogButtons)
        if (button) button->removeListener (this);
    for (auto& button : digitalButtons)
        if (button) button->removeListener (this);
}

bool DataThreadPluginEditor::isAcquiringOrRecording() const
{
    return CoreServices::getAcquisitionStatus() ||
           CoreServices::getRecordingStatus();
}

void DataThreadPluginEditor::ActivityTimer::timerCallback()
{
    const bool acquiringOrRecording = editor.isAcquiringOrRecording();
    if (! acquiringOrRecording)
    {
        // Another MCDAQ instance may have been deleted, created, or switched
        // since this editor last rebuilt its board list. Read only UL's live
        // registration status; do not rescan USB or alter any reservation.
        editor.retryPendingBoardRestoration();
        editor.refreshBoardAvailability();
    }

    if (acquiringOrRecording != editor.controlsLocked)
        editor.updateControlAvailability();
}

void DataThreadPluginEditor::retryPendingBoardRestoration()
{
    if (unresolvedSavedBoardId.empty() || thread == nullptr)
        return;

    // MCC board lookup is unnecessary every 100 ms. Also throttle failed
    // registration retries if a device reports free but UL refuses to open it.
    if (++restoreRetryTicks < 5)
        return;
    restoreRetryTicks = 0;

    bool available = false;
    for (const auto& board : thread->getBoardChoices())
        if (board.uniqueId == unresolvedSavedBoardId && board.selectable)
        {
            available = true;
            break;
        }
    if (! available)
        return;

    if (! thread->restoreBoardByUniqueId (unresolvedSavedBoardId))
        return; // Keep the saved ID and try again later; never substitute another board.

    unresolvedSavedBoardId.clear();
    applySavedSettings (pendingSavedSettings.analogMask,
                        pendingSavedSettings.digitalMask,
                        pendingSavedSettings.sampleRateHz,
                        pendingSavedSettings.inputRangeVolts);
    refreshControls();
    CoreServices::updateSignalChain (this);
    refreshControls(); // Refresh actual board mode and negotiated sample rate.
}

void DataThreadPluginEditor::applySavedSettings (int analogMask, int digitalMask,
                                                  int sampleRateHz, int inputRangeVolts)
{
    // Restoring the same stream attributes is essential after a deferred
    // board assignment: setters reject changes when no supported board exists.
    thread->setChannelMask (static_cast<std::uint8_t> (analogMask));
    thread->setDigitalMask (static_cast<std::uint8_t> (digitalMask));
    thread->setSampleRateHz (sampleRateHz);
    thread->setVoltageRangeVolts (inputRangeVolts);
}

void DataThreadPluginEditor::updateControlAvailability()
{
    controlsLocked = isAcquiringOrRecording();
    const bool editable = ! controlsLocked;

    // Lock *all* configurable controls, never just the board selector.
    boardSelector->setEnabled (editable);
    sampleRateSelector->setEnabled (editable);
    voltageRangeSelector->setEnabled (editable && thread->canSelectVoltageRange());

    const int analogCount = (std::max) (0, (std::min) (8, thread->getAnalogInputCount()));
    const int digitalCount = (std::max) (0, (std::min) (8, thread->getDigitalInputCount()));
    for (int channel = 0; channel < 8; ++channel)
    {
        analogButtons[static_cast<std::size_t> (channel)]->setEnabled (
            editable && channel < analogCount);
        digitalButtons[static_cast<std::size_t> (channel)]->setEnabled (
            editable && channel < digitalCount);
    }
}

void DataThreadPluginEditor::refreshBoardSelector()
{
    // Keep occupied boards in the list. Selection is always validated by the
    // device manager; these labels are informational snapshots only.
    displayedBoardChoices = thread->getBoardChoices();
    boardSelector->clear (juce::dontSendNotification);
    boardChoiceIds.clear();
    const auto selectedId = thread->getSelectedBoardUniqueId();
    int selectedComboId = 0;
    for (const auto& choice : displayedBoardChoices)
    {
        boardChoiceIds.push_back (choice.uniqueId);
        const int comboId = static_cast<int> (boardChoiceIds.size());
        juce::String title (choice.name.c_str());
        title += " [" + juce::String (choice.uniqueId.c_str()) + "]";
        if (! choice.selectable) title += " (in use)";
        boardSelector->addItem (title, comboId);
        // JUCE renders disabled popup items in grey. Keep them visible but
        // prevent selecting a board currently reserved by another instance.
        if (! choice.selectable && choice.uniqueId != selectedId)
            boardSelector->setItemEnabled (comboId, false);
        if (choice.uniqueId == selectedId) selectedComboId = comboId;
    }
    if (! unresolvedSavedBoardId.empty() && unresolvedSavedBoardId != selectedId)
    {
        const int missingId = static_cast<int> (boardChoiceIds.size()) + 1;
        boardChoiceIds.push_back (unresolvedSavedBoardId);
        boardSelector->addItem ("Unavailable: " + juce::String (unresolvedSavedBoardId.c_str()),
                                missingId);
        selectedComboId = missingId;
    }
    boardSelector->setSelectedId (selectedComboId, juce::dontSendNotification);

}

void DataThreadPluginEditor::refreshBoardAvailability()
{
    // Rebuilding the ComboBox while its popup is open can invalidate the
    // user's pending selection. Refresh on the next timer tick instead.
    if (thread == nullptr || boardSelector == nullptr || boardSelector->isPopupActive())
        return;

    const auto choices = thread->getBoardChoices();
    const bool changed = choices.size() != displayedBoardChoices.size() ||
        ! std::equal (choices.begin(), choices.end(), displayedBoardChoices.begin(),
                      [] (const DataThreadPlugin::BoardChoice& a,
                          const DataThreadPlugin::BoardChoice& b)
                      {
                          return a.name == b.name && a.uniqueId == b.uniqueId &&
                                 a.selectable == b.selectable;
                      });
    if (changed)
        refreshBoardSelector(); // Update labels only; never select or release hardware.
}

void DataThreadPluginEditor::refreshControls()
{
    refreshBoardSelector();

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
    voltageRangeSelector->setTooltip (thread->canSelectVoltageRange()
        ? "Select the common range for all enabled USB-1608FS-Plus analog inputs"
        : "USB-204 analog input range is fixed at -10 to +10 V");

    architectureLabel->setText (
        juce::String (thread->isSimultaneousSampling() ? "SIM" : "MUX") +
        " | " + juce::String (analogCount) + "AI / " +
        juce::String (digitalCount) + "DI",
        juce::dontSendNotification);

    updateControlAvailability();
}

void DataThreadPluginEditor::comboBoxChanged (juce::ComboBox* box)
{
    if (isAcquiringOrRecording())
    {
        // Protect against a queued GUI click racing with the Play button.
        refreshControls();
        return;
    }
    bool accepted = false;
    if (box == boardSelector.get())
    {
        const int id = boardSelector->getSelectedId();
        if (id <= 0 || static_cast<std::size_t> (id) > boardChoiceIds.size())
            return;
        accepted = thread->selectBoardByUniqueId (
            boardChoiceIds[static_cast<std::size_t> (id - 1)]);
        if (accepted)
        {
            unresolvedSavedBoardId.clear();
            restoreRetryTicks = 0;
        }
    }
    else if (box == sampleRateSelector.get())
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
    if (isAcquiringOrRecording())
    {
        refreshControls();
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
    const auto selectedId = thread->getSelectedBoardUniqueId();
    const auto boardId = unresolvedSavedBoardId.empty() ? selectedId : unresolvedSavedBoardId;
    if (! boardId.empty())
        xml->setAttribute ("board_unique_id", juce::String (boardId.c_str()));
    // A GUI autosave during two-instance restoration must not overwrite the
    // desired channel/rate settings with defaults from an unassigned board.
    const bool pending = ! unresolvedSavedBoardId.empty();
    xml->setAttribute ("sample_rate_hz", pending ? pendingSavedSettings.sampleRateHz
                                                  : static_cast<int> (thread->getSampleRateHz()));
    xml->setAttribute ("ai_channel_mask", pending ? pendingSavedSettings.analogMask
                                                  : static_cast<int> (thread->getChannelMask()));
    xml->setAttribute ("di_channel_mask", pending ? pendingSavedSettings.digitalMask
                                                  : static_cast<int> (thread->getDigitalMask()));
    xml->setAttribute ("input_range_volts", pending ? pendingSavedSettings.inputRangeVolts
                                                    : thread->getVoltageRangeVolts());
}

void DataThreadPluginEditor::loadCustomParametersFromXml (XmlElement* xml)
{
    if (! xml) return;
    // Select hardware BEFORE restoring the range and rate. Old configs have
    // no ID and keep the same automatic-first-board behavior as before.
    const auto savedId = xml->getStringAttribute ("board_unique_id").toStdString();
    // Backward compatibility with older configs containing channel_count.
    int channelMask = xml->getIntAttribute ("ai_channel_mask", -1);
    if (channelMask < 0)
    {
        const int previousCount = xml->getIntAttribute ("channel_count", 1);
        channelMask = (previousCount >= 1 && previousCount <= 8)
            ? ((1 << previousCount) - 1) : 1;
    }
    pendingSavedSettings = { channelMask,
        xml->getIntAttribute ("di_channel_mask", 0),
        xml->getIntAttribute ("sample_rate_hz", 1000),
        xml->getIntAttribute ("input_range_volts", 10) };

    unresolvedSavedBoardId.clear();
    restoreRetryTicks = 0;
    if (! savedId.empty() && ! thread->restoreBoardByUniqueId (savedId))
        unresolvedSavedBoardId = savedId;

    // Ignore legacy di_observation_phase/di_holdback_ms attributes from
    // experimental versions. END + 1 ms is fixed in the acquisition source.
    // Delay applying the saved stream settings until the matching board is
    // selected. Otherwise all four setters can reject the configuration.
    if (unresolvedSavedBoardId.empty())
        applySavedSettings (pendingSavedSettings.analogMask,
                            pendingSavedSettings.digitalMask,
                            pendingSavedSettings.sampleRateHz,
                            pendingSavedSettings.inputRangeVolts);
    refreshControls();
}
