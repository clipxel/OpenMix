#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "UI/Theme/LookAndFeel.h"
#include "Library/Library.h"
#include "Tests/SelfTest.h"
#include "Control/MappingImport.h"
#include "Common/Updater.h"
#include <iostream>

class OpenMixApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override
    {
        // Headless tools may run next to the open app; the app itself stays single-instance.
        const auto cl = getCommandLineParameters();
        return cl.contains ("--snapshot") || cl.contains ("--selftest") || cl.contains ("--inspect-usb") || cl.contains ("--audio-test") || cl.contains ("--analyzer-benchmark") || cl.contains ("--make-icons") || cl.contains ("--import-check") || cl.contains ("--simulate-controllers") || cl.contains ("--build-library") || cl.contains ("--update-test") || cl.contains ("--update-install-test") || cl.contains ("--dump-mapping") || cl.contains ("--grid-debug");
    }

    void initialise (const juce::String& commandLine) override
    {
        lookAndFeel = std::make_unique<om::LookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        if (commandLine.contains ("--make-icons"))
        {
            // Build-time helper: macOS app icon (rounded square, Apple grid) and a transparent logo mark.
            auto args = juce::StringArray::fromTokens (commandLine, true);
            const auto src = juce::ImageFileFormat::loadFrom (juce::File (args[args.indexOf ("--make-icons") + 1].unquoted()));
            const auto dir = juce::File (args[args.indexOf ("--make-icons") + 2].unquoted());
            if (src.isValid())
            {
                // Artwork: light background with a dark disc holding the mark. Find the disc.
                const auto bg = src.getPixelAt (4, 4);
                int x0 = src.getWidth(), y0 = src.getHeight(), x1 = 0, y1 = 0;
                for (int y = 0; y < src.getHeight(); ++y)
                    for (int x = 0; x < src.getWidth(); ++x)
                        if (src.getPixelAt (x, y).getBrightness() < bg.getBrightness() * 0.5f)
                        {
                            x0 = juce::jmin (x0, x); y0 = juce::jmin (y0, y); x1 = juce::jmax (x1, x); y1 = juce::jmax (y1, y);
                        }
                const float cx = (float) (x0 + x1) * 0.5f, cy = (float) (y0 + y1) * 0.5f;
                const float radius = (float) juce::jmax (x1 - x0, y1 - y0) * 0.5f + 0.5f;

                // Logo mark: the disc alone, anti-aliased edge, transparent outside.
                const int side = (int) std::ceil (radius * 2.0f) + 2;
                juce::Image mark (juce::Image::ARGB, side, side, true);
                for (int y = 0; y < side; ++y)
                    for (int x = 0; x < side; ++x)
                    {
                        const float sx = cx - radius - 1.0f + (float) x, sy = cy - radius - 1.0f + (float) y;
                        const float d = std::hypot (sx - cx, sy - cy);
                        const float a = juce::jlimit (0.0f, 1.0f, radius - d + 0.5f);
                        if (a > 0.0f)
                            mark.setPixelAt (x, y, src.getPixelAt (juce::jlimit (0, src.getWidth() - 1, (int) sx),
                                                                   juce::jlimit (0, src.getHeight() - 1, (int) sy)).withAlpha (a));
                    }
                {
                    juce::FileOutputStream os (dir.getChildFile ("logo-mark.png"));
                    os.setPosition (0); os.truncate();
                    juce::PNGImageFormat().writeImageToStream (mark, os);
                }

                // App icon (Dock): the round logo mark itself, no background plate. 1024 canvas, disc on
                // the 824 Apple grid with the standard soft shadow.
                juce::Image icon (juce::Image::ARGB, 1024, 1024, true);
                {
                    juce::Graphics g (icon);
                    const juce::Rectangle<float> body (100.0f, 100.0f, 824.0f, 824.0f);
                    juce::Path shape;
                    shape.addEllipse (body);
                    juce::DropShadow (juce::Colours::black.withAlpha (0.28f), 28, { 0, 12 }).drawForPath (g, shape);
                    g.setOpacity (1.0f);                                  // the shadow left a translucent colour set
                    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
                    g.drawImage (mark, body);
                }
                juce::FileOutputStream os (dir.getChildFile ("AppIcon.png"));
                os.setPosition (0); os.truncate();
                juce::PNGImageFormat().writeImageToStream (icon, os);
            }
            quit();
            return;
        }

        if (commandLine.contains ("--grid-debug"))
        {
            auto args = juce::StringArray::fromTokens (commandLine, true);
            auto arg = [&args] (const juce::String& key) { auto i = args.indexOf (key); return i >= 0 ? args[i + 1].unquoted() : juce::String(); };
            setApplicationReturnValue (om::gridDebug (juce::File (arg ("--usb")), arg ("--grid-debug")));
            quit();
            return;
        }
        if (commandLine.contains ("--analyzer-benchmark"))
        {
            auto args = juce::StringArray::fromTokens (commandLine, true);
            auto arg = [&args] (const juce::String& key) { auto i = args.indexOf (key); return i >= 0 ? args[i + 1].unquoted() : juce::String(); };
            setApplicationReturnValue (om::runAnalyzerBenchmark (juce::File (arg ("--usb")), juce::jmax (10, arg ("--count").getIntValue()),
                                                                  juce::File (arg ("--report"))));
            quit();
            return;
        }

        if (commandLine.contains ("--audio-test"))
        {
            auto args = juce::StringArray::fromTokens (commandLine, true);
            auto arg = [&args] (const juce::String& key) { auto i = args.indexOf (key); return i >= 0 ? args[i + 1].unquoted() : juce::String(); };
            om::Library::setDataFolderOverride (juce::File (arg ("--data")));
            setApplicationReturnValue (om::runAudioDeviceTest (juce::File (arg ("--usb")), juce::File (arg ("--data"))));
            quit();
            return;
        }

        if (commandLine.contains ("--dump-mapping"))
        {
            // Diagnostics: one line per binding of a mapping file (action, deck, layer, message).
            auto args = juce::StringArray::fromTokens (commandLine, true);
            const auto r = om::MappingImport::fromFile (juce::File (args[args.indexOf ("--dump-mapping") + 1].unquoted()));
            for (auto& b : r.bindings)
                std::cout << (b.action + "|" + juce::String (b.deck) + "|" + (b.shift ? "S" : "-") + "|" + juce::String ((int) b.kind)
                              + "|" + juce::String (b.channel) + "|" + juce::String (b.number) + "|" + juce::String ((int) b.encoding)).toRawUTF8() << std::endl;
            quit();
            return;
        }

        if (commandLine.contains ("--update-install-test"))
        {
            // Diagnostics: full self-update of THIS copy of the app (download, verify, swap, relaunch).
            auto args = juce::StringArray::fromTokens (commandLine, true);
            const auto info = om::Updater::parseManifest (juce::JSON::parse (juce::File (args[args.indexOf ("--update-install-test") + 1].unquoted())));
            static om::Updater u;
            if (info) { u.setAvailable (*info); if (u.downloadNow()) { u.markReady(); u.installAndRelaunch(); return; } }
            std::cout << ("FALLA: " + u.error()).toRawUTF8() << std::endl;
            setApplicationReturnValue (1);
            quit();
            return;
        }

        if (commandLine.contains ("--update-test"))
        {
            // Diagnostics: download + verify + stage the update a manifest describes (does not install).
            auto args = juce::StringArray::fromTokens (commandLine, true);
            const auto mf = juce::File (args[args.indexOf ("--update-test") + 1].unquoted());
            const auto info = om::Updater::parseManifest (juce::JSON::parse (mf));
            if (! info) { std::cout << "manifiesto invalido" << std::endl; setApplicationReturnValue (1); quit(); return; }
            om::Updater u;
            u.setAvailable (*info);
            const bool ok = u.downloadNow();
            std::cout << (ok ? "OK, version " + info->version + " preparada en " + u.stagedApp().getFullPathName() : "FALLA: " + u.error()).toRawUTF8() << std::endl;
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--build-library"))
        {
            auto args = juce::StringArray::fromTokens (commandLine, true);
            const auto i = args.indexOf ("--build-library");
            setApplicationReturnValue (om::buildMappingLibrary (juce::File (args[i + 1].unquoted()), juce::File (args[i + 2].unquoted())));
            quit();
            return;
        }

        if (commandLine.contains ("--simulate-controllers"))
        {
            auto args = juce::StringArray::fromTokens (commandLine, true);
            const auto i = args.indexOf ("--simulate-controllers");
            const auto path = args[i + 1].unquoted();
            // A folder of mapping files to test, or the ones installed on this computer.
            std::vector<om::MappingImport::Source> sources;
            if (juce::File::isAbsolutePath (path) && juce::File (path).isDirectory())
                for (const auto& e : juce::RangedDirectoryIterator (juce::File (path), false, "*.csv;*.xml", juce::File::findFiles))
                    sources.push_back ({ e.getFile().getFileNameWithoutExtension(), e.getFile(), e.getFile() });
            else
                sources = om::MappingImport::findInstalled();
            const auto only = juce::File::isAbsolutePath (path) ? args[i + 2].unquoted() : path;
            setApplicationReturnValue (om::simulateControllers (sources, only.startsWith ("-") ? juce::String() : only));
            quit();
            return;
        }

        if (commandLine.contains ("--import-check"))
        {
            // Diagnostics: translate every controller mapping installed by other DJ software (read-only).
            int total = 0;
            const auto args = juce::StringArray::fromTokens (commandLine, true);
            const auto only = args[args.indexOf ("--import-check") + 1].unquoted();
            for (auto& s : om::MappingImport::findInstalled())
            {
                const auto r = om::MappingImport::convert (s);
                if (only.isNotEmpty() && ! only.startsWith ("-") && s.controller == only)
                    for (auto& b : r.bindings)
                        std::cout << ("  " + b.action + " deck " + juce::String (b.deck) + (b.shift ? " SHIFT" : "") + " · " + b.describeControl()
                                      + " enc " + juce::String ((int) b.encoding) + (b.lsbNumber >= 0 ? " lsb " + juce::String (b.lsbNumber) : juce::String())).toRawUTF8() << std::endl;
                juce::StringArray actions;
                for (auto& b : r.bindings) actions.addIfNotAlreadyThere (b.action);
                std::cout << (s.controller + " | " + juce::String ((int) r.bindings.size()) + " asignaciones, "
                              + juce::String (actions.size()) + " funciones | sin traducir: " + juce::String (r.untranslated.size())).toRawUTF8() << std::endl;
                ++total;
            }
            std::cout << "Mapeos encontrados: " << total << std::endl;
            quit();
            return;
        }

        if (commandLine.contains ("--inspect-usb"))
        {
            auto args = juce::StringArray::fromTokens (commandLine, true);
            const auto i = args.indexOf ("--inspect-usb");
            setApplicationReturnValue (om::inspectUsbExport (juce::File (args[i + 1].unquoted())));
            quit();
            return;
        }

        if (commandLine.contains ("--selftest"))
        {
            auto args = juce::StringArray::fromTokens (commandLine, true);
            auto arg = [&args] (const juce::String& key) { auto i = args.indexOf (key); return i >= 0 ? args[i + 1].unquoted() : juce::String(); };
            if (auto data = arg ("--data"); data.isNotEmpty())
                om::Library::setDataFolderOverride (juce::File (data));
            const int code = om::runSelfTest (juce::File (arg ("--a")), juce::File (arg ("--b")),
                                              arg ("--usb").isNotEmpty() ? juce::File (arg ("--usb")) : juce::File());
            setApplicationReturnValue (code);
            quit();
            return;
        }

        // Headless verification: render the main screen to a PNG and quit.
        if (commandLine.contains ("--snapshot"))
        {
            runSnapshot (commandLine);
            return;
        }

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel = nullptr;
    }

    void systemRequestedQuit() override { quit(); }

    /** Holds the app and scales it as a whole on small screens: below the reference size every
        button, text, jog and waveform shrinks evenly (instead of squeezing the layout), so a
        notebook shows the same full interface — jogs included — just smaller. */
    class ScaledHost : public juce::Component
    {
    public:
        explicit ScaledHost (bool snapshot = false) : content (snapshot) { addAndMakeVisible (content); }
        MainComponent& main() { return content; }

        void resized() override
        {
            constexpr float refW = 1440.0f, refH = 880.0f, minScale = 0.72f;
            const float w = (float) getWidth(), h = (float) getHeight();
            const float scale = juce::jlimit (minScale, 1.0f, juce::jmin (w / refW, h / refH));
            content.setTransform (juce::AffineTransform::scale (scale));
            content.setBounds (0, 0, juce::roundToInt (w / scale), juce::roundToInt (h / scale));
        }

    private:
        MainComponent content;
    };

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, om::colour (om::Col::bgApp), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new ScaledHost(), false);
            setResizable (true, true);
            setResizeLimits (960, 560, 10000, 10000);

            // Restore the last window bounds; first run fills the screen (reference layout).
            const auto saved = stateFile().loadFileAsString();
            if (saved.isEmpty() || ! restoreWindowStateFromString (saved))
                setBounds (juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userBounds.toNearestInt());
            setVisible (true);
        }

        ~MainWindow() override
        {
            stateFile().replaceWithText (getWindowStateAsString());
        }

        static juce::File stateFile() { return om::Library::dataFolder().getChildFile ("window.txt"); }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    void runSnapshot (const juce::String& commandLine)
    {
        auto args = juce::StringArray::fromTokens (commandLine, true);
        auto arg = [&args] (const juce::String& key) { auto i = args.indexOf (key); return i >= 0 ? args[i + 1].unquoted() : juce::String(); };

        const auto out = juce::File (arg ("--snapshot"));
        const auto size = arg ("--size").isNotEmpty() ? arg ("--size") : juce::String ("1440x860");
        const int w = size.upToFirstOccurrenceOf ("x", false, false).getIntValue();
        const int h = size.fromFirstOccurrenceOf ("x", false, false).getIntValue();

        if (auto data = arg ("--data"); data.isNotEmpty())
            om::Library::setDataFolderOverride (juce::File (data));
        if (args.contains ("--no-devices"))
        {
            const auto none = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("openmix-no-volumes");
            none.createDirectory();
            om::DeviceMonitor::setVolumesFolderOverride (none);
        }
        snapshotHost = std::make_unique<ScaledHost> (true);
        snapshotHost->setSize (w, h);
        auto* snapshotComponent = &snapshotHost->main();
        if (auto pg = arg ("--pads"); pg.isNotEmpty())
            snapshotComponent->showPadPageForSnapshot (pg.getIntValue());
        if (auto st = arg ("--settings"); st.isNotEmpty())
        {
            snapshotComponent->showSettingsForSnapshot (st.upToFirstOccurrenceOf (":", false, false).getIntValue(),
                                                        st.fromFirstOccurrenceOf (":", false, false).getIntValue());
            if (st.endsWith (":wizard"))
                snapshotComponent->openMappingWizardForSnapshot();
        }
        snapshotComponent->prepareSnapshot (juce::File (arg ("--load-a")), juce::File (arg ("--load-b")),
                                            [this, out, w, h, automixShot = args.contains ("--automix"), gridShot = args.contains ("--grid-editor")]
        {
            if (automixShot)
            {
                snapshotHost->main().showAutoMixForSnapshot();
            }
            std::unique_ptr<juce::Component> gridEd;
            if (gridShot) { gridEd = snapshotHost->main().gridEditorForSnapshot(); }
            auto img = gridEd != nullptr ? gridEd->createComponentSnapshot (gridEd->getLocalBounds(), true, 2.0f)
                                         : snapshotHost->createComponentSnapshot ({ 0, 0, w, h }, true, 2.0f);
            out.deleteFile();
            juce::FileOutputStream stream (out);
            juce::PNGImageFormat().writeImageToStream (img, stream);
            stream.flush();
            snapshotHost = nullptr;
            quit();
        }, arg ("--device-playlist"));
    }

    std::unique_ptr<om::LookAndFeel> lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<ScaledHost> snapshotHost;
};

START_JUCE_APPLICATION (OpenMixApplication)
