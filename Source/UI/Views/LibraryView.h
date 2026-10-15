#pragma once

#include "../Components/Controls.h"
#include "../../Library/Library.h"
#include "../../Library/DeviceMonitor.h"

namespace om
{

class AudioEngine;

/** Library browser: source tree, search, high-density track table, status line. */
class LibraryView : public juce::Component,
                    private juce::ChangeListener,
                    private juce::TableListBoxModel,
                    private juce::TextEditor::Listener
{
public:
    LibraryView (Library&, AudioEngine&, DeviceMonitor&);
    ~LibraryView() override;

    void setShowSidebar (bool);
    bool isShowingSidebar() const noexcept        { return showSidebar; }
    void focusSearch()                             { search.grabKeyboardFocus(); }
    void moveSelection (int delta);
    TrackPtr selectedTrack() const;
    std::vector<TrackPtr> selectedTracks() const;
    const std::vector<TrackPtr>& visibleTracks() const noexcept { return rows; }

    juce::String getColumnState() const            { return table.getHeader().toString(); }
    int  getSidebarWidth() const noexcept          { return sidebarWidth; }
    void setSidebarWidth (int w)                   { sidebarWidth = juce::jlimit (minSidebar, maxSidebar, w); resized(); }
    static constexpr int minSidebar = 150, maxSidebar = 640;
    void setColumnState (const juce::String& s)    { if (s.isNotEmpty()) table.getHeader().restoreFromString (s); }

    std::function<void (const TrackPtr&, int deck)> onLoadToDeck;   // deck −1 = auto
    std::function<void()> onAddFolder;
    std::function<void (const std::vector<TrackPtr>&)> onAddToAutoMix;

    void refresh();                                 // cheap; called every frame
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    struct Source
    {
        enum class Kind { collection, folder, playlist, history, historyDay, device, explorer, computer } kind = Kind::collection;
        int index = 0;
        juce::String day;
        int sub = -1;   // device: playlist id (−1 = all tracks)
        juce::String path {};   // explorer: the folder shown
        bool operator== (const Source& o) const { return kind == o.kind && index == o.index && day == o.day && sub == o.sub && path == o.path; }
    };

    void selectSource (Source);

private:
    class SourceTree;
    friend class SourceTree;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void textEditorTextChanged (juce::TextEditor&) override
    {
        if (source.kind == Source::Kind::computer) searchComputer (search.getText());
        else rebuildRows();
    }
    void textEditorEscapeKeyPressed (juce::TextEditor&) override { search.clear(); rebuildRows(); }

    // TableListBoxModel
    int  getNumRows() override                     { return (int) rows.size(); }
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    void sortOrderChanged (int columnId, bool forwards) override;
    void cellDoubleClicked (int row, int col, const juce::MouseEvent&) override;
    void cellClicked (int row, int col, const juce::MouseEvent&) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>&) override;
    juce::String getCellTooltip (int row, int col) override;
    void returnKeyPressed (int row) override;

    void rebuildRows();

    // EXPLORADOR: folders of this computer and a system-index search over it. Everything that touches the
    // disk runs on the explorer pool; results come back to the message thread.
    TrackPtr explorerTrack (const juce::File&);
    void loadExplorerFolder (const juce::String& path);
    void searchComputer (const juce::String& text);
    void readTags (std::vector<TrackPtr>, int generation);
    void rebuildTree();
    void showRowMenu (int row);
    juce::String sourceTitle() const;

    Library& library;
    AudioEngine& engine;
    DeviceMonitor& devices;
    juce::uint64 seenDeviceRevision = 0;

    std::unique_ptr<SourceTree> tree;

    /** Thin handle on the sidebar's right edge: drag left / right to resize it. */
    struct SidebarHandle : public juce::Component, public juce::SettableTooltipClient
    {
        SidebarHandle()                                         { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); setRepaintsOnMouseActivity (true); }
        void paint (juce::Graphics& g) override
        {
            g.setColour (isMouseOverOrDragging() ? accent() : colour (Col::line));
            g.fillRect (getWidth() / 2, 0, isMouseOverOrDragging() ? 2 : 1, getHeight());
        }
        void mouseDown (const juce::MouseEvent&) override       { if (onStart) onStart(); }
        void mouseDrag (const juce::MouseEvent& e) override     { if (onDrag) onDrag (e.getDistanceFromDragStartX()); }
        void mouseUp (const juce::MouseEvent&) override         { if (onEnd) onEnd(); }
        std::function<void()> onStart, onEnd;
        std::function<void (int)> onDrag;
    };
    SidebarHandle sidebarHandle;
    int sidebarWidth = Size::sidebarWidth, dragStartWidth = 0;
public:
    std::function<void()> onSidebarResized;            // persist the width
private:
    juce::TextEditor search;
    Button addFolderBtn { "Carpeta", Icon::plus, Button::Style::ghost };
    Button newListBtn   { "Playlist", Icon::plus, Button::Style::ghost };
    juce::TableListBox table { {}, this };

    std::vector<TrackPtr> rows;
    Source source;
    int sortColumn = 0;
    bool sortForwards = true;
    juce::uint64 seenRevision = 0;
    bool showSidebar = true;
    juce::String lastStatus;
    std::array<juce::String, 2> deckPaths;
    juce::StringArray playedThisSession;

    juce::Rectangle<int> sidebarArea, toolbarArea, statusArea;

    std::vector<TrackPtr> explorerTracks;
    juce::String explorerShown, computerQuery;
    int explorerGen = 0;
    bool explorerBusy = false;
    std::map<juce::String, TrackPtr> explorerCache;     // path → track (tags read once)
    std::set<juce::String> tagged;
    juce::ThreadPool explorerPool { juce::ThreadPoolOptions{}.withNumberOfThreads (2).withThreadName ("OpenMix explorer") };
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};

} // namespace om
