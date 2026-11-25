#include "DriveSearch.h"

namespace om
{

DriveSearch::DriveSearch (GoogleDrive& d) : juce::Thread ("OpenMix Drive search"), drive (d)
{
    startThread (juce::Thread::Priority::low);
}

DriveSearch::~DriveSearch()
{
    alive.reset();
    signalThreadShouldExit();
    cv.notify_all();
    stopThread (4000);
}

void DriveSearch::request (const juce::String& query, std::vector<Root> roots, std::function<void (Result)> done)
{
    const auto gen = ++latest;
    {
        const std::lock_guard<std::mutex> l (m);
        pending = Pending { query, std::move (roots), std::move (done), gen };
    }
    cv.notify_one();
}

std::vector<std::string> DriveSearch::tokens (const juce::String& query)
{
    std::vector<std::string> out;
    for (auto& t : juce::StringArray::fromTokens (query.toLowerCase(), true))
        if (t.isNotEmpty()) out.push_back (t.toStdString());
    return out;
}

std::shared_ptr<DriveSearch::Prepared> DriveSearch::prepare (std::shared_ptr<const GoogleDrive::Index> idx, const juce::String& rootId,
                                                             const juce::String& rootName)
{
    auto p = std::make_shared<Prepared>();
    p->index = idx;
    p->rootName = rootName;
    const auto& folders = idx->folders;
    p->folderIds.reserve (folders.size() + 1);
    p->folderIds.push_back (rootId);
    p->number[rootId] = 0;
    for (auto& [id, pn] : folders)
        if (id != rootId)
        {
            p->number[id] = (int) p->folderIds.size();
            p->folderIds.push_back (id);
        }
    const auto n = p->folderIds.size();
    p->parent.assign (n, -1);
    std::vector<juce::String> names (n);
    names[0] = rootName;
    for (size_t i = 1; i < n; ++i)
    {
        const auto& pn = folders.at (p->folderIds[i]);
        names[i] = pn.second;
        const auto it = p->number.find (pn.first);
        p->parent[i] = it != p->number.end() ? it->second : 0;      // unknown parent: under the top folder
    }
    // Paths "Root / A / B", walking up until a folder whose path is known (no recursion).
    p->paths.assign (n, {});
    std::vector<char> known (n, 0);
    p->paths[0] = rootName;
    known[0] = 1;
    std::vector<size_t> chain;
    for (size_t i = 1; i < n; ++i)
    {
        chain.clear();
        size_t f = i;
        while (! known[f] && chain.size() < 128) { chain.push_back (f); f = p->parent[f] < 0 ? 0 : (size_t) p->parent[f]; }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            const auto par = p->parent[*it] < 0 ? 0 : (size_t) p->parent[*it];
            p->paths[*it] = p->paths[par].isEmpty() ? names[*it] : p->paths[par] + " / " + names[*it];
            known[*it] = 1;
        }
    }
    p->lowerPaths.reserve (n);
    for (auto& path : p->paths) p->lowerPaths.push_back (path.toLowerCase().toStdString());

    p->folderOf.reserve (idx->files.size());
    p->lowerNames.reserve (idx->files.size());
    for (auto& [e, parentId] : idx->files)
    {
        const auto it = p->number.find (parentId);
        p->folderOf.push_back (it != p->number.end() ? it->second : 0);
        p->lowerNames.push_back (e.name.toLowerCase().toStdString());
    }
    return p;
}

bool DriveSearch::match (const Prepared& p, const juce::String& under, const std::vector<std::string>& toks,
                         Result& out, const std::function<bool()>& cancelled)
{
    // Which folders are inside `under` (all of them when it is the top folder).
    const auto u = p.number.find (under);
    const int top = u != p.number.end() ? u->second : 0;
    std::vector<char> inside;
    if (top != 0)
    {
        inside.assign (p.folderIds.size(), 0);                  // 0 unknown · 1 inside · 2 outside
        inside[(size_t) top] = 1;
        std::vector<size_t> chain;
        for (size_t i = 0; i < inside.size(); ++i)
        {
            chain.clear();
            size_t f = i;
            char verdict = 2;
            for (int guard = 0; guard < 128; ++guard)
            {
                if (inside[f] != 0) { verdict = inside[f]; break; }
                chain.push_back (f);
                if (p.parent[f] < 0) break;
                f = (size_t) p.parent[f];
            }
            for (auto c : chain) inside[c] = verdict;
        }
    }

    const auto& files = p.index->files;
    for (size_t i = 0; i < files.size(); ++i)
    {
        if ((i & 16383) == 0 && cancelled()) return false;
        const int f = p.folderOf[i];
        if (top != 0 && inside[(size_t) f] != 1) continue;
        bool all = true;
        for (auto& t : toks)
            if (p.lowerNames[i].find (t) == std::string::npos && p.lowerPaths[(size_t) f].find (t) == std::string::npos) { all = false; break; }
        if (! all) continue;
        ++out.total;
        if ((int) out.hits.size() < maxHits)
            out.hits.push_back ({ files[i].first, p.paths[(size_t) f] });
    }
    return true;
}

void DriveSearch::run()
{
    while (! threadShouldExit())
    {
        Pending job;
        {
            std::unique_lock<std::mutex> l (m);
            cv.wait_for (l, std::chrono::milliseconds (500), [this] { return pending.has_value() || threadShouldExit(); });
            if (! pending.has_value()) continue;
            job = std::move (*pending);
            pending.reset();
        }
        auto cancelled = [this, &job] { return threadShouldExit() || latest.load() != job.gen; };
        Result r;
        const auto toks = tokens (job.query);
        bool stopped = toks.empty();
        for (auto& root : job.roots)
        {
            if (stopped) break;
            const auto idx = drive.index (root.id);          // only what is in memory: never reads the disk here
            if (idx == nullptr) { r.waiting = true; continue; }
            auto& pp = prepared[root.id];
            if (pp == nullptr || pp->index != idx || pp->rootName != root.name)
                pp = prepare (idx, root.id, root.name);
            if (cancelled() || ! match (*pp, root.under, toks, r, cancelled)) stopped = true;
        }
        if (stopped || cancelled()) continue;
        juce::MessageManager::callAsync ([done = std::move (job.done), r = std::move (r), a = std::weak_ptr<bool> (alive)]() mutable
        {
            if (! a.expired() && done) done (std::move (r));
        });
    }
}

} // namespace om
