#pragma once

#include "../Components/Controls.h"
#include "../../Control/AutoMix.h"
#include "../../Common/Strings.h"

namespace om
{

/** AUTOMIX side panel (opened from the top bar, beside the library): the queue, start / stop,
    "mix now" and the length of each mix. Tracks are dragged in from the library (or added with
    the buttons) and reordered by dragging inside the list. */
class AutoMixPanel : public juce::Component,
                     public juce::DragAndDropTarget,
                     private juce::ListBoxModel,
                     private juce::ChangeListener
{
public:
    struct Hooks
    {
        std::function<std::vector<TrackPtr>()> selected;      // library selection
        std::function<std::vector<TrackPtr>()> visible;       // everything the library shows now
        std::function<TrackPtr (const juce::String& path)> trackFor;
        std::function<void()> onClose;
    };

    AutoMixPanel (AutoMix&, AudioEngine&, Hooks);
    ~AutoMixPanel() override;

    void refresh();                 // per frame: status line

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>&) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void deleteKeyPressed (int lastRowSelected) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    int insertIndexAt (juce::Point<int> posInPanel) const;
    void updateButtons();

    AutoMix& automix;
    AudioEngine& engine;
    Hooks hooks;

    Button closeBtn { {}, Icon::close, Button::Style::ghost };
    Button runBtn { "INICIAR", Icon::play };
    Button mixNowBtn { "MEZCLAR AHORA" };
    std::array<Button, 5> lengthBtns { Button { "4" }, Button { "8" }, Button { "16" }, Button { "32" }, Button { "64" } };
    static constexpr int lengths[5] = { 4, 8, 16, 32, 64 };
    std::array<Button, (size_t) AutoMix::Mode::numModes> modeBtns;
    Button addSelBtn { "+ Selección"_u }, addAllBtn { "+ Lista visible" }, shuffleBtn { "Aleatorio" }, clearBtn { "Vaciar" };
    juce::ListBox list { "automix", this };

    juce::Rectangle<int> headerArea, lengthLabelArea, modeLabelArea, statusArea;
    juce::String lastStatus;
    int dropIndex = -1;
    int shownRows = -1;
};

} // namespace om
