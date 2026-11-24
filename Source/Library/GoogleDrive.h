#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <set>
#include <map>

namespace om
{

/** Google Drive as a music source: sign in once in the browser (read-only access), link folders by
    their address (own or shared with you), list them, and fetch a song to a temporary file to play it.
    Nothing is kept unless the user adds the song to the collection.

    Sign-in is OAuth 2.0 for installed apps: the browser comes back to a one-time local address
    (http://127.0.0.1:port) with PKCE, and only a refresh token is stored (data folder, drive.json). */
class GoogleDrive
{
public:
    struct Entry
    {
        juce::String id, name, mime;
        juce::int64 size = 0;
        bool folder = false;
    };
    struct Folder { juce::String id, name; };

    GoogleDrive();
    ~GoogleDrive();

    // Configuration (the OAuth client of the Google Cloud project).
    bool hasClient() const                         { return clientId.isNotEmpty(); }
    /** True when this build carries OpenMix's own Google client (nothing to configure). */
    static bool builtIn();
    void setClient (const juce::String& id, const juce::String& secret);
    bool isSignedIn() const                        { return refreshToken.isNotEmpty(); }
    juce::String account() const                   { return accountEmail; }

    /** Opens the browser to sign in; `done` (message thread) with an error text or empty on success. */
    void signIn (std::function<void (juce::String error)> done);
    void signOut();

    // Linked folders, in the sidebar.
    const std::vector<Folder>& folders() const     { return linked; }
    /** A folder by its address (drive.google.com/drive/folders/…, …?id=…, or the bare id). */
    void linkFolder (const juce::String& address, std::function<void (juce::String error)> done);
    void unlinkFolder (const juce::String& id);
    static juce::String idFromAddress (const juce::String& address);
    /** Every web address in a pasted text (a message with several links, one per line or not). */
    static juce::StringArray linksIn (const juce::String& text);
    /** Where a shortened address (TinyURL, bit.ly…) points, following up to 5 redirects (network). */
    static juce::String followShortLink (juce::String address);

    /** The folder's sub-folders and audio files (background; `done` on the message thread). */
    void list (const juce::String& folderId, std::function<void (std::vector<Entry>, juce::String error)> done);

    /** Downloads a file to `dest` (background). `progress` 0…1 and `done` run on the message thread. */
    void download (const juce::String& fileId, const juce::File& dest, std::function<void (float)> progress,
                   std::function<void (bool ok, juce::String error)> done, bool bulk = false);
    bool isDownloading (const juce::File& dest) const;

    /** Every sub-folder and song under a linked folder, to search all of it at once. Built in the
        background (a few requests per level), kept in the data folder and refreshed when old. */
    struct Index
    {
        std::map<juce::String, std::pair<juce::String, juce::String>> folders;   // id → parent id, name
        std::vector<std::pair<Entry, juce::String>> files;                         // song, its folder id
        juce::int64 builtAt = 0;                                                   // ms since 1970
        bool contains (const juce::String& folderId, const juce::String& underFolderId) const;
        juce::String pathOf (const juce::String& folderId, const juce::String& rootId) const;
    };
    /** The saved index of a folder (message thread), or nullptr when there is none yet. */
    std::shared_ptr<const Index> index (const juce::String& rootId);
    /** Walks the folder tree; `progress` (songs found so far) and `done` run on the message thread. */
    void buildIndex (const juce::String& rootId, std::function<void (int)> progress, std::function<void (juce::String error)> done);
    bool isIndexing (const juce::String& rootId) const   { return indexing.count (rootId) > 0; }

    /** Where songs being played are kept until they leave the decks. */
    static juce::File cacheFolder();
    static bool isAudioName (const juce::String& name);

private:
    juce::String accessToken();                     // background threads only
    juce::var getJson (const juce::String& url, juce::String& error);
    void load();
    void save() const;

    juce::String clientId, clientSecret, refreshToken, accountEmail;
    juce::String token;
    double tokenExpiresMs = 0.0;
    std::vector<Folder> linked;
    mutable juce::CriticalSection lock;
    std::set<juce::String> downloading;
    std::map<juce::String, std::shared_ptr<const Index>> indexes;   // message thread
    std::set<juce::String> indexing;                                 // message thread
    static juce::File indexFile (const juce::String& rootId);
    juce::ThreadPool pool { 3 };
    juce::ThreadPool bulkPool { 3 };                 // whole folders to the collection, behind what plays
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};

} // namespace om
