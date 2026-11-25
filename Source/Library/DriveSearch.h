#pragma once

#include "GoogleDrive.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace om
{

/** Searching Google Drive folders without ever holding the screen: a low-priority thread keeps the
    names ready (lower case, with their folder paths) and answers each query; a newer query cancels
    the one running. Results come back on the message thread. */
class DriveSearch : private juce::Thread
{
public:
    struct Root { juce::String id, name, under; };       // a linked folder, and the folder searched inside it
    struct Hit { GoogleDrive::Entry entry; juce::String path; };
    struct Result
    {
        std::vector<Hit> hits;                            // at most maxHits
        int total = 0;                                    // all that matched
        bool waiting = false;                             // some folder's index is not in memory yet
    };
    static constexpr int maxHits = 2000;

    explicit DriveSearch (GoogleDrive&);
    ~DriveSearch() override;

    /** The newest request wins; `done` runs on the message thread (not at all if a newer one came). */
    void request (const juce::String& query, std::vector<Root> roots, std::function<void (Result)> done);

    /** The matching itself (any thread), for tests. */
    struct Prepared
    {
        std::shared_ptr<const GoogleDrive::Index> index;
        juce::String rootName;
        std::vector<int> parent;                          // folder number → parent number (−1 at the top)
        std::vector<juce::String> folderIds, paths;       // "Root / A / B"
        std::vector<std::string> lowerPaths;
        std::vector<int> folderOf;                        // per song
        std::vector<std::string> lowerNames;
        std::unordered_map<juce::String, int> number;
    };
    static std::shared_ptr<Prepared> prepare (std::shared_ptr<const GoogleDrive::Index>, const juce::String& rootId, const juce::String& rootName);
    /** Adds the matches of `p` (inside folder `under`) to `out`; false if `cancelled` turned true. */
    static bool match (const Prepared& p, const juce::String& under, const std::vector<std::string>& tokens,
                       Result& out, const std::function<bool()>& cancelled);
    static std::vector<std::string> tokens (const juce::String& query);

private:
    void run() override;

    GoogleDrive& drive;
    std::mutex m;
    std::condition_variable cv;
    struct Pending { juce::String query; std::vector<Root> roots; std::function<void (Result)> done; juce::uint64 gen = 0; };
    std::optional<Pending> pending;
    std::atomic<juce::uint64> latest { 0 };
    std::map<juce::String, std::shared_ptr<Prepared>> prepared;    // worker thread only
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};

} // namespace om
