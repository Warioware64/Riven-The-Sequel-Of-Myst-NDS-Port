#include "riven/Installer.hpp"

#include <cstdio>

#include "riven/Dcl.hpp"

namespace fs = std::filesystem;

namespace riven
{
namespace
{
    /// A bounds-checked cursor over the archive. Every read past the end sets
    /// bad() and yields zero, so the parse below can be written straight through
    /// and checked once -- an installer file is untrusted input and a truncated
    /// one is a real case (the archive sits on a 1997 CD).
    class Cursor
    {
    public:
        Cursor(const std::vector<std::uint8_t> &b) : b_(b) {}

        void seek(std::size_t p) { pos_ = p; }
        std::size_t tell() const { return pos_; }
        void skip(std::size_t n) { pos_ += n; }
        bool bad() const { return bad_; }

        std::uint8_t u8()
        {
            if (pos_ + 1 > b_.size())
            {
                bad_ = true;
                return 0;
            }
            return b_[pos_++];
        }

        std::uint16_t u16()
        {
            const std::uint32_t a = u8();
            const std::uint32_t c = u8();
            return static_cast<std::uint16_t>(a | (c << 8));
        }

        std::uint32_t u32()
        {
            const std::uint32_t a = u16();
            const std::uint32_t c = u16();
            return a | (c << 16);
        }

        /// Big-endian, for the magic only.
        std::uint32_t u32be()
        {
            std::uint32_t v = 0;
            for (int i = 0; i < 4; ++i)
                v = (v << 8) | u8();
            return v;
        }

        std::string text(std::size_t n)
        {
            std::string s;
            s.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
                s.push_back(static_cast<char>(u8()));
            return s;
        }

    private:
        const std::vector<std::uint8_t> &b_;
        std::size_t pos_ = 0;
        bool bad_ = false;
    };

    std::string lowered(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (const char c : s)
            out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        return out;
    }

    /// Entries in a multi-directory archive are named "<dir>\<file>"; Riven's has
    /// one unnamed directory, so lookups are by bare filename. Taking the last
    /// component makes both work without the caller knowing which it has.
    std::string baseName(std::string_view name)
    {
        const std::size_t slash = name.find_last_of("\\/");
        return std::string(slash == std::string_view::npos ? name : name.substr(slash + 1));
    }

    bool readWholeFile(const fs::path &path, std::vector<std::uint8_t> &out)
    {
        std::FILE *f = std::fopen(path.string().c_str(), "rb");
        if (f == nullptr)
            return false;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0)
        {
            std::fclose(f);
            return false;
        }
        out.resize(static_cast<std::size_t>(size));
        const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        return ok;
    }
} // namespace

InstallerArchive InstallerArchive::open(const fs::path &path)
{
    InstallerArchive a;

    std::vector<std::uint8_t> bytes;
    if (!readWholeFile(path, bytes))
        return a;

    Cursor c(bytes);
    if (c.u32be() != 0x135D658Cu)
        return a; // not an InstallShield v3 archive; not an error

    // The header's useful half starts at 41. What the 33 bytes before it hold is
    // not documented anywhere this project can cite, and nothing here needs them.
    c.seek(41);
    const std::uint32_t dirTableOffset = c.u32();
    (void)c.u32(); // directory table size
    const std::uint16_t dirCount = c.u16();
    (void)c.u32(); // file table offset -- the table follows the directories anyway
    (void)c.u32(); // file table size
    if (c.bad() || dirCount == 0)
        return a;

    // Directory table: how many files each directory holds, and its name.
    std::vector<std::uint16_t> filesPerDir;
    filesPerDir.reserve(dirCount);
    c.seek(dirTableOffset);
    for (std::uint16_t i = 0; i < dirCount && !c.bad(); ++i)
    {
        const std::uint16_t fileCount = c.u16();
        const std::uint16_t chunkSize = c.u16();
        const std::uint16_t nameLength = c.u16();
        const std::string name = c.text(nameLength);
        filesPerDir.push_back(fileCount);

        // The chunk is padded past the name; 6 is the three halfwords above.
        if (chunkSize < name.size() + 6)
            return a;
        c.skip(chunkSize - name.size() - 6);
    }
    if (c.bad())
        return a;

    // File table, directory by directory, straight after the directory table.
    for (std::uint16_t d = 0; d < dirCount && !c.bad(); ++d)
    {
        for (std::uint16_t f = 0; f < filesPerDir[d] && !c.bad(); ++f)
        {
            c.skip(3);
            Entry e;
            e.uncompressed = c.u32();
            e.compressed = c.u32();
            e.offset = c.u32();
            c.skip(14);
            const std::uint8_t nameLength = c.u8();
            e.name = c.text(nameLength);
            c.skip(13);

            if (c.bad() || e.name.empty())
                continue;
            if (static_cast<std::size_t>(e.offset) + e.compressed > bytes.size())
                continue; // the entry points outside the file: skip it, not the archive

            a.byLowerName_[lowered(baseName(e.name))] = a.entries_.size();
            a.entries_.push_back(std::move(e));
        }
    }

    if (a.entries_.empty())
        return a;

    a.bytes_ = std::move(bytes);
    a.path_ = path;
    return a;
}

std::vector<std::string> InstallerArchive::names() const
{
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const Entry &e : entries_)
        out.push_back(e.name);
    return out;
}

bool InstallerArchive::contains(std::string_view name) const
{
    return byLowerName_.find(lowered(baseName(name))) != byLowerName_.end();
}

std::vector<std::uint8_t> InstallerArchive::read(std::string_view name,
                                                 std::string &error) const
{
    const auto it = byLowerName_.find(lowered(baseName(name)));
    if (it == byLowerName_.end())
    {
        error = std::string(name) + " is not in " + path_.filename().string();
        return {};
    }

    const Entry &e = entries_[it->second];
    auto out = decompressDcl(bytes_.data() + e.offset, e.compressed, e.uncompressed, error);
    if (out.empty() && error.empty())
        error = e.name + " decompressed to nothing";
    if (!error.empty())
        error = e.name + ": " + error;
    return out;
}

} // namespace riven
