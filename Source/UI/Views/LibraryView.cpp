#include "LibraryView.h"
#include "../../Engine/AudioEngine.h"
#include "../../Library/MetadataReader.h"

namespace om
{

enum ColumnId { colStatus = 1, colTitle, colArtist, colBpm, colKey, colTime, colGenre, colComment, colAnalysis, colAdded, colPlays };

static juce::String durationText (double s)
{
    if (s <= 0.0) return "—"_u;
    const int t = (int) std::round (s);
    return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
}

static juce::String analysisText (Track::Analysis a)
{
    switch (a)
    {
        case Track::Analysis::done:      return "Analizado";
        case Track::Analysis::analysing: return juce::String::fromUTF8 ("Analizando…");
        case Track::Analysis::failed:    return "Error";
        case Track::Analysis::pending:   return "Pendiente";
    }
    return {};
}

//==============================================================================
class LibraryView::SourceTree : public juce::Component
{
public:
    explicit SourceTree (LibraryView& v) : view (v)
    {
        tree.setRootItemVisible (false);
        tree.setDefaultOpenness (false);          // sections are opened explicitly; explorer folders stay closed
        tree.setIndentSize (12);
        tree.setColour (juce::TreeView::backgroundColourId, colour (Col::bgPanel));
        addAndMakeVisible (tree);
        rebuild();
    }

    ~SourceTree() override { tree.setRootItem (nullptr); }

    void resized() override { tree.setBounds (getLocalBounds()); }

    struct Item : public juce::TreeViewItem
    {
        Item (LibraryView& v, juce::String t, Icon i, std::optional<Source> s, int c = -1)
            : view (v), text (std::move (t)), icon (i), source (std::move (s)), count (c) {}

        bool mightContainSubItems() override          { return dir.isNotEmpty() ? (! listed || getNumSubItems() > 0) : getNumSubItems() > 0; }

        // EXPLORADOR folders list their sub-folders when first opened.
        juce::String dir;
        bool listed = false;
        void itemOpennessChanged (bool isNowOpen) override
        {
            if (! isNowOpen || dir.isEmpty() || listed) return;
            listed = true;
            juce::Array<juce::File> subs;
            for (const auto& e : juce::RangedDirectoryIterator (juce::File (dir), false, "*", juce::File::findDirectories))
            {
                const auto f = e.getFile();
                if (f.isHidden() || f.getFileName().startsWith (".") || f.getFileExtension().equalsIgnoreCase (".app")
                    || f.getFileExtension().equalsIgnoreCase (".bundle") || f.getFileExtension().equalsIgnoreCase (".photoslibrary"))
                    continue;
                subs.add (f);
            }
            std::sort (subs.begin(), subs.end(), [] (const juce::File& a, const juce::File& b)
                       { return a.getFileName().compareNatural (b.getFileName()) < 0; });
            for (auto& f : subs)
                addSubItem (makeDir (view, f.getFileName(), f, Icon::folder));
            treeHasChanged();
        }
        static Item* makeDir (LibraryView& v, const juce::String& name, const juce::File& f, Icon ic)
        {
            Source s; s.kind = Source::Kind::explorer; s.path = f.getFullPathName();
            auto* it = new Item (v, name, ic, s);
            it->dir = f.getFullPathName();
            it->setOpenness (juce::TreeViewItem::Openness::opennessClosed);
            return it;
        }
        int  getItemHeight() const override           { return source.has_value() || folderStyle ? 24 : 26; }
        bool canBeSelected() const override           { return source.has_value(); }
        bool folderStyle = false;   // device folder: looks like an entry, holds playlists
        juce::String getUniqueName() const override   { return text; }
        juce::String getTooltip() override             { return dir.isNotEmpty() ? dir : text; }

        void paintItem (juce::Graphics& g, int w, int h) override
        {
            auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
            if (! source.has_value() && ! folderStyle)
            {
                Text::caption (g, text, r.withTrimmedLeft (2.0f), colour (Col::textTertiary), juce::Justification::centredLeft);
                return;
            }

            const bool sel = source.has_value() && view.source == *source;
            if (sel)
            {
                g.setColour (colour (Col::bgControl));
                g.fillRoundedRectangle (r.reduced (0, 1), Radius::sm);
                g.setColour (accent());
                g.fillRoundedRectangle (r.reduced (0, 5).withWidth (2.0f), 1.0f);
            }
            else if (isDropTarget)
            {
                g.setColour (accent().withAlpha (0.2f));
                g.fillRoundedRectangle (r.reduced (0, 1), Radius::sm);
            }

            auto content = r.reduced (8.0f, 0.0f);
            Icons::draw (g, icon, content.removeFromLeft (14.0f).withSizeKeepingCentre (13, 13),
                         sel ? accent() : colour (Col::textSecondary));
            content.removeFromLeft (8.0f);
            if (count >= 0)
                Text::drawTabular (g, juce::String (count), content.removeFromRight (40.0f), font (Type::label),
                                   colour (Col::textTertiary), juce::Justification::centredRight);
            Text::draw (g, text, content, font (Type::body, sel ? Weight::medium : Weight::regular),
                        colour (sel ? Col::textPrimary : Col::textSecondary));
        }

        void itemClicked (const juce::MouseEvent& e) override
        {
            if (folderStyle) { setOpen (! isOpen()); return; }
            if (! source.has_value()) return;
            if (e.mods.isPopupMenu()) { showMenu(); return; }
            view.selectSource (*source);
        }

        void showMenu()
        {
            juce::PopupMenu m;
            auto& lib = view.library;
            if (source->kind == Source::Kind::playlist)
            {
                const int idx = source->index;
                m.addItem ("Renombrar…"_u, [this, idx]
                {
                    auto* w = new juce::AlertWindow ("Renombrar playlist", {}, juce::MessageBoxIconType::NoIcon);
                    w->addTextEditor ("name", view.library.playlists()[(size_t) idx].name);
                    w->addButton ("Renombrar", 1, juce::KeyPress (juce::KeyPress::returnKey));
                    w->addButton ("Cancelar", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                    auto& lv = view;
                    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, idx, &lv] (int r)
                    {
                        if (r == 1 && w->getTextEditorContents ("name").trim().isNotEmpty())
                            lv.library.renamePlaylist (idx, w->getTextEditorContents ("name").trim());
                    }), true);
                });
                m.addItem ("Eliminar playlist", [&lib, idx] { lib.deletePlaylist (idx); });
            }
            else if (source->kind == Source::Kind::folder)
            {
                const auto folder = lib.folders()[source->index];
                m.addItem ("Volver a escanear", [&lib] { lib.rescan(); });
                m.addItem ("Mostrar en Finder", [folder] { folder.revealToUser(); });
                m.addSeparator();
                m.addItem ("Quitar carpeta de la biblioteca", [&lib, folder] { lib.removeFolder (folder); });
            }
            else if (source->kind == Source::Kind::history)
            {
                m.addItem ("Borrar historial", [&lib] { lib.clearHistory(); });
            }
            else return;

            m.showMenuAsync ({});
        }

        bool isInterestedInDragSource (const juce::DragAndDropTarget::SourceDetails& d) override
        {
            return source.has_value() && source->kind == Source::Kind::playlist && d.description.toString().startsWith ("track:");
        }

        void itemDropped (const juce::DragAndDropTarget::SourceDetails& d, int) override
        {
            juce::StringArray paths;
            paths.addLines (d.description.toString().fromFirstOccurrenceOf ("track:", false, false));
            view.library.addToPlaylist (source->index, paths);
        }


        LibraryView& view;
        juce::String text;
        Icon icon;
        std::optional<Source> source;
        int count;
        bool isDropTarget = false;
    };

    void rebuild()
    {
        auto& lib = view.library;
        auto root = std::make_unique<Item> (view, "root", Icon::none, std::nullopt);

        root->addSubItem (new Item (view, "Colección"_u, Icon::collection, Source { Source::Kind::collection, 0, {} }, (int) lib.tracks().size()));

        auto* explorer = new Item (view, "Explorador", Icon::none, std::nullopt);
        {
            Source all; all.kind = Source::Kind::computer;
            explorer->addSubItem (new Item (view, "Buscar en el equipo", Icon::search, all));
            using SL = juce::File::SpecialLocationType;
            const auto home = juce::File::getSpecialLocation (SL::userHomeDirectory);
            const std::pair<const char*, juce::File> places[] = {
                { "Música",     juce::File::getSpecialLocation (SL::userMusicDirectory) },
                { "Descargas",  home.getChildFile ("Downloads") },
                { "Escritorio", juce::File::getSpecialLocation (SL::userDesktopDirectory) },
                { "Documentos", juce::File::getSpecialLocation (SL::userDocumentsDirectory) },
                { "Inicio",     home } };
            for (auto& [name, f] : places)
                if (f.isDirectory())
                    explorer->addSubItem (Item::makeDir (view, juce::String::fromUTF8 (name), f, Icon::folder));
            for (auto& v : view.devices.volumes())
                explorer->addSubItem (Item::makeDir (view, v.getFileName(), v, Icon::eject));
        }
        explorer->setOpenness (juce::TreeViewItem::Openness::opennessOpen);
        root->addSubItem (explorer);

        auto* folders = new Item (view, "Carpetas", Icon::none, std::nullopt);
        for (int i = 0; i < lib.folders().size(); ++i)
            folders->addSubItem (new Item (view, lib.folders()[i].getFileName(), Icon::folder, Source { Source::Kind::folder, i, {} }));
        folders->setOpenness (juce::TreeViewItem::Openness::opennessOpen);
        root->addSubItem (folders);

        auto* lists = new Item (view, "Playlists", Icon::none, std::nullopt);
        for (int i = 0; i < (int) lib.playlists().size(); ++i)
            lists->addSubItem (new Item (view, lib.playlists()[(size_t) i].name, Icon::playlist, Source { Source::Kind::playlist, i, {} },
                                         lib.playlists()[(size_t) i].paths.size()));
        lists->setOpenness (juce::TreeViewItem::Openness::opennessOpen);
        root->addSubItem (lists);

        auto* hist = new Item (view, "Historial", Icon::none, std::nullopt);
        hist->addSubItem (new Item (view, "Todo el historial", Icon::history, Source { Source::Kind::history, 0, {} }, (int) lib.history().size()));
        juce::StringArray days;
        for (auto it = lib.history().rbegin(); it != lib.history().rend(); ++it)
            days.addIfNotAlreadyThere (it->time.formatted ("%Y-%m-%d"));
        for (int i = 0; i < juce::jmin (10, days.size()); ++i)
            hist->addSubItem (new Item (view, days[i], Icon::history, Source { Source::Kind::historyDay, 0, days[i] }));
        hist->setOpenness (juce::TreeViewItem::Openness::opennessOpen);
        root->addSubItem (hist);

        if (! view.devices.devices().empty())
        {
            auto* devs = new Item (view, "Dispositivos", Icon::none, std::nullopt);
            for (int di = 0; di < (int) view.devices.devices().size(); ++di)
            {
                const auto& dev = *view.devices.devices()[(size_t) di];
                auto* d = new Item (view, dev.ex->volumeName, Icon::collection, Source { Source::Kind::device, di, {}, -1 }, (int) dev.tracks.size());

                // Rebuild the device's folder tree (parent id 0 = root).
                std::function<void (Item*, int)> addChildren = [&] (Item* parent, int parentId)
                {
                    for (auto& p : dev.ex->playlists)
                    {
                        if (p.parentId != parentId) continue;
                        if (p.isFolder)
                        {
                            auto* f = new Item (view, p.name, Icon::folder, std::nullopt);
                            f->folderStyle = true;
                            f->setOpenness (juce::TreeViewItem::Openness::opennessOpen);
                            addChildren (f, p.id);
                            parent->addSubItem (f);
                        }
                        else
                        {
                            parent->addSubItem (new Item (view, p.name, Icon::playlist, Source { Source::Kind::device, di, {}, p.id },
                                                          (int) p.trackIds.size()));
                        }
                    }
                };
                addChildren (d, 0);
                d->setOpenness (juce::TreeViewItem::Openness::opennessOpen);
                devs->addSubItem (d);
            }
            devs->setOpenness (juce::TreeViewItem::Openness::opennessOpen);
        root->addSubItem (devs);
        }

        // Keep the explorer folders the user had opened (sections open by themselves).
        juce::StringArray openDirs;
        std::function<void (juce::TreeViewItem*)> collect = [&] (juce::TreeViewItem* it)
        {
            for (int i = 0; it != nullptr && i < it->getNumSubItems(); ++i)
                if (auto* sub = dynamic_cast<Item*> (it->getSubItem (i)))
                {
                    if (sub->dir.isNotEmpty() && sub->isOpen()) openDirs.add (sub->dir);
                    collect (sub);
                }
        };
        collect (rootItem.get());

        tree.setRootItem (nullptr);
        rootItem = std::move (root);
        tree.setRootItem (rootItem.get());

        std::function<void (juce::TreeViewItem*)> reopen = [&] (juce::TreeViewItem* it)
        {
            for (int i = 0; it != nullptr && i < it->getNumSubItems(); ++i)
                if (auto* sub = dynamic_cast<Item*> (it->getSubItem (i)))
                {
                    if (sub->dir.isNotEmpty() && openDirs.contains (sub->dir)) sub->setOpen (true);   // lists its children
                    reopen (sub);
                }
        };
        reopen (rootItem.get());
    }

    void repaintAll() { tree.repaint(); }

    LibraryView& view;
    juce::TreeView tree;
    std::unique_ptr<Item> rootItem;
};

//==============================================================================
LibraryView::LibraryView (Library& l, AudioEngine& e, DeviceMonitor& d) : library (l), engine (e), devices (d)
{
    devices.addChangeListener (this);
    tree = std::make_unique<SourceTree> (*this);
    addAndMakeVisible (*tree);
    addAndMakeVisible (sidebarHandle);
    sidebarHandle.setTooltip ("Arrastrá para agrandar o achicar el panel"_u);
    sidebarHandle.onStart = [this] { dragStartWidth = sidebarWidth; };
    sidebarHandle.onDrag  = [this] (int dx) { setSidebarWidth (dragStartWidth + dx); };
    sidebarHandle.onEnd   = [this] { if (onSidebarResized) onSidebarResized(); };

    search.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Buscar título, artista, género, BPM…   ⌘F"), colour (Col::textTertiary));
    search.setFont (font (Type::body));
    search.setIndents (28, 0);
    search.setJustification (juce::Justification::centredLeft);
    search.addListener (this);
    addAndMakeVisible (search);

    addAndMakeVisible (addFolderBtn);
    addAndMakeVisible (newListBtn);
    addFolderBtn.setTooltip ("Agregar una carpeta de música a la biblioteca"_u);
    newListBtn.setTooltip ("Crear una playlist");
    addFolderBtn.onClick = [this] { if (onAddFolder) onAddFolder(); };
    newListBtn.onClick = [this]
    {
        const int idx = library.createPlaylist ("Playlist " + juce::String ((int) library.playlists().size() + 1));
        selectSource ({ Source::Kind::playlist, idx, {} });
    };

    auto& h = table.getHeader();
    const int flags = juce::TableHeaderComponent::defaultFlags;
    h.addColumn ("",           colStatus,   34, 34, 34, juce::TableHeaderComponent::visible | juce::TableHeaderComponent::sortable);
    h.addColumn ("Título"_u,     colTitle,   260, 80, -1, flags);
    h.addColumn ("Artista",    colArtist,  190, 60, -1, flags);
    h.addColumn ("BPM",        colBpm,      62, 44, 90, flags);
    h.addColumn ("Key",        colKey,      48, 36, 80, flags);
    h.addColumn ("Tiempo",     colTime,     58, 44, 90, flags);
    h.addColumn ("Género"_u,     colGenre,   110, 50, -1, flags);
    h.addColumn ("Comentario", colComment, 160, 50, -1, flags);
    h.addColumn ("Análisis"_u,   colAnalysis, 84, 60, 120, flags);
    h.addColumn ("Agregado",   colAdded,    92, 60, 140, flags);
    h.addColumn ("Plays",      colPlays,    50, 40, 80, flags);
    h.setPopupMenuActive (true); // right-click header: choose visible columns
    h.setStretchToFitActive (false);
    h.setColumnVisible (colPlays, false);

    table.setRowHeight (Size::tableRow);
    table.setHeaderHeight (Size::panelHeader);
    table.setMultipleSelectionEnabled (true);
    table.setColour (juce::ListBox::backgroundColourId, colour (Col::bgPanel));
    table.getViewport()->setScrollBarThickness (10);
    addAndMakeVisible (table);

    library.addChangeListener (this);
    rebuildRows();
}

LibraryView::~LibraryView()
{
    *alive = false;
    explorerPool.removeAllJobs (true, 2000);
    devices.removeChangeListener (this);
    library.removeChangeListener (this);
}

void LibraryView::setShowSidebar (bool s)
{
    showSidebar = s;
    tree->setVisible (s);
    resized();
}

void LibraryView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (library.revision() != seenRevision || devices.revision() != seenDeviceRevision)
    {
        seenRevision = library.revision();
        seenDeviceRevision = devices.revision();
        if (source.kind == Source::Kind::device && ! juce::isPositiveAndBelow (source.index, (int) devices.devices().size()))
            source = {};
        tree->rebuild();
        rebuildRows();
    }
    repaint (statusArea);
}

void LibraryView::selectSource (Source s)
{
    source = s;
    tree->repaintAll();
    if (s.kind == Source::Kind::explorer)      loadExplorerFolder (s.path);
    else if (s.kind == Source::Kind::computer) searchComputer (search.getText());
    rebuildRows();
    repaint (toolbarArea);
}

TrackPtr LibraryView::explorerTrack (const juce::File& f)
{
    if (auto t = library.find (f.getFullPathName())) return t;          // already in the collection
    auto& slot = explorerCache[f.getFullPathName()];
    if (slot == nullptr)
    {
        slot = std::make_shared<Track>();
        slot->file = f;
        const auto name = f.getFileNameWithoutExtension();
        if (name.contains (" - "))
        {
            slot->artist = name.upToFirstOccurrenceOf (" - ", false, false).trim();
            slot->title = name.fromFirstOccurrenceOf (" - ", false, false).trim();
        }
    }
    return slot;
}

void LibraryView::loadExplorerFolder (const juce::String& path)
{
    explorerShown = path;
    explorerTracks.clear();
    explorerBusy = true;
    const int gen = ++explorerGen;
    auto flag = alive;
    explorerPool.addJob ([this, flag, gen, path]
    {
        juce::Array<juce::File> files;
        for (const auto& e : juce::RangedDirectoryIterator (juce::File (path), false, "*", juce::File::findFiles))
            if (Library::isAudioFile (e.getFile()) && ! e.getFile().getFileName().startsWith ("."))
                files.add (e.getFile());
        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
                   { return a.getFileName().compareNatural (b.getFileName()) < 0; });
        juce::MessageManager::callAsync ([this, flag, gen, files]
        {
            if (! *flag || gen != explorerGen) return;
            explorerTracks.clear();
            for (auto& f : files) explorerTracks.push_back (explorerTrack (f));
            explorerBusy = false;
            rebuildRows();
            readTags (explorerTracks, gen);
        });
    });
}

void LibraryView::searchComputer (const juce::String& text)
{
    computerQuery = text.trim();
    const int gen = ++explorerGen;
    const auto tokens = juce::StringArray::fromTokens (computerQuery.removeCharacters ("\"'*\\"), true);
    if (computerQuery.length() < 2 || tokens.isEmpty())
    {
        explorerTracks.clear();
        explorerBusy = false;
        rebuildRows();
        return;
    }
    explorerBusy = true;
    repaint();

    auto flag = alive;
   #if JUCE_MAC
    // The system search index macOS already keeps: audio files whose name, title or artist match every word.
    juce::String query = "kMDItemContentTypeTree == \"public.audio\"";
    for (auto& t : tokens)
        query << " && (kMDItemFSName == \"*" << t << "*\"cd || kMDItemTitle == \"*" << t << "*\"cd || kMDItemAuthors == \"*"
              << t << "*\"cd || kMDItemAlbum == \"*" << t << "*\"cd)";
    juce::StringArray args { "/usr/bin/mdfind", "-onlyin", juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName() };
    for (auto& v : devices.volumes())
        args.addArray ({ "-onlyin", v.getFullPathName() });
    args.add (query);

    explorerPool.addJob ([this, flag, gen, args]
    {
        juce::Thread::sleep (250);                                        // still typing: search only the last text
        if (! *flag || gen != explorerGen) return;
        juce::ChildProcess p;
        juce::StringArray lines;
        if (p.start (args, juce::ChildProcess::wantStdOut))
            lines.addLines (p.readAllProcessOutput());
        juce::Array<juce::File> files;
        for (auto& l : lines)
        {
            if (files.size() >= 500) break;
            if (l.isEmpty() || l.contains ("/Library/") || l.contains ("/.Trash/") || l.contains (".app/") || l.contains ("/node_modules/"))
                continue;
            const juce::File f (l);
            if (Library::isAudioFile (f) && ! f.getFileName().startsWith ("."))
                files.add (f);
        }
   #else
    // No system index to ask: walk the usual music places and the external drives, matching file names.
    juce::Array<juce::File> roots;
    for (auto where : { juce::File::userMusicDirectory, juce::File::userDesktopDirectory, juce::File::userDocumentsDirectory })
        roots.addIfNotAlreadyThere (juce::File::getSpecialLocation (where));
    roots.addIfNotAlreadyThere (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads"));
    for (auto& v : devices.volumes())
        roots.addIfNotAlreadyThere (v);
    juce::StringArray words;
    for (auto& t : tokens) words.add (t.toLowerCase());

    explorerPool.addJob ([this, flag, gen, roots, words]
    {
        juce::Thread::sleep (250);                                        // still typing: search only the last text
        juce::Array<juce::File> files;
        int visited = 0;
        for (auto& root : roots)
        {
            if (! root.isDirectory()) continue;
            for (const auto& e : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFiles))
            {
                if (! *flag || gen != explorerGen) return;
                if (++visited > 200000 || files.size() >= 500) break;
                const auto f = e.getFile();
                if (! Library::isAudioFile (f) || f.getFileName().startsWith (".")) continue;
                const auto name = f.getFileName().toLowerCase();
                bool all = true;
                for (auto& w : words) if (! name.contains (w)) { all = false; break; }
                if (all) files.add (f);
            }
        }
        if (! *flag || gen != explorerGen) return;
   #endif
        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
                   { return a.getFileName().compareNatural (b.getFileName()) < 0; });
        juce::MessageManager::callAsync ([this, flag, gen, files]
        {
            if (! *flag || gen != explorerGen) return;
            explorerTracks.clear();
            for (auto& f : files) explorerTracks.push_back (explorerTrack (f));
            explorerBusy = false;
            rebuildRows();
            readTags (explorerTracks, gen);
        });
    });
}

void LibraryView::readTags (std::vector<TrackPtr> list, int gen)
{
    std::vector<TrackPtr> todo;
    for (auto& t : list)
        if (library.find (t->path()) == nullptr && tagged.count (t->path()) == 0)
            todo.push_back (t);
    if (todo.empty()) return;
    for (auto& t : todo) tagged.insert (t->path());
    auto flag = alive;
    explorerPool.addJob ([this, flag, gen, todo]
    {
        std::vector<std::pair<TrackPtr, FileTags>> read;
        for (auto& t : todo)
        {
            if (! *flag) return;
            read.push_back ({ t, Metadata::read (t->file) });
            if (read.size() == 32 || &t == &todo.back())
            {
                juce::MessageManager::callAsync ([this, flag, gen, batch = std::move (read)]
                {
                    if (! *flag) return;
                    for (auto& [track, tags] : batch)
                    {
                        if (tags.title.isNotEmpty())  track->title = tags.title;
                        if (tags.artist.isNotEmpty()) track->artist = tags.artist;
                        track->album = tags.album;
                        track->genre = tags.genre;
                        track->comment = tags.comment;
                        track->key = tags.key;
                        if (tags.bpm > 0.0) track->bpm = tags.bpm;
                        track->duration = tags.durationSeconds;
                    }
                    if (gen == explorerGen) table.repaint();
                });
                read = {};
            }
        }
    });
}

juce::String LibraryView::sourceTitle() const
{
    switch (source.kind)
    {
        case Source::Kind::collection: return "Colección"_u;
        case Source::Kind::folder:     return library.folders()[source.index].getFileName();
        case Source::Kind::playlist:   return juce::isPositiveAndBelow (source.index, (int) library.playlists().size())
                                              ? library.playlists()[(size_t) source.index].name : juce::String ("Playlist");
        case Source::Kind::history:    return "Historial";
        case Source::Kind::historyDay: return "Historial · "_u + source.day;
        case Source::Kind::explorer:   return juce::File (source.path).getFileName().isNotEmpty() ? juce::File (source.path).getFileName() : source.path;
        case Source::Kind::computer:   return "Buscar en el equipo";
        case Source::Kind::device:
        {
            if (! juce::isPositiveAndBelow (source.index, (int) devices.devices().size())) return "Dispositivo";
            const auto& ex = *devices.devices()[(size_t) source.index]->ex;
            for (auto& p : ex.playlists)
                if (p.id == source.sub) return ex.volumeName + " · "_u + p.name;
            return ex.volumeName + juce::String::fromUTF8 (" · Todas las pistas");
        }
    }
    return {};
}

void LibraryView::rebuildRows()
{
    std::vector<TrackPtr> out;

    switch (source.kind)
    {
        case Source::Kind::collection:
            out = library.tracks();
            break;
        case Source::Kind::folder:
        {
            const auto folder = library.folders()[source.index];
            for (auto& t : library.tracks())
                if (t->file.isAChildOf (folder)) out.push_back (t);
            break;
        }
        case Source::Kind::playlist:
            if (juce::isPositiveAndBelow (source.index, (int) library.playlists().size()))
                for (auto& p : library.playlists()[(size_t) source.index].paths)
                    if (auto t = library.find (p)) out.push_back (t);
            break;
        case Source::Kind::history:
        case Source::Kind::historyDay:
            for (auto it = library.history().rbegin(); it != library.history().rend(); ++it)
                if (source.kind == Source::Kind::history || it->time.formatted ("%Y-%m-%d") == source.day)
                {
                    if (auto t = library.find (it->path)) out.push_back (t);
                    else if (auto dt = devices.findByPath (it->path)) out.push_back (dt);
                }
            break;
        case Source::Kind::explorer:
        case Source::Kind::computer:
            out = explorerTracks;
            break;
        case Source::Kind::device:
            if (juce::isPositiveAndBelow (source.index, (int) devices.devices().size()))
            {
                const auto& dev = *devices.devices()[(size_t) source.index];
                if (source.sub < 0)
                    out = dev.tracks;
                else
                    for (auto& p : dev.ex->playlists)
                        if (p.id == source.sub)
                            for (auto id : p.trackIds)
                                if (auto it = dev.byId.find (id); it != dev.byId.end()) out.push_back (it->second);
            }
            break;
    }

    // Search: every whitespace-separated token must match some field.
    const auto tokens = juce::StringArray::fromTokens (search.getText().toLowerCase(), true);
    if (! tokens.isEmpty() && source.kind != Source::Kind::computer)     // the computer search already matched
    {
        std::vector<TrackPtr> filtered;
        for (auto& t : out)
        {
            const auto hay = (t->title + " " + t->artist + " " + t->album + " " + t->genre + " " + t->comment + " " + t->displayKey() + " "
                              + t->file.getFileName() + " " + (t->bpm > 0 ? juce::String (t->bpm, 1) : juce::String())).toLowerCase();
            bool all = true;
            for (auto& tok : tokens)
                if (! hay.contains (tok)) { all = false; break; }
            if (all) filtered.push_back (t);
        }
        out = std::move (filtered);
    }

    if (sortColumn > 0)
    {
        auto key = [this] (const TrackPtr& a, const TrackPtr& b) -> int
        {
            switch (sortColumn)
            {
                case colTitle:    return a->displayTitle().compareNatural (b->displayTitle());
                case colArtist:   return a->artist.compareNatural (b->artist);
                case colBpm:      return a->bpm < b->bpm ? -1 : (a->bpm > b->bpm ? 1 : 0);
                case colKey:      return a->displayKey().compareNatural (b->displayKey());
                case colTime:     return a->duration < b->duration ? -1 : (a->duration > b->duration ? 1 : 0);
                case colGenre:    return a->genre.compareNatural (b->genre);
                case colComment:  return a->comment.compareNatural (b->comment);
                case colAnalysis: return (int) a->analysis - (int) b->analysis;
                case colAdded:    return a->added < b->added ? -1 : (a->added > b->added ? 1 : 0);
                case colPlays:    return a->playCount - b->playCount;
                default:          return 0;
            }
        };
        std::stable_sort (out.begin(), out.end(), [&] (const TrackPtr& a, const TrackPtr& b)
        {
            const int c = key (a, b);
            return sortForwards ? c < 0 : c > 0;
        });
    }

    rows = std::move (out);
    table.updateContent();
    table.repaint();
    repaint (statusArea);
}

void LibraryView::refresh()
{
    bool changed = false;
    for (int i = 0; i < 2; ++i)
    {
        const auto t = engine.deck (i).getTrack();
        const auto p = t != nullptr ? t->file.getFullPathName() : juce::String();
        if (p != deckPaths[(size_t) i]) { deckPaths[(size_t) i] = p; changed = true; }
    }
    if (changed)
        table.repaint();

    // Status line updates (analysis progress) without rebuilding rows.
    const auto status = juce::String ((int) rows.size()) + "|" + juce::String (library.pendingAnalysisCount()) + "|"
                      + juce::String ((int) library.isScanning());
    if (status != lastStatus)
    {
        lastStatus = status;
        repaint (statusArea);
        table.repaint();
    }
}

//==============================================================================
void LibraryView::paintRowBackground (juce::Graphics& g, int row, int w, int h, bool selected)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
    if (selected)
    {
        g.setColour (colour (Col::bgControl));
        g.fillRect (r);
        g.setColour (accent());
        g.fillRect (r.withWidth (2.0f));
    }
    else if (row % 2 == 1)
    {
        g.setColour (colour (Col::bgRaised).withAlpha (0.45f));
        g.fillRect (r);
    }
}

void LibraryView::paintCell (juce::Graphics& g, int row, int col, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return;

    const auto& t = rows[(size_t) row];
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (8.0f, 0.0f);
    const bool missing = ! t->file.existsAsFile();
    const bool played = t->playCount > 0 && source.kind != Source::Kind::history && source.kind != Source::Kind::historyDay;
    const auto primary = missing ? colour (Col::textDisabled) : colour (played && ! selected ? Col::textSecondary : Col::textPrimary);
    const auto f = font (Type::body);

    switch (col)
    {
        case colStatus:
        {
            // Loaded-on-deck marker takes priority, then missing file, then analysis state.
            for (int d = 0; d < 2; ++d)
                if (deckPaths[(size_t) d] == t->path())
                {
                    auto b = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ (float) w * 0.5f, (float) h * 0.5f });
                    g.setColour (Theme::get().deckColour (d));
                    g.fillRoundedRectangle (b, 2.0f);
                    Text::draw (g, juce::String (d + 1), b, font (Type::micro, Weight::bold), colour (Col::textOnAccent),
                                juce::Justification::centred, false);
                    return;
                }
            auto ic = juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ (float) w * 0.5f, (float) h * 0.5f });
            if (missing)                                       Icons::draw (g, Icon::warning, ic, colour (Col::warning));
            else if (t->analysis == Track::Analysis::failed)   Icons::draw (g, Icon::error, ic, colour (Col::error));
            else if (t->analysis != Track::Analysis::done)     Icons::draw (g, Icon::history, ic, colour (Col::textTertiary));
            else if (played)                                   Icons::draw (g, Icon::check, ic, colour (Col::textTertiary));
            return;
        }
        case colTitle:   Text::draw (g, t->displayTitle(), r, font (Type::body, Weight::medium), primary); return;
        case colArtist:  Text::draw (g, t->artist, r, f, primary); return;
        case colBpm:
            Text::drawTabular (g, t->bpm > 0 ? juce::String (t->bpm, 2) : juce::String ("—"_u), r, f,
                               t->bpm > 0 ? primary : colour (Col::textDisabled), juce::Justification::centredRight);
            return;
        case colKey:
        {
            const auto k = t->displayKey();
            if (k.isEmpty()) { Text::draw (g, "—"_u, r, f, colour (Col::textDisabled)); return; }
            // Harmonic hint: keys compatible with what's playing get a subtle accent.
            bool compatible = false;
            for (int d = 0; d < 2; ++d)
                if (auto dt = engine.deck (d).getTrack(); dt != nullptr && engine.deck (d).isPlaying()
                        && dt->file.getFullPathName() != t->path())
                    compatible = compatible || Analysis::camelotCompatible (dt->meta.key, k);
            Text::draw (g, k, r, font (Type::body, compatible ? Weight::semibold : Weight::regular),
                        compatible ? colour (Col::play) : primary);
            return;
        }
        case colTime:    Text::drawTabular (g, durationText (t->duration), r, f, primary, juce::Justification::centredRight); return;
        case colGenre:   Text::draw (g, t->genre, r, f, colour (Col::textSecondary)); return;
        case colComment: Text::draw (g, t->comment, r, f, colour (Col::textSecondary)); return;
        case colAnalysis:
        {
            const auto c = t->analysis == Track::Analysis::done ? colour (Col::textTertiary)
                         : t->analysis == Track::Analysis::failed ? colour (Col::error) : colour (Col::warning);
            Text::draw (g, analysisText (t->analysis), r, font (Type::label), c);
            return;
        }
        case colAdded:   Text::drawTabular (g, t->added.toMilliseconds() > 0 ? t->added.formatted ("%Y-%m-%d") : "—"_u, r, font (Type::label), colour (Col::textTertiary)); return;
        case colPlays:   Text::drawTabular (g, juce::String (t->playCount), r, f, colour (Col::textSecondary), juce::Justification::centredRight); return;
        default: return;
    }
}

juce::String LibraryView::getCellTooltip (int row, int col)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size())) return {};
    const auto& t = rows[(size_t) row];
    if (! t->file.existsAsFile()) return "Archivo no encontrado: " + t->path();
    if (col == colStatus && t->isExternal()) return juce::String::fromUTF8 ("Pista de un pendrive exportado (solo lectura)");
    if (col == colKey && t->displayKey().isNotEmpty())
        return Analysis::camelotToMusical (t->displayKey()) + juce::String::fromUTF8 (" — verde: compatible armónicamente con lo que suena");
    if (col == colKey) return juce::String::fromUTF8 ("Tonalidad no detectada todavía");
    if (col == colStatus && t->analysis == Track::Analysis::failed) return "No se pudo analizar el archivo";
    return {};
}

void LibraryView::sortOrderChanged (int columnId, bool forwards)
{
    sortColumn = columnId == colStatus ? 0 : columnId;
    sortForwards = forwards;
    rebuildRows();
}

void LibraryView::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow (row, (int) rows.size()) && onLoadToDeck)
        onLoadToDeck (rows[(size_t) row], -1);
}

void LibraryView::moveSelection (int delta)
{
    if (rows.empty()) return;
    const int cur = table.getSelectedRow();
    const int next = juce::jlimit (0, (int) rows.size() - 1, cur < 0 ? 0 : cur + delta);
    table.selectRow (next);
    table.scrollToEnsureRowIsOnscreen (next);
}

TrackPtr LibraryView::selectedTrack() const
{
    const int r = table.getSelectedRow();
    return juce::isPositiveAndBelow (r, (int) rows.size()) ? rows[(size_t) r] : nullptr;
}

void LibraryView::returnKeyPressed (int row)
{
    if (juce::isPositiveAndBelow (row, (int) rows.size()) && onLoadToDeck)
        onLoadToDeck (rows[(size_t) row], -1);
}

void LibraryView::cellClicked (int row, int, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        showRowMenu (row);
}

juce::var LibraryView::getDragSourceDescription (const juce::SparseSet<int>& selected)
{
    juce::StringArray paths;
    for (int i = 0; i < selected.size(); ++i)
        if (juce::isPositiveAndBelow (selected[i], (int) rows.size()))
            paths.add (rows[(size_t) selected[i]]->path());
    return paths.isEmpty() ? juce::var() : juce::var ("track:" + paths.joinIntoString ("\n"));
}

std::vector<TrackPtr> LibraryView::selectedTracks() const
{
    std::vector<TrackPtr> sel;
    for (int i = 0; i < table.getNumSelectedRows(); ++i)
        if (juce::isPositiveAndBelow (table.getSelectedRow (i), (int) rows.size()))
            sel.push_back (rows[(size_t) table.getSelectedRow (i)]);
    return sel;
}

void LibraryView::showRowMenu (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size())) return;
    if (! table.isRowSelected (row)) table.selectRow (row);

    std::vector<TrackPtr> sel;
    for (int i = 0; i < table.getNumSelectedRows(); ++i)
        if (juce::isPositiveAndBelow (table.getSelectedRow (i), (int) rows.size()))
            sel.push_back (rows[(size_t) table.getSelectedRow (i)]);
    juce::StringArray paths;
    for (auto& t : sel) if (! t->isExternal()) paths.add (t->path());

    const auto t = rows[(size_t) row];
    juce::PopupMenu m;
    m.addItem ("Cargar en Deck 1", [this, t] { if (onLoadToDeck) onLoadToDeck (t, 0); });
    m.addItem ("Cargar en Deck 2", [this, t] { if (onLoadToDeck) onLoadToDeck (t, 1); });
    if (onAddToAutoMix)
        m.addItem ("Agregar a AUTOMIX", [this, sel] { onAddToAutoMix (sel); });
    m.addSeparator();

    juce::PopupMenu lists;
    for (int i = 0; i < (int) library.playlists().size(); ++i)
        lists.addItem (library.playlists()[(size_t) i].name, [this, i, paths] { library.addToPlaylist (i, paths); });
    if (library.playlists().empty())
        lists.addItem ("No hay playlists", false, false, nullptr);
    m.addSubMenu ("Agregar a playlist", lists);

    if (source.kind == Source::Kind::playlist)
        m.addItem ("Quitar de esta playlist", [this, paths] { library.removeFromPlaylist (source.index, paths); });

    m.addItem ("Volver a analizar (BPM y grilla)", [this, sel] { if (onReanalyse) onReanalyse (sel); });
    m.addItem ("Mostrar en Finder", [t] { t->file.revealToUser(); });
    m.showMenuAsync ({});
}

//==============================================================================
void LibraryView::resized()
{
    auto r = getLocalBounds();
    if (showSidebar)
    {
        sidebarArea = r.removeFromLeft (juce::jmin (sidebarWidth, juce::jmax (minSidebar, getWidth() - 300)));
        auto sb = sidebarArea;
        auto top = sb.removeFromTop (34).reduced (Space::sm, 6);
        const int bw = (top.getWidth() - Space::xs) / 2;
        addFolderBtn.setBounds (top.removeFromLeft (bw));
        newListBtn.setBounds (top.removeFromRight (bw));
        tree->setBounds (sb.reduced (Space::sm, 0));
        r.removeFromLeft (1);
        sidebarHandle.setBounds (sidebarArea.getRight() - 3, sidebarArea.getY(), 7, sidebarArea.getHeight());
    }
    else
    {
        sidebarArea = {};
    }
    addFolderBtn.setVisible (showSidebar);
    sidebarHandle.setVisible (showSidebar);
    newListBtn.setVisible (showSidebar);

    toolbarArea = r.removeFromTop (34);
    statusArea = r.removeFromBottom (20);
    auto tb = toolbarArea.reduced (Space::md, 5);
    search.setBounds (tb.removeFromRight (juce::jlimit (200, 420, tb.getWidth() / 2)));
    table.setBounds (r);
}

void LibraryView::paint (juce::Graphics& g)
{
    g.fillAll (colour (Col::bgPanel));


    // Toolbar: current source title
    auto tb = toolbarArea.reduced (Space::md, 0).toFloat();
    Text::draw (g, sourceTitle(), tb.removeFromLeft (tb.getWidth() * 0.45f), font (Type::title, Weight::semibold), colour (Col::textPrimary));

    // Search icon inside the field
    const auto sb = search.getBounds().toFloat();
    Icons::draw (g, Icon::search, juce::Rectangle<float> (sb.getX() + 8.0f, sb.getCentreY() - 6.0f, 12.0f, 12.0f), colour (Col::textTertiary));

    // Status line
    auto st = statusArea.toFloat().reduced (Space::md, 0);
    g.setColour (colour (Col::line));
    g.fillRect (statusArea.withHeight (1));
    juce::String left = juce::String ((int) rows.size()) + " pistas";
    if (search.getText().isNotEmpty())
        left << "  ·  filtro: \""_u << search.getText() << "\"";
    Text::draw (g, left, st, font (Type::label), colour (Col::textTertiary));

    juce::String right;
    if (library.isScanning()) right << juce::String::fromUTF8 ("Escaneando carpetas…   ");
    if (library.pendingAnalysisCount() > 0) right << "Analizando BPM: " << library.pendingAnalysisCount() << " en cola";
    if (right.isNotEmpty())
        Text::draw (g, right, st, font (Type::label, Weight::medium), colour (Col::warning), juce::Justification::centredRight);
}

void LibraryView::paintOverChildren (juce::Graphics& g)
{
    auto emptyArea = table.getBounds().toFloat().withTrimmedTop ((float) Size::panelHeader);
    if (source.kind == Source::Kind::computer || source.kind == Source::Kind::explorer)
    {
        if (! rows.empty()) return;
        const juce::String msg = explorerBusy ? juce::String::fromUTF8 ("Buscando…")
                               : source.kind == Source::Kind::explorer ? juce::String::fromUTF8 ("No hay canciones en esta carpeta (abrí las subcarpetas a la izquierda)")
                               : computerQuery.length() < 2 ? juce::String::fromUTF8 ("Escribí arriba el nombre de una canción o artista para buscarla en todo el equipo")
                               : juce::String::fromUTF8 ("No se encontró nada en el equipo con «") + computerQuery + juce::String::fromUTF8 ("»");
        Text::draw (g, msg, emptyArea, font (Type::body), colour (Col::textTertiary), juce::Justification::centred);
        return;
    }
    if (library.tracks().empty() && ! library.isScanning() && source.kind == Source::Kind::collection)
    {
        auto empty = emptyArea;
        Text::draw (g, juce::String::fromUTF8 ("La biblioteca está vacía. Agregá una carpeta de música con «+ Carpeta» o arrastrá archivos a un deck."),
                    empty, font (Type::body), colour (Col::textTertiary), juce::Justification::centred);
    }
}

} // namespace om
