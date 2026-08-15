#include "riven/Archive.hpp"

#include <algorithm>
#include <utility>

extern "C" {
#include <vaht/vaht.h>
}

namespace fs = std::filesystem;

namespace riven
{

// ---------------------------------------------------------------------------
// Archive
// ---------------------------------------------------------------------------

Archive::~Archive()
{
    if (handle_ != nullptr)
        vaht_archive_close(handle_);
}

Archive::Archive(Archive &&other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), path_(std::move(other.path_))
{
}

Archive &Archive::operator=(Archive &&other) noexcept
{
    if (this != &other)
    {
        if (handle_ != nullptr)
            vaht_archive_close(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
        path_ = std::move(other.path_);
    }
    return *this;
}

Archive Archive::open(const fs::path &path)
{
    Archive a;
    a.path_ = path;
    a.handle_ = vaht_archive_open(path.string().c_str());
    return a;
}

std::vector<std::uint16_t> Archive::resourceIds(const char *type) const
{
    std::vector<std::uint16_t> ids;
    if (handle_ == nullptr)
        return ids;

    vaht_resource **rs = vaht_resources_open(handle_, type);
    if (rs == nullptr)
        return ids;
    for (int i = 0; rs[i] != nullptr; ++i)
        ids.push_back(vaht_resource_id(rs[i]));
    vaht_resources_close(rs);
    return ids;
}

std::size_t Archive::resourceCount(const char *type) const
{
    return resourceIds(type).size();
}

std::vector<std::pair<std::string, std::uint16_t>> Archive::names(const char *type) const
{
    std::vector<std::pair<std::string, std::uint16_t>> out;
    if (handle_ == nullptr)
        return out;

    vaht_resource **rs = vaht_resources_open(handle_, type);
    if (rs == nullptr)
        return out;
    for (int i = 0; rs[i] != nullptr; ++i)
    {
        const char *const name = vaht_resource_name(rs[i]);
        if (name == nullptr || name[0] == '\0')
            continue; // a resource may have no name; most do

        // Folded here, once, because every consumer wants it folded: this
        // release spells them "339_abigtic_1" and ScummVM asks for "aBigTic".
        std::string lower(name);
        for (char &c : lower)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');

        out.emplace_back(std::move(lower), vaht_resource_id(rs[i]));
    }
    vaht_resources_close(rs);
    return out;
}

bool Archive::has(const char *type, std::uint16_t id) const
{
    if (handle_ == nullptr)
        return false;
    vaht_resource *r = vaht_resource_open(handle_, type, id);
    if (r == nullptr)
        return false;
    vaht_resource_close(r);
    return true;
}

std::vector<std::uint8_t> Archive::read(const char *type, std::uint16_t id) const
{
    std::vector<std::uint8_t> bytes;
    if (handle_ == nullptr)
        return bytes;

    vaht_resource *r = vaht_resource_open(handle_, type, id);
    if (r == nullptr)
        return bytes;

    const std::uint32_t size = vaht_resource_size(r);
    if (size > 0)
    {
        bytes.resize(size);
        vaht_resource_seek(r, 0);
        const std::uint32_t got = vaht_resource_read(r, size, bytes.data());
        if (got != size)
            bytes.resize(got); // short read: hand back what there is, caller decides
    }
    vaht_resource_close(r);
    return bytes;
}

// ---------------------------------------------------------------------------
// Bitmap
// ---------------------------------------------------------------------------

Bitmap::~Bitmap()
{
    if (handle_ != nullptr)
        vaht_bmp_close(handle_);
}

Bitmap::Bitmap(Bitmap &&other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      width_(std::exchange(other.width_, 0)),
      height_(std::exchange(other.height_, 0))
{
}

Bitmap &Bitmap::operator=(Bitmap &&other) noexcept
{
    if (this != &other)
    {
        if (handle_ != nullptr)
            vaht_bmp_close(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
    }
    return *this;
}

bool Bitmap::indexed() const
{
    return handle_ != nullptr && vaht_bmp_truecolor(handle_) == 0;
}

const std::uint8_t *Bitmap::rgb() const
{
    return handle_ != nullptr ? vaht_bmp_data(handle_) : nullptr;
}

const std::uint8_t *Bitmap::indices() const
{
    return handle_ != nullptr ? vaht_bmp_indexed_data(handle_) : nullptr;
}

const std::uint8_t *Bitmap::palette() const
{
    return handle_ != nullptr ? vaht_bmp_palette(handle_) : nullptr;
}

// ---------------------------------------------------------------------------
// ArchiveSet
// ---------------------------------------------------------------------------

void ArchiveSet::openAll(const std::vector<fs::path> &paths,
                         std::vector<std::string> &failures)
{
    for (const auto &p : paths)
    {
        Archive a = Archive::open(p);
        if (!a.isOpen())
        {
            failures.push_back(p.filename().string());
            continue;
        }
        archives_.push_back(std::move(a));
    }
}

std::vector<std::uint16_t> ArchiveSet::resourceIds(const char *type) const
{
    std::vector<std::uint16_t> all;
    for (const auto &a : archives_)
    {
        const auto ids = a.resourceIds(type);
        all.insert(all.end(), ids.begin(), ids.end());
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    return all;
}

std::size_t ArchiveSet::rawResourceCount(const char *type) const
{
    std::size_t n = 0;
    for (const auto &a : archives_)
        n += a.resourceCount(type);
    return n;
}

const Archive *ArchiveSet::find(const char *type, std::uint16_t id) const
{
    // First hit wins. archives_ is in the priority order Layout computed, so
    // this is where a patch archive overrides the base game.
    for (const auto &a : archives_)
        if (a.has(type, id))
            return &a;
    return nullptr;
}

std::vector<std::pair<std::string, std::uint16_t>>
ArchiveSet::names(const char *type) const
{
    std::vector<std::pair<std::string, std::uint16_t>> out;
    std::vector<std::string> seen;

    // Same priority rule as find(): the first archive to name something owns
    // that name, so a patch archive's copy wins over the base game's.
    for (const auto &a : archives_)
    {
        for (auto &nv : a.names(type))
        {
            if (std::find(seen.begin(), seen.end(), nv.first) != seen.end())
                continue;
            seen.push_back(nv.first);
            out.push_back(std::move(nv));
        }
    }
    return out;
}

std::vector<std::uint8_t> ArchiveSet::read(const char *type, std::uint16_t id) const
{
    if (const Archive *a = find(type, id); a != nullptr)
        return a->read(type, id);
    return {};
}

Bitmap ArchiveSet::readBitmap(std::uint16_t id) const
{
    Bitmap bmp;

    const Archive *owner = find("tBMP", id);
    if (owner == nullptr)
        return bmp;

    vaht_resource *r = vaht_resource_open(owner->raw(), "tBMP", id);
    if (r == nullptr)
        return bmp;

    bmp.handle_ = vaht_bmp_open(r);

    // libvaht refcounts: vaht_bmp_open calls vaht_resource_grab, and
    // vaht_bmp_close calls vaht_resource_close. So the bmp holds its OWN
    // reference and this function must still drop the one vaht_resource_open
    // handed us -- otherwise every converted image leaks a resource and, since
    // a resource also holds a reference to its archive, the archive with it.
    vaht_resource_close(r);

    if (bmp.handle_ == nullptr)
        return bmp;

    bmp.width_ = vaht_bmp_width(bmp.handle_);
    bmp.height_ = vaht_bmp_height(bmp.handle_);
    return bmp;
}

std::vector<std::uint8_t> ArchiveSet::readMovie(std::uint16_t id) const
{
    std::vector<std::uint8_t> out;

    const Archive *owner = find("tMOV", id);
    if (owner == nullptr)
        return out;

    vaht_resource *r = vaht_resource_open(owner->raw(), "tMOV", id);
    if (r == nullptr)
        return out;

    const std::uint32_t size = vaht_resource_size(r);
    vaht_mov *mov = vaht_mov_open(r);
    // Same refcount rule as readBitmap: vaht_mov_open grabs its own reference.
    vaht_resource_close(r);
    if (mov == nullptr)
        return out;

    out.resize(size);
    vaht_mov_seek(mov, 0);
    const std::uint32_t got = vaht_mov_read(mov, size, out.data());
    vaht_mov_close(mov);

    out.resize(got);
    return out;
}

// ---------------------------------------------------------------------------

bool looksDamaged(const std::vector<std::uint8_t> &bytes)
{
    if (bytes.empty())
        return true;
    for (const auto b : bytes)
        if (b != 0)
            return false;
    return true;
}

} // namespace riven
