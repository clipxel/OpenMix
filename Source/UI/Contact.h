#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <thread>

namespace om::Contact
{

/** Where users reach us (top bar, Settings › Ayuda, the goodbye card). One place to change them. */
inline const char* community()     { return "https://chat.whatsapp.com/EfyHSpbE1KGJ3zcCKjhx4R"; }
inline const char* instagram()     { return "https://www.instagram.com/nanobrunetti/"; }
inline const char* instagramChat() { return "https://ig.me/m/nanobrunetti"; }
inline const char* site()          { return "https://clipxel.github.io/OpenMix/"; }
inline const char* handle()        { return "@nanobrunetti"; }
inline const char* donate()        { return "https://onetouch.astropay.com/payment?external_reference_id=AERRP6sKfsrGdRunEEa6gJ7V5fQ1E0gO"; }
inline juce::String donateNote()   { return juce::String::fromUTF8 ("OpenMix es gratis y siempre lo va a ser. Tu aporte nos ayuda a seguir mejorándolo."); }

inline juce::String about()
{
    return "OpenMix " + juce::JUCEApplication::getInstance()->getApplicationVersion() + " · " + juce::SystemStats::getOperatingSystemName();
}

inline void open (const char* url) { juce::URL (url).launchInDefaultBrowser(); }

/** A ready message in the clipboard (version + system) and the chat opens: paste, tell, send. */
inline void sendReport (const juce::String& intro = juce::String::fromUTF8 ("Qué pasó / qué te gustaría:"))
{
    juce::SystemClipboard::copyTextToClipboard ("OpenMix\n" + about() + "\n\n" + intro + "\n");
    open (instagramChat());
}

/** Sends a message to the OpenMix feedback form (Google Forms: "Reportes OpenMix") on a background
    thread; `done (ok)` runs on the message thread. Only what the user wrote plus version / system. */
inline void submitFeedback (const juce::String& message, std::function<void (bool)> done)
{
    const auto info = about() + " · " + juce::SystemStats::getDeviceDescription() + " · " + juce::SystemStats::getUserLanguage();
    std::thread ([message, info, done]
    {
        const auto url = juce::URL ("https://docs.google.com/forms/d/e/1FAIpQLSdSf8hY9KvOsCrz8WXr0-ByGoLaHjSbbuH3lAZJqi7D9_Bvsg/formResponse")
                             .withParameter ("entry.1136859481", message)
                             .withParameter ("entry.1953322693", info);
        int status = 0;
        auto in = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                                             .withConnectionTimeoutMs (10000)
                                             .withStatusCode (&status));
        const bool ok = in != nullptr && (status == 200 || status == 0);
        juce::MessageManager::callAsync ([done, ok] { if (done) done (ok); });
    }).detach();
}

} // namespace om::Contact
