#include "riven/Layout.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

namespace riven
{
namespace
{
    namespace fs = std::filesystem;

    std::string lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }

    /// The stack letter used in archive names: a_Data.MHK -> aspit, etc.
    rivendata::StackId stackForLetter(char c)
    {
        switch (c)
        {
        case 'a': return rivendata::StackId::Aspit;
        case 'b': return rivendata::StackId::Bspit;
        case 'g': return rivendata::StackId::Gspit;
        case 'j': return rivendata::StackId::Jspit;
        case 'o': return rivendata::StackId::Ospit;
        case 'p': return rivendata::StackId::Pspit;
        case 'r': return rivendata::StackId::Rspit;
        case 't': return rivendata::StackId::Tspit;
        default:  return rivendata::StackId::None;
        }
    }

    /// A classified archive filename.
    struct ArchiveName
    {
        rivendata::StackId stack = rivendata::StackId::None;
        bool isSound = false;
        /// Variant number. Higher wins. 0 means the name carried no digit.
        ///
        /// This single rule reproduces every ordering ScummVM hardcodes at
        /// riven.cpp:440-484 -- CD b_Data1 before b_Data, CD j_Data3/2/1, DVD
        /// t_Data2/t_Data1 -- and it also copes with releases that spell the
        /// patch differently. The French 5-CD set ships "b2_data.MHK" rather
        /// than "b_Data1.mhk"; a hardcoded name list silently ignores it and
        /// converts the unpatched Boiler Island.
        int variant = 0;
        bool valid = false;
    };

    /// Match <letter>[digit]_data[digit].mhk and the _sounds equivalent.
    ArchiveName classify(const std::string &filename)
    {
        ArchiveName a;
        const std::string n = lower(filename);

        if (n.size() < 8 || n.compare(n.size() - 4, 4, ".mhk") != 0)
            return a;

        const std::string stem = n.substr(0, n.size() - 4);

        std::size_t i = 0;
        a.stack = stackForLetter(stem[i++]);
        if (a.stack == rivendata::StackId::None)
            return a;

        // Optional digit immediately after the stack letter: "b2_data".
        if (i < stem.size() && std::isdigit(static_cast<unsigned char>(stem[i])))
            a.variant = stem[i++] - '0';

        if (i >= stem.size() || stem[i++] != '_')
            return a;

        const std::string rest = stem.substr(i);
        if (rest.rfind("data", 0) == 0)
            a.isSound = false;
        else if (rest.rfind("sounds", 0) == 0)
            a.isSound = true;
        else
            return a;

        // Optional trailing digit: "j_data3", "t_data2".
        const char last = rest.back();
        if (std::isdigit(static_cast<unsigned char>(last)))
            a.variant = last - '0';

        a.valid = true;
        return a;
    }

    bool hasDirCI(const fs::path &root, const char *name)
    {
        std::error_code ec;
        if (!fs::is_directory(root, ec))
            return false;
        const std::string want = lower(name);
        for (const auto &e : fs::directory_iterator(root, ec))
        {
            if (e.is_directory(ec) && lower(e.path().filename().string()) == want)
                return true;
        }
        return false;
    }

    /// Case-insensitive lookup of a file anywhere in the scanned tree.
    fs::path findFileCI(const std::vector<fs::path> &all, const char *name)
    {
        const std::string want = lower(name);
        for (const auto &p : all)
            if (lower(p.filename().string()) == want)
                return p;
        return {};
    }
} // namespace

const char *toString(SourceLayout l)
{
    switch (l)
    {
    case SourceLayout::FiveCd: return "5-CD install";
    case SourceLayout::Dvd:    return "DVD / GOG";
    case SourceLayout::Flat:   return "flat directory";
    default:                   return "unrecognised";
    }
}

const StackSource *Source::find(rivendata::StackId id) const
{
    for (const auto &s : stacks)
        if (s.id == id)
            return &s;
    return nullptr;
}

bool looksLikeMohawk(const fs::path &p)
{
    std::FILE *f = std::fopen(p.string().c_str(), "rb");
    if (f == nullptr)
        return false;
    char hdr[12] = {};
    const std::size_t got = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
    if (got != sizeof(hdr))
        return false;
    // 'MHWK' at 0, 'RSRC' at 8. Checking both is what rejects the zero-filled
    // placeholder archives a partial download leaves behind.
    return std::memcmp(hdr, "MHWK", 4) == 0 && std::memcmp(hdr + 8, "RSRC", 4) == 0;
}

Source detectSource(const fs::path &root)
{
    Source src;
    src.root = root;

    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return src;

    // Walk the tree once. Riven installs are shallow, and scanning beats
    // guessing at directory names that vary by release and by locale.
    std::vector<fs::path> allFiles;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (it->is_regular_file(ec))
            allFiles.push_back(it->path());
    }

    // Layout is reported for the user's benefit; the archive search below does
    // not depend on it, which is what makes odd installs work anyway.
    const bool hasData = hasDirCI(root, "Data");
    const bool hasAll = hasDirCI(root, "All");
    const bool hasExe = hasDirCI(root, "exe");
    const bool hasAssets1 = hasDirCI(root, "Assets1");

    std::map<rivendata::StackId, std::vector<std::pair<int, fs::path>>> data;
    std::map<rivendata::StackId, std::vector<std::pair<int, fs::path>>> sounds;

    for (const auto &p : allFiles)
    {
        const ArchiveName a = classify(p.filename().string());
        if (!a.valid)
            continue;
        if (!looksLikeMohawk(p))
            continue; // zero-filled or truncated: treat as absent
        (a.isSound ? sounds : data)[a.stack].emplace_back(a.variant, p);
    }

    if (hasExe && !hasAll)
        src.layout = SourceLayout::Dvd;
    else if (hasData && (hasAll || hasAssets1))
        src.layout = SourceLayout::FiveCd;
    else if (!data.empty())
        src.layout = SourceLayout::Flat;

    for (int i = 1; i < rivendata::kStackCount; ++i)
    {
        const auto id = static_cast<rivendata::StackId>(i);

        auto dit = data.find(id);
        if (dit == data.end() || dit->second.empty())
        {
            src.missingStacks.push_back(id);
            continue;
        }

        StackSource ss;
        ss.id = id;

        // Highest variant first: patch archives override the base ones, and
        // the converter resolves each resource from the first archive that
        // has it (riven.cpp:441-443).
        auto byVariantDesc = [](const auto &a, const auto &b) {
            if (a.first != b.first)
                return a.first > b.first;
            return a.second < b.second;
        };
        std::sort(dit->second.begin(), dit->second.end(), byVariantDesc);
        for (const auto &[variant, path] : dit->second)
        {
            (void)variant;
            ss.dataArchives.push_back(path);
        }

        if (auto sit = sounds.find(id); sit != sounds.end())
        {
            std::sort(sit->second.begin(), sit->second.end(), byVariantDesc);
            for (const auto &[variant, path] : sit->second)
            {
                (void)variant;
                ss.soundArchives.push_back(path);
            }
        }

        src.stacks.push_back(std::move(ss));
    }

    src.installerArchive = findFileCI(allFiles, "arcriven.z");
    src.extrasArchive = findFileCI(allFiles, "extras.mhk");

    return src;
}

} // namespace riven
