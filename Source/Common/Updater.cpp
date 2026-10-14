#include "Updater.h"
#include <juce_cryptography/juce_cryptography.h>
#include <juce_gui_basics/juce_gui_basics.h>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <unistd.h>
#endif

namespace om
{

static constexpr const char* defaultManifest = "https://github.com/clipxel/OpenMix/releases/latest/download/update.json";

Updater::Updater()
{
    workDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("OpenMix-update");
}

Updater::~Updater()
{
    alive->store (false);
    pool.removeAllJobs (true, 5000);
}

juce::String Updater::currentVersion()
{
   #ifdef JUCE_APPLICATION_VERSION_STRING
    return JUCE_APPLICATION_VERSION_STRING;
   #else
    return "0.0.0";
   #endif
}

int Updater::compareVersions (const juce::String& a, const juce::String& b)
{
    auto pa = juce::StringArray::fromTokens (a.trim().trimCharactersAtStart ("vV"), ".", "");
    auto pb = juce::StringArray::fromTokens (b.trim().trimCharactersAtStart ("vV"), ".", "");
    for (int i = 0; i < juce::jmax (pa.size(), pb.size()); ++i)
    {
        const int x = pa[i].getIntValue(), y = pb[i].getIntValue();
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

std::optional<Updater::Info> Updater::parseManifest (const juce::var& m)
{
   #if JUCE_MAC
    const auto& a = m["assets"]["mac"];
   #elif JUCE_WINDOWS
    // The installer when the manifest has it; "windows" is the .zip older versions understand.
    const auto& a = m["assets"].hasProperty ("windows_setup") ? m["assets"]["windows_setup"] : m["assets"]["windows"];
   #else
    const auto& a = m["assets"]["linux"];
   #endif
    Info i;
    i.version = m["version"].toString();
    i.notes = m["notes"].toString();
    i.url = a["url"].toString();
    i.sha256 = a["sha256"].toString().toLowerCase();
    i.file = a["file"].toString();
    i.size = (juce::int64) a["size"];
    const bool safeUrl = i.url.startsWithIgnoreCase ("https://") || i.url.startsWithIgnoreCase ("file:");
    if (i.version.isEmpty() || ! safeUrl || i.sha256.length() != 64 || i.file.isEmpty() || i.file.containsAnyOf ("/\\"))
        return std::nullopt;
    return i;
}

static juce::String fetch (const juce::URL& url, juce::String& error)
{
    if (url.isLocalFile()) return url.getLocalFile().loadFileAsString();
    int status = 0;
    auto in = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                         .withConnectionTimeoutMs (8000).withNumRedirectsToFollow (5).withStatusCode (&status));
    if (in == nullptr || (status != 0 && status != 200)) { error = "Sin conexión"; return {}; }
    return in->readEntireStreamAsString();
}

void Updater::checkForUpdates()
{
    if (current == State::checking || current == State::downloading) return;
    current = State::checking;
    auto flag = alive;
    pool.addJob ([this, flag]
    {
        juce::String err;
        const auto url = ! manifestOverride().isEmpty() ? manifestOverride() : juce::URL (defaultManifest);
        const auto parsed = parseManifest (juce::JSON::parse (fetch (url, err)));
        juce::MessageManager::callAsync ([this, flag, parsed, err]
        {
            if (! flag->load()) return;
            if (parsed && compareVersions (parsed->version, currentVersion()) > 0)
            {
                info = *parsed;
                current = State::available;
            }
            else
            {
                lastError = err;
                current = State::upToDate;                   // offline counts as "nothing new": no nagging
            }
            sendChangeMessage();
        });
    });
}

void Updater::download()
{
    if (current != State::available && current != State::failed) return;
    current = State::downloading;
    prog = 0.0f;
    sendChangeMessage();
    auto flag = alive;
    pool.addJob ([this, flag]
    {
        const bool ok = downloadNow();
        juce::MessageManager::callAsync ([this, flag, ok]
        {
            if (! flag->load()) return;
            current = ok ? State::ready : State::failed;
            sendChangeMessage();
        });
    });
}

bool Updater::downloadNow()
{
    workDir.deleteRecursively();
    workDir.createDirectory();
    const auto package = workDir.getChildFile (info.file);

    // Download (with progress), then check it is exactly the published file.
    const juce::URL url (info.url);
    if (url.isLocalFile())
        url.getLocalFile().copyFileTo (package);
    else
    {
        int status = 0;
        auto in = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                             .withConnectionTimeoutMs (15000).withNumRedirectsToFollow (5).withStatusCode (&status));
        if (in == nullptr || (status != 0 && status != 200)) { lastError = juce::String::fromUTF8 ("No se pudo descargar la actualización"); return false; }
        juce::FileOutputStream out (package);
        if (! out.openedOk()) { lastError = "No se pudo guardar la descarga"; return false; }
        const auto total = info.size > 0 ? info.size : in->getTotalLength();
        juce::HeapBlock<char> buf (65536);
        juce::int64 done = 0;
        for (;;)
        {
            if (! alive->load()) return false;
            const int n = in->read (buf, 65536);
            if (n <= 0) break;
            out.write (buf, (size_t) n);
            done += n;
            if (total > 0) prog = (float) juce::jlimit (0.0, 1.0, (double) done / (double) total);
        }
    }

    juce::FileInputStream check (package);
    if (! check.openedOk() || ! juce::SHA256 (check).toHexString().equalsIgnoreCase (info.sha256))
    {
        lastError = juce::String::fromUTF8 ("La descarga no coincide con la versión publicada: no se instaló");
        package.deleteFile();
        return false;
    }
    prog = 1.0f;
    return stage (package);
}

static bool run (const juce::StringArray& args, int timeoutMs = 120000)
{
    juce::ChildProcess p;
    return p.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)
        && p.waitForProcessToFinish (timeoutMs) && p.getExitCode() == 0;
}

bool Updater::stage (const juce::File& package)
{
    const auto stageDir = workDir.getChildFile ("new");
    stageDir.createDirectory();
   #if JUCE_MAC
    // Mount the disk image read-only, copy the app out, unmount.
    const auto mount = workDir.getChildFile ("mnt");
    mount.createDirectory();
    if (! run ({ "/usr/bin/hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", mount.getFullPathName(), package.getFullPathName() }))
    {
        lastError = "No se pudo abrir la imagen de la actualización";
        return false;
    }
    const auto app = mount.getChildFile ("OpenMix.app");
    const bool copied = app.isDirectory() && run ({ "/usr/bin/ditto", app.getFullPathName(), stageDir.getChildFile ("OpenMix.app").getFullPathName() });
    run ({ "/usr/bin/hdiutil", "detach", mount.getFullPathName(), "-force" });
    staged = stageDir.getChildFile ("OpenMix.app");
    if (! copied || ! staged.isDirectory()) { lastError = "La imagen no contiene la app"; return false; }
    return true;
   #elif JUCE_WINDOWS
    if (package.hasFileExtension (".exe"))
    {
        staged = package;                                   // the installer itself: it runs after OpenMix quits
        return true;
    }
    juce::ZipFile zip (package);
    if (zip.getNumEntries() == 0 || zip.uncompressTo (stageDir).failed()) { lastError = "No se pudo descomprimir la actualización"; return false; }
    staged = stageDir.getChildFile ("OpenMix");
    if (! staged.getChildFile ("OpenMix.exe").existsAsFile()) { lastError = "El paquete no contiene OpenMix.exe"; return false; }
    return true;
   #else
    if (! run ({ "tar", "-xzf", package.getFullPathName(), "-C", stageDir.getFullPathName() }))
    {
        lastError = "No se pudo descomprimir la actualización";
        return false;
    }
    staged = stageDir.getChildFile ("OpenMix");
    if (! staged.getChildFile ("openmix").existsAsFile()) { lastError = "El paquete no contiene OpenMix"; return false; }
    return true;
   #endif
}

void Updater::installAndRelaunch()
{
    if (current != State::ready) return;
    const auto pid = juce::String ((juce::int64)
                                   #if JUCE_WINDOWS
                                    GetCurrentProcessId()
                                   #else
                                    getpid()
                                   #endif
                                   );
   #if JUCE_MAC
    const auto target = juce::File::getSpecialLocation (juce::File::currentApplicationFile);   // …/OpenMix.app
    if (! target.getParentDirectory().hasWriteAccess())
    {
        lastError = juce::String::fromUTF8 ("No hay permiso para reemplazar la app en ") + target.getParentDirectory().getFullPathName();
        current = State::failed;
        sendChangeMessage();
        return;
    }
    const auto script = workDir.getChildFile ("install.sh");
    script.replaceWithText ("#!/bin/sh\n"
        "set -x\n"
        "while kill -0 " + pid + " 2>/dev/null; do sleep 0.2; done\n"
        "T=" + target.getFullPathName().quoted() + "\n"
        "rm -rf \"$T.old\"\n"
        "if mv \"$T\" \"$T.old\" && /usr/bin/ditto " + staged.getFullPathName().quoted() + " \"$T\"; then rm -rf \"$T.old\"; "
        "else rm -rf \"$T\"; mv \"$T.old\" \"$T\"; fi\n"
        "/usr/bin/xattr -dr com.apple.quarantine \"$T\" 2>/dev/null\n"
        "/usr/bin/open \"$T\"\n", false, false, "\n");          // Unix line endings: sh does not take \r\n
    juce::ChildProcess launcher;
    launcher.start (juce::StringArray { "/bin/sh", "-c", "nohup /bin/sh " + script.getFullPathName().quoted() + " >"
                                        + workDir.getChildFile ("install.log").getFullPathName().quoted() + " 2>&1 &" });
    launcher.waitForProcessToFinish (5000);
   #elif JUCE_WINDOWS
    if (staged.hasFileExtension (".exe"))
    {
        // Installer: per-user, silent, then open the installed OpenMix.
        const auto installed = juce::File::getSpecialLocation (juce::File::windowsLocalAppData)
                                 .getChildFile ("Programs").getChildFile ("OpenMix").getChildFile ("OpenMix.exe");
        const auto script = workDir.getChildFile ("install.cmd");
        script.replaceWithText ("@echo off\r\n"
            ":wait\r\n"
            "tasklist /FI \"PID eq " + pid + "\" | find \"" + pid + "\" >nul && (timeout /t 1 /nobreak >nul & goto wait)\r\n"
            "\"" + staged.getFullPathName() + "\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS\r\n"
            "start \"\" \"" + installed.getFullPathName() + "\"\r\n");
        script.startAsProcess();
        juce::JUCEApplicationBase::quit();
        return;
    }
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    const auto dir = exe.getParentDirectory();
    if (! dir.hasWriteAccess())
    {
        lastError = juce::String::fromUTF8 ("No hay permiso para reemplazar OpenMix en ") + dir.getFullPathName();
        current = State::failed;
        sendChangeMessage();
        return;
    }
    const auto script = workDir.getChildFile ("install.cmd");
    script.replaceWithText ("@echo off\r\n"
        ":wait\r\n"
        "tasklist /FI \"PID eq " + pid + "\" | find \"" + pid + "\" >nul && (timeout /t 1 /nobreak >nul & goto wait)\r\n"
        "xcopy /E /Y /Q \"" + staged.getFullPathName() + "\\*\" \"" + dir.getFullPathName() + "\\\" >nul\r\n"
        "start \"\" \"" + exe.getFullPathName() + "\"\r\n");
    script.startAsProcess();
   #else
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    if (! exe.getParentDirectory().hasWriteAccess())
    {
        lastError = juce::String::fromUTF8 ("No hay permiso para reemplazar OpenMix en ") + exe.getParentDirectory().getFullPathName();
        current = State::failed;
        sendChangeMessage();
        return;
    }
    const auto script = workDir.getChildFile ("install.sh");
    script.replaceWithText ("#!/bin/sh\n"
        "while kill -0 " + pid + " 2>/dev/null; do sleep 0.2; done\n"
        "cp " + staged.getChildFile ("openmix").getFullPathName().quoted() + " " + exe.getFullPathName().quoted() + "\n"
        "chmod +x " + exe.getFullPathName().quoted() + "\n"
        "nohup " + exe.getFullPathName().quoted() + " >/dev/null 2>&1 &\n", false, false, "\n");
    juce::ChildProcess launcher;
    launcher.start (juce::StringArray { "/bin/sh", "-c", "nohup /bin/sh " + script.getFullPathName().quoted() + " >/dev/null 2>&1 &" });
    launcher.waitForProcessToFinish (5000);
   #endif
    juce::JUCEApplicationBase::quit();
}

} // namespace om
