#include "GoogleDrive.h"
#include "Library.h"
#include <juce_cryptography/juce_cryptography.h>
#include "GoogleDriveClient.h"

namespace om
{

namespace
{
constexpr const char* scope = "https://www.googleapis.com/auth/drive.readonly";   // see and play, never change
constexpr const char* api = "https://www.googleapis.com/drive/v3/";

juce::String esc (const juce::String& s) { return juce::URL::addEscapeChars (s, true); }

juce::String base64Url (const void* data, size_t size)
{
    return juce::Base64::toBase64 (data, size).replaceCharacter ('+', '-').replaceCharacter ('/', '_').trimCharactersAtEnd ("=");
}

juce::String randomVerifier()
{
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    juce::Random r (juce::Time::getHighResolutionTicks() ^ (juce::int64) juce::Time::currentTimeMillis());
    juce::String s;
    for (int i = 0; i < 64; ++i) s << chars[r.nextInt (66)];
    return s;
}

/** A POST to Google's token endpoint; the JSON reply or an error. */
juce::var postForm (const juce::String& url, const juce::String& body, juce::String& error)
{
    auto u = juce::URL (url).withPOSTData (body);
    int status = 0;
    auto in = u.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                                       .withExtraHeaders ("Content-Type: application/x-www-form-urlencoded")
                                       .withConnectionTimeoutMs (15000)
                                       .withStatusCode (&status));
    if (in == nullptr) { error = "Sin conexión con Google"; return {}; }
    const auto reply = juce::JSON::parse (in->readEntireStreamAsString());
    if (status >= 400) error = reply["error_description"].toString().isNotEmpty() ? reply["error_description"].toString() : "Google rechazó el pedido (" + juce::String (status) + ")";
    return reply;
}
} // namespace

bool GoogleDrive::builtIn()  { return juce::String (OPENMIX_GOOGLE_CLIENT_ID).isNotEmpty(); }

GoogleDrive::GoogleDrive()  { load(); }
GoogleDrive::~GoogleDrive() { *alive = false; pool.removeAllJobs (true, 3000); }

juce::File GoogleDrive::cacheFolder()
{
    auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("OpenMix Drive");
    f.createDirectory();
    return f;
}

bool GoogleDrive::isAudioName (const juce::String& name)
{
    static const juce::StringArray ext { ".mp3", ".wav", ".aif", ".aiff", ".flac", ".m4a", ".aac", ".ogg", ".alac", ".wma" };
    for (auto& e : ext) if (name.endsWithIgnoreCase (e)) return true;
    return false;
}

void GoogleDrive::load()
{
    const auto v = juce::JSON::parse (Library::dataFolder().getChildFile ("drive.json"));
    clientId = v["client_id"].toString();
    clientSecret = v["client_secret"].toString();
    if (builtIn())                              // OpenMix's own client: "Iniciar sesión con Google" in one click
    {
        clientId = OPENMIX_GOOGLE_CLIENT_ID;
        clientSecret = OPENMIX_GOOGLE_CLIENT_SECRET;
    }
    refreshToken = v["refresh_token"].toString();
    accountEmail = v["account"].toString();
    linked.clear();
    if (auto* arr = v["folders"].getArray())
        for (auto& f : *arr) linked.push_back ({ f["id"].toString(), f["name"].toString() });
}

void GoogleDrive::save() const
{
    auto o = std::make_unique<juce::DynamicObject>();
    o->setProperty ("client_id", clientId);
    o->setProperty ("client_secret", clientSecret);
    o->setProperty ("refresh_token", refreshToken);
    o->setProperty ("account", accountEmail);
    juce::Array<juce::var> fs;
    for (auto& f : linked)
    {
        auto x = std::make_unique<juce::DynamicObject>();
        x->setProperty ("id", f.id);
        x->setProperty ("name", f.name);
        fs.add (juce::var (x.release()));
    }
    o->setProperty ("folders", fs);
    Library::dataFolder().getChildFile ("drive.json").replaceWithText (juce::JSON::toString (juce::var (o.release())));
}

void GoogleDrive::setClient (const juce::String& id, const juce::String& secret)
{
    clientId = id.trim();
    clientSecret = secret.trim();
    save();
}

void GoogleDrive::signOut()
{
    const juce::ScopedLock sl (lock);
    refreshToken = token = accountEmail = {};
    tokenExpiresMs = 0.0;
    save();
}

void GoogleDrive::signIn (std::function<void (juce::String)> done)
{
    pool.addJob ([this, done, alive = std::weak_ptr<bool> (alive)]
    {
        auto finish = [done, alive] (juce::String err)
        {
            juce::MessageManager::callAsync ([done, alive, err] { if (! alive.expired() && done) done (err); });
        };

        // A one-time local address the browser comes back to.
        juce::StreamingSocket server;
        if (! server.createListener (0, "127.0.0.1")) { finish ("No se pudo abrir el puerto local para el inicio de sesión"); return; }
        const auto redirect = "http://127.0.0.1:" + juce::String (server.getBoundPort());

        const auto verifier = randomVerifier();
        const juce::SHA256 sha (verifier.toRawUTF8(), verifier.getNumBytesAsUTF8());
        const auto digest = sha.getRawData();
        const auto challenge = base64Url (digest.getData(), digest.getSize());

        const auto authUrl = juce::String ("https://accounts.google.com/o/oauth2/v2/auth?client_id=") + esc (clientId)
                           + "&redirect_uri=" + esc (redirect) + "&response_type=code&scope=" + esc (scope)
                           + "&code_challenge=" + challenge + "&code_challenge_method=S256&access_type=offline&prompt=consent";
        juce::MessageManager::callAsync ([authUrl] { juce::URL (authUrl).launchInDefaultBrowser(); });

        // Wait up to 5 minutes for the browser to come back.
        juce::String code, problem;
        for (int waited = 0; waited < 300 && code.isEmpty() && problem.isEmpty(); ++waited)
        {
            if (alive.expired()) return;
            if (server.waitUntilReady (true, 1000) != 1) continue;
            std::unique_ptr<juce::StreamingSocket> c (server.waitForNextConnection());
            if (c == nullptr) continue;
            // The browser may connect before it sends anything (pre-connections), and the request may
            // arrive in pieces: read until the end of the headers, giving up on an idle connection.
            juce::MemoryBlock got;
            while (got.getSize() < 16384)
            {
                if (c->waitUntilReady (true, 3000) != 1) break;
                char buf[4096];
                const int n = c->read (buf, (int) sizeof (buf), false);
                if (n <= 0) break;
                got.append (buf, (size_t) n);
                if (got.toString().contains ("\r\n\r\n")) break;
            }
            const auto request = got.toString();
            const auto target = request.fromFirstOccurrenceOf ("GET ", false, false).upToFirstOccurrenceOf (" ", false, false);
            if (! target.startsWith ("/?"))
            {
                // Empty pre-connections, /favicon.ico…: not the answer from Google.
                if (target.isNotEmpty())
                {
                    const juce::String none = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    c->write (none.toRawUTF8(), (int) none.getNumBytesAsUTF8());
                }
                continue;
            }
            const juce::URL back (redirect + target);
            const auto names = back.getParameterNames(), values = back.getParameterValues();
            for (int i = 0; i < names.size(); ++i)
            {
                if (names[i] == "code") code = values[i];
                if (names[i] == "error") problem = values[i] == "access_denied" ? "Google no dio acceso (¿la cuenta está entre los usuarios de prueba?)" : values[i];
            }
            if (code.isEmpty() && problem.isEmpty()) continue;
            const juce::String page = juce::String::fromUTF8 ("<html><meta charset='utf-8'><body style='font-family:sans-serif;padding:40px'><h2>")
                                    + (code.isNotEmpty() ? juce::String::fromUTF8 ("Listo: OpenMix ya puede ver tu Google Drive.") : juce::String::fromUTF8 ("No se completó el inicio de sesión."))
                                    + juce::String::fromUTF8 ("</h2><p>Podés cerrar esta pestaña y volver a OpenMix.</p></body></html>");
            const auto reply = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
                             + juce::String ((int) page.getNumBytesAsUTF8()) + "\r\nConnection: close\r\n\r\n" + page;
            c->write (reply.toRawUTF8(), (int) reply.getNumBytesAsUTF8());
        }
        server.close();
        if (code.isEmpty()) { finish (problem.isNotEmpty() ? problem : "Se agotó el tiempo para iniciar sesión"); return; }

        juce::String err;
        const auto tok = postForm ("https://oauth2.googleapis.com/token",
                                   "code=" + esc (code) + "&client_id=" + esc (clientId) + "&client_secret=" + esc (clientSecret)
                                   + "&redirect_uri=" + esc (redirect) + "&grant_type=authorization_code&code_verifier=" + verifier, err);
        if (err.isNotEmpty() || tok["refresh_token"].toString().isEmpty()) { finish (err.isNotEmpty() ? err : "Google no devolvió el permiso"); return; }
        {
            const juce::ScopedLock sl (lock);
            refreshToken = tok["refresh_token"].toString();
            token = tok["access_token"].toString();
            tokenExpiresMs = juce::Time::getMillisecondCounterHiRes() + ((double) tok["expires_in"] - 60.0) * 1000.0;
        }
        juce::String e2;
        const auto about = getJson (juce::String (api) + "about?fields=user(emailAddress)", e2);
        accountEmail = about["user"]["emailAddress"].toString();
        juce::MessageManager::callAsync ([this, alive] { if (! alive.expired()) save(); });
        finish ({});
    });
}

juce::String GoogleDrive::accessToken()
{
    {
        const juce::ScopedLock sl (lock);
        if (token.isNotEmpty() && juce::Time::getMillisecondCounterHiRes() < tokenExpiresMs) return token;
        if (refreshToken.isEmpty()) return {};
    }
    juce::String err;
    const auto tok = postForm ("https://oauth2.googleapis.com/token",
                               "client_id=" + esc (clientId) + "&client_secret=" + esc (clientSecret)
                               + "&refresh_token=" + esc (refreshToken) + "&grant_type=refresh_token", err);
    const juce::ScopedLock sl (lock);
    if (err.isNotEmpty()) return {};
    token = tok["access_token"].toString();
    tokenExpiresMs = juce::Time::getMillisecondCounterHiRes() + ((double) tok["expires_in"] - 60.0) * 1000.0;
    return token;
}

juce::var GoogleDrive::getJson (const juce::String& url, juce::String& error)
{
    const auto t = accessToken();
    if (t.isEmpty()) { error = "Conectá Google Drive de nuevo"; return {}; }
    int status = 0;
    auto in = juce::URL (url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                     .withExtraHeaders ("Authorization: Bearer " + t)
                                                     .withConnectionTimeoutMs (20000)
                                                     .withStatusCode (&status));
    if (in == nullptr) { error = "Sin conexión con Google Drive"; return {}; }
    const auto v = juce::JSON::parse (in->readEntireStreamAsString());
    if (status == 404) error = juce::String::fromUTF8 ("No se encontró (o tu cuenta no tiene acceso)");
    else if (status >= 400) error = v["error"]["message"].toString().isNotEmpty() ? v["error"]["message"].toString() : "Error de Google Drive (" + juce::String (status) + ")";
    return v;
}

juce::String GoogleDrive::idFromAddress (const juce::String& address)
{
    const auto a = address.trim();
    if (a.contains ("/folders/")) return a.fromFirstOccurrenceOf ("/folders/", false, false).upToFirstOccurrenceOf ("?", false, false).upToFirstOccurrenceOf ("/", false, false);
    if (a.contains ("id=")) return a.fromFirstOccurrenceOf ("id=", false, false).upToFirstOccurrenceOf ("&", false, false);
    if (a.contains ("/d/")) return a.fromFirstOccurrenceOf ("/d/", false, false).upToFirstOccurrenceOf ("/", false, false);
    return a.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ? a : juce::String();
}

juce::StringArray GoogleDrive::linksIn (const juce::String& text)
{
    juce::StringArray out;
    for (auto t : juce::StringArray::fromTokens (text, " \t\r\n<>\"'", ""))
    {
        t = t.trim().trimCharactersAtEnd (".,;)]");
        if (t.startsWithIgnoreCase ("http://") || t.startsWithIgnoreCase ("https://"))
            out.addIfNotAlreadyThere (t);
    }
    return out;
}

/** A shortened address (TinyURL, bit.ly…): where it points, following up to 5 redirects. */
juce::String GoogleDrive::followShortLink (juce::String address)
{
    for (int hop = 0; hop < 5 && ! address.containsIgnoreCase ("drive.google.com"); ++hop)
    {
        juce::StringPairArray headers;
        int status = 0;
        auto in = juce::URL (address).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                             .withHttpRequestCmd ("HEAD")
                                                             .withNumRedirectsToFollow (0)
                                                             .withConnectionTimeoutMs (10000)
                                                             .withResponseHeaders (&headers)
                                                             .withStatusCode (&status));
        juce::String next;
        for (auto& k : headers.getAllKeys())
            if (k.equalsIgnoreCase ("location")) next = headers[k].upToFirstOccurrenceOf (",", false, false).trim();
        if (next.isEmpty() || status < 300 || status >= 400) break;
        address = next;
    }
    return address;
}

void GoogleDrive::linkFolder (const juce::String& address, std::function<void (juce::String)> done)
{
    auto id = idFromAddress (address);
    const bool shortLink = id.isEmpty() && address.trim().startsWithIgnoreCase ("http") && ! address.containsIgnoreCase ("google.com");
    if (id.isEmpty() && ! shortLink) { done (juce::String::fromUTF8 ("Ese enlace no parece de una carpeta de Google Drive")); return; }
    pool.addJob ([this, id, address, done, alive = std::weak_ptr<bool> (alive)]
    {
        juce::String err;
        auto folderId = id;
        if (folderId.isEmpty())
        {
            folderId = idFromAddress (followShortLink (address.trim()));
            if (folderId.isEmpty())
            {
                juce::MessageManager::callAsync ([alive, done]
                {
                    if (! alive.expired() && done) done (juce::String::fromUTF8 ("El enlace acortado no lleva a una carpeta de Google Drive"));
                });
                return;
            }
        }
        const auto v = getJson (juce::String (api) + "files/" + esc (folderId) + "?fields=id,name,mimeType&supportsAllDrives=true", err);
        const auto name = v["name"].toString();
        if (err.isEmpty() && v["mimeType"].toString() != "application/vnd.google-apps.folder") err = "Ese enlace no es de una carpeta";
        juce::MessageManager::callAsync ([this, alive, done, id = folderId, name, err]
        {
            if (alive.expired()) return;
            if (err.isEmpty() && std::none_of (linked.begin(), linked.end(), [&] (const Folder& f) { return f.id == id; }))
            {
                linked.push_back ({ id, name });
                save();
            }
            if (done) done (err);
        });
    });
}

void GoogleDrive::unlinkFolder (const juce::String& id)
{
    linked.erase (std::remove_if (linked.begin(), linked.end(), [&] (const Folder& f) { return f.id == id; }), linked.end());
    setIndex (id, nullptr);
    { const juce::ScopedLock sl (indexLock); indexes.erase (id); }
    indexFile (id).deleteFile();
    save();
}

//==============================================================================
bool GoogleDrive::Index::contains (const juce::String& folderId, const juce::String& under) const
{
    auto f = folderId;
    for (int guard = 0; guard < 64 && f.isNotEmpty(); ++guard)
    {
        if (f == under) return true;
        const auto it = folders.find (f);
        if (it == folders.end()) return false;
        f = it->second.first;
    }
    return false;
}

juce::String GoogleDrive::Index::pathOf (const juce::String& folderId, const juce::String& rootId) const
{
    juce::StringArray parts;
    auto f = folderId;
    for (int guard = 0; guard < 64 && f.isNotEmpty() && f != rootId; ++guard)
    {
        const auto it = folders.find (f);
        if (it == folders.end()) break;
        parts.insert (0, it->second.second);
        f = it->second.first;
    }
    return parts.joinIntoString (" / ");
}

juce::File GoogleDrive::indexFile (const juce::String& rootId)
{
    return Library::dataFolder().getChildFile ("drive-index").getChildFile (juce::File::createLegalFileName (rootId) + ".json");
}

std::shared_ptr<const GoogleDrive::Index> GoogleDrive::index (const juce::String& rootId)
{
    {
        const juce::ScopedLock sl (indexLock);
        if (auto it = indexes.find (rootId); it != indexes.end()) return it->second;
    }
    // Reading a saved index (tens of MB) must never hold the screen: it is read in the background.
    if (juce::MessageManager::existsAndIsCurrentThread() && indexFile (rootId).existsAsFile() && indexLoading.insert (rootId).second)
        indexPool.addJob ([this, rootId, alive = std::weak_ptr<bool> (alive)]
        {
            const auto v = juce::JSON::parse (indexFile (rootId));
            auto x = std::make_shared<Index>();
            x->builtAt = (juce::int64) v["built"];
            if (auto* fs = v["folders"].getArray())
                for (auto& f : *fs) x->folders[f[0].toString()] = { f[1].toString(), f[2].toString() };
            if (auto* fs = v["files"].getArray())
            {
                x->files.reserve ((size_t) fs->size());
                for (auto& f : *fs)
                    x->files.push_back ({ Entry { f[0].toString(), f[1].toString(), f[2].toString(), f[3].toString().getLargeIntValue(), false }, f[4].toString() });
            }
            juce::MessageManager::callAsync ([this, alive, rootId, x]
            {
                if (alive.expired()) return;
                indexLoading.erase (rootId);
                {
                    const juce::ScopedLock sl (indexLock);
                    if (indexes.count (rootId)) return;          // a newer one (being built) is already there
                }
                setIndex (rootId, x);
                if (onIndexChanged) onIndexChanged (rootId);
            });
        });
    return nullptr;
}

bool GoogleDrive::hasIndex (const juce::String& rootId) const
{
    {
        const juce::ScopedLock sl (indexLock);
        if (indexes.count (rootId)) return true;
    }
    return indexLoading.count (rootId) > 0 || indexFile (rootId).existsAsFile();
}

void GoogleDrive::setIndex (const juce::String& rootId, std::shared_ptr<const Index> x)
{
    std::shared_ptr<const Index> old;
    {
        const juce::ScopedLock sl (indexLock);
        auto& slot = indexes[rootId];
        old = std::move (slot);
        slot = std::move (x);
    }
    // Freeing hundreds of thousands of entries takes a while: never on the screen's thread.
    if (old != nullptr && old.use_count() == 1)
        indexPool.addJob ([o = std::move (old)]() mutable { o.reset(); });
}

void GoogleDrive::buildIndex (const juce::String& rootId, std::function<void (int)> progress, std::function<void (juce::String)> done)
{
    if (rootId.isEmpty() || ! indexing.insert (rootId).second) return;
    indexPool.addJob ([this, rootId, progress, done, alive = std::weak_ptr<bool> (alive)]
    {
        auto x = std::make_shared<Index>();
        juce::CriticalSection xl;                   // x, next, seen, err
        juce::String err;
        std::vector<juce::String> level { rootId };
        std::set<juce::String> seen { rootId };
        std::atomic<int> found { 0 };
        double lastSnapshot = juce::Time::getMillisecondCounterHiRes();

        // What has been found so far, so the search shows results while the rest is still coming.
        auto publish = [&]
        {
            std::shared_ptr<Index> snap;
            { const juce::ScopedLock sl (xl); snap = std::make_shared<Index> (*x); }
            const int n = found.load();
            juce::MessageManager::callAsync ([this, alive, progress, rootId, snap, n]
            {
                if (alive.expired()) return;
                setIndex (rootId, snap);
                if (progress) progress (n);
            });
        };

        // One group of folders ("('a' in parents or 'b' in parents …)"), every page, retrying when Google is busy.
        auto ask = [&] (std::vector<juce::String> group, std::vector<juce::String>& next)
        {
            juce::StringArray ors;
            for (auto& id : group) ors.add ("'" + id + "' in parents");
            const auto q = "(" + ors.joinIntoString (" or ") + ") and trashed=false";
            const std::set<juce::String> inGroup (group.begin(), group.end());
            juce::String pageToken;
            do
            {
                auto url = juce::String (api) + "files?q=" + esc (q)
                         + "&fields=" + esc ("nextPageToken,files(id,name,mimeType,size,parents,shortcutDetails(targetId,targetMimeType))")
                         + "&pageSize=1000&supportsAllDrives=true&includeItemsFromAllDrives=true";
                if (pageToken.isNotEmpty()) url << "&pageToken=" << esc (pageToken);
                juce::var v;
                juce::String e1;
                for (int attempt = 0; attempt < 4; ++attempt)
                {
                    e1 = {};
                    v = getJson (url, e1);
                    if (e1.isEmpty() || alive.expired()) break;
                    juce::Thread::sleep (500 << attempt);
                }
                if (e1.isNotEmpty()) { const juce::ScopedLock sl (xl); if (err.isEmpty()) err = e1; return; }

                const juce::ScopedLock sl (xl);
                if (auto* files = v["files"].getArray())
                    for (auto& f : *files)
                    {
                        Entry e { f["id"].toString(), f["name"].toString(), f["mimeType"].toString(), f["size"].toString().getLargeIntValue(), false };
                        if (e.mime == "application/vnd.google-apps.shortcut")
                        {
                            e.id = f["shortcutDetails"]["targetId"].toString();
                            e.mime = f["shortcutDetails"]["targetMimeType"].toString();
                        }
                        juce::String parent;                // the parent among the folders asked for
                        if (auto* ps = f["parents"].getArray())
                            for (auto& p : *ps)
                                if (inGroup.count (p.toString())) { parent = p.toString(); break; }
                        if (e.mime == "application/vnd.google-apps.folder")
                        {
                            if (seen.size() < 50000 && seen.insert (e.id).second)
                            {
                                x->folders[e.id] = { parent, e.name };
                                next.push_back (e.id);
                            }
                        }
                        else if (e.mime.startsWith ("audio/") || isAudioName (e.name))
                        {
                            x->files.push_back ({ e, parent });
                            ++found;
                        }
                    }
                pageToken = v["nextPageToken"].toString();
            }
            while (pageToken.isNotEmpty() && ! alive.expired());
        };

        // Level by level; the groups of a level go at the same time.
        juce::ThreadPool workers (6);
        while (! level.empty())
        {
            std::vector<juce::String> next;
            for (size_t from = 0; from < level.size(); from += 30)
            {
                std::vector<juce::String> group (level.begin() + (long) from, level.begin() + (long) juce::jmin (level.size(), from + 30));
                workers.addJob ([&ask, &next, group] { ask (group, next); });
            }
            while (workers.getNumJobs() > 0)
            {
                juce::Thread::sleep (40);
                if (const auto now = juce::Time::getMillisecondCounterHiRes(); now - lastSnapshot > 4000.0) { lastSnapshot = now; publish(); }
            }
            if (alive.expired()) return;
            { const juce::ScopedLock sl (xl); if (err.isNotEmpty()) break; }
            level = std::move (next);
        }
        x->builtAt = juce::Time::currentTimeMillis();

        if (err.isEmpty())
        {
            juce::Array<juce::var> fs, ss;
            for (auto& [id, pn] : x->folders) fs.add (juce::Array<juce::var> { id, pn.first, pn.second });
            for (auto& [e, parent] : x->files) ss.add (juce::Array<juce::var> { e.id, e.name, e.mime, juce::String (e.size), parent });
            auto o = std::make_unique<juce::DynamicObject>();
            o->setProperty ("built", x->builtAt);
            o->setProperty ("folders", fs);
            o->setProperty ("files", ss);
            const auto file = indexFile (rootId);
            file.getParentDirectory().createDirectory();
            file.replaceWithText (juce::JSON::toString (juce::var (o.release()), true));
        }
        else
            x->builtAt = 0;                         // what was found stays searchable; it is asked again later
        juce::MessageManager::callAsync ([this, alive, done, rootId, x, err]
        {
            if (alive.expired()) return;
            indexing.erase (rootId);
            setIndex (rootId, x);
            if (done) done (err);
        });
    });
}

void GoogleDrive::list (const juce::String& folderId, std::function<void (std::vector<Entry>, juce::String)> done)
{
    pool.addJob ([this, folderId, done, alive = std::weak_ptr<bool> (alive)]
    {
        std::vector<Entry> out;
        juce::String err, pageToken;
        do
        {
            const auto q = "'" + folderId + "' in parents and trashed=false";
            auto url = juce::String (api) + "files?q=" + esc (q)
                     + "&fields=" + esc ("nextPageToken,files(id,name,mimeType,size,shortcutDetails(targetId,targetMimeType))")
                     + "&pageSize=1000&orderBy=folder,name_natural&supportsAllDrives=true&includeItemsFromAllDrives=true";
            if (pageToken.isNotEmpty()) url << "&pageToken=" << esc (pageToken);
            const auto v = getJson (url, err);
            if (err.isNotEmpty()) break;
            if (auto* files = v["files"].getArray())
                for (auto& f : *files)
                {
                    Entry e { f["id"].toString(), f["name"].toString(), f["mimeType"].toString(), f["size"].toString().getLargeIntValue(), false };
                    if (e.mime == "application/vnd.google-apps.shortcut")       // a shortcut: what it points to
                    {
                        e.id = f["shortcutDetails"]["targetId"].toString();
                        e.mime = f["shortcutDetails"]["targetMimeType"].toString();
                    }
                    e.folder = e.mime == "application/vnd.google-apps.folder";
                    if (e.folder || e.mime.startsWith ("audio/") || isAudioName (e.name)) out.push_back (e);
                }
            pageToken = v["nextPageToken"].toString();
        }
        while (pageToken.isNotEmpty());
        juce::MessageManager::callAsync ([alive, done, out, err] { if (! alive.expired() && done) done (out, err); });
    });
}

bool GoogleDrive::isDownloading (const juce::File& dest) const
{
    const juce::ScopedLock sl (lock);
    return downloading.count (dest.getFullPathName()) > 0;
}

void GoogleDrive::download (const juce::String& fileId, const juce::File& dest, std::function<void (float)> progress,
                            std::function<void (bool, juce::String)> done, bool bulk)
{
    {
        const juce::ScopedLock sl (lock);
        if (! downloading.insert (dest.getFullPathName()).second) return;      // already on its way
    }
    (bulk ? bulkPool : pool).addJob ([this, fileId, dest, progress, done, alive = std::weak_ptr<bool> (alive)]
    {
        juce::String err;
        const auto t = accessToken();
        bool ok = false;
        if (t.isEmpty()) err = "Conectá Google Drive de nuevo";
        else
        {
            int status = 0;
            auto in = juce::URL (juce::String (api) + "files/" + esc (fileId) + "?alt=media&supportsAllDrives=true")
                          .createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                  .withExtraHeaders ("Authorization: Bearer " + t)
                                                  .withConnectionTimeoutMs (20000)
                                                  .withStatusCode (&status));
            if (in == nullptr || status >= 400) err = juce::String::fromUTF8 ("No se pudo bajar el tema de Google Drive");
            else
            {
                dest.getParentDirectory().createDirectory();
                const auto part = dest.getSiblingFile (dest.getFileName() + ".part");
                part.deleteFile();
                {
                    juce::FileOutputStream out (part);
                    const auto total = in->getTotalLength();
                    juce::HeapBlock<char> buf (1 << 16);
                    juce::int64 got = 0;
                    float last = -1.0f;
                    while (! in->isExhausted() && ! alive.expired())
                    {
                        const int n = in->read (buf, 1 << 16);
                        if (n <= 0) break;
                        out.write (buf, (size_t) n);
                        got += n;
                        if (total > 0 && progress)
                        {
                            const float p = (float) got / (float) total;
                            if (p - last >= 0.02f) { last = p; juce::MessageManager::callAsync ([progress, p, alive] { if (! alive.expired()) progress (p); }); }
                        }
                    }
                    out.flush();
                    ok = out.getStatus().wasOk() && got > 0 && (total <= 0 || got == total);
                }
                // Google answered with a page (an error or a warning) instead of the song.
                if (ok)
                {
                    juce::MemoryBlock head;
                    part.createInputStream()->readIntoMemoryBlock (head, 64);
                    const auto start = head.toString().trimStart();
                    if (start.startsWithChar ('<') || start.startsWithChar ('{'))
                    {
                        ok = false;
                        part.deleteFile();
                        err = juce::String::fromUTF8 ("Google Drive no entregó el tema (respondió con un aviso). Probá de nuevo en un rato");
                    }
                }
                ok = ok && part.moveFileTo (dest);
                if (! ok) { part.deleteFile(); if (err.isEmpty()) err = juce::String::fromUTF8 ("La descarga se cortó"); }
            }
        }
        if (alive.expired()) return;
        {
            const juce::ScopedLock sl (lock);
            downloading.erase (dest.getFullPathName());
        }
        juce::MessageManager::callAsync ([done, ok, err, alive] { if (! alive.expired() && done) done (ok, err); });
    });
}

} // namespace om
