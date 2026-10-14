#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "UI/Theme/LookAndFeel.h"
#include "Library/Library.h"
#include "Tests/SelfTest.h"
#include "Control/MappingImport.h"
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
        return cl.contains ("--snapshot") || cl.contains ("--selftest") || cl.contains ("--inspect-usb") || cl.contains ("--audio-test") || cl.contains ("--analyzer-benchmark") || cl.contains ("--make-icons") || cl.contains ("--import-check") || cl.contains ("--simulate-controllers");
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

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, om::colour (om::Col::bgApp), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (1024, 640, 10000, 10000);

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
        snapshotComponent = std::make_unique<MainComponent> (true);
        snapshotComponent->setSize (w, h);
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
                                            [this, out, w, h]
        {
            auto img = snapshotComponent->createComponentSnapshot ({ 0, 0, w, h }, true, 2.0f);
            out.deleteFile();
            juce::FileOutputStream stream (out);
            juce::PNGImageFormat().writeImageToStream (img, stream);
            stream.flush();
            snapshotComponent = nullptr;
            quit();
        }, arg ("--device-playlist"));
    }

    std::unique_ptr<om::LookAndFeel> lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<MainComponent> snapshotComponent;
};

START_JUCE_APPLICATION (OpenMixApplication)
