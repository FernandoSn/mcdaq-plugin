/* MCDAQ Open Ephys editor: NI-style single-ended analog channel selection. */
#ifndef DATATHREADPLUGINEDITOR_H_DEFINED
#define DATATHREADPLUGINEDITOR_H_DEFINED

#include <EditorHeaders.h>
#include "DataThreadPlugin.h"
#include <array>

class DataThreadPluginEditor : public GenericEditor,
                               private juce::ComboBox::Listener,
                               private juce::Button::Listener
{
public:
    DataThreadPluginEditor (GenericProcessor* parentNode, DataThreadPlugin* thread);
    ~DataThreadPluginEditor() override;
    void saveCustomParametersToXml (XmlElement* xml) override;
    void loadCustomParametersFromXml (XmlElement* xml) override;

private:
    void comboBoxChanged (juce::ComboBox* box) override;
    void buttonClicked (juce::Button* button) override;
    void refreshControls();
    void refreshBoardSelector();
    void refreshBoardAvailability();
    // Saved XML may be restored after another instance temporarily auto-owns
    // the requested device. Retry only when it becomes free; never steal it.
    void retryPendingBoardRestoration();
    void applySavedSettings (int analogMask, int digitalMask,
                             int sampleRateHz, int inputRangeVolts);
    void updateControlAvailability();
    bool isAcquiringOrRecording() const;

    // A separate timer avoids depending on Open Ephys' editor activation
    // callbacks, which are not fired every time the global Play/Record state
    // changes. The timer never touches acquisition or hardware code.
    class ActivityTimer final : private juce::Timer
    {
    public:
        explicit ActivityTimer (DataThreadPluginEditor& editor) : editor (editor) {}
        void start() { juce::Timer::startTimer (100); }
        void stop()  { juce::Timer::stopTimer(); }

    private:
        void timerCallback() override;
        DataThreadPluginEditor& editor;
    };

    DataThreadPlugin* thread = nullptr;
    std::array<std::unique_ptr<juce::TextButton>, 8> analogButtons;
    std::array<std::unique_ptr<juce::TextButton>, 8> digitalButtons;
    std::unique_ptr<juce::ComboBox> boardSelector;
    std::vector<std::string> boardChoiceIds; // Index corresponds to ComboBox ID (index + 1).
    std::vector<DataThreadPlugin::BoardChoice> displayedBoardChoices;
    std::string unresolvedSavedBoardId;
    // Preserve the complete saved configuration while its physical board is
    // temporarily occupied. Otherwise setters reject it with no board present.
    struct SavedSettings
    {
        int analogMask = 1;
        int digitalMask = 0;
        int sampleRateHz = 1000;
        int inputRangeVolts = 10;
    };
    SavedSettings pendingSavedSettings;
    int restoreRetryTicks = 0;
    std::unique_ptr<juce::ComboBox> sampleRateSelector;
    std::unique_ptr<juce::Label> exactRateLabel;
    std::unique_ptr<juce::ComboBox> voltageRangeSelector;
    std::unique_ptr<juce::Label> sampleRateLabel;
    std::unique_ptr<juce::Label> voltageRangeLabel;
    std::unique_ptr<juce::Label> architectureLabel;

    ActivityTimer activityTimer { *this };
    bool controlsLocked = false;
};

#endif
