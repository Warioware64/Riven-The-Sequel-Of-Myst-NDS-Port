#pragma once

// Big-endian reader over a raw resource buffer.
//
// libvaht covers most of Riven's resources, but not MLST, FLST or SFXE, and it
// exposes no generic byte reader for the ones it misses. Rather than reach into
// its internals, those three parsers pull the resource out with
// vaht_resource_read and walk it with this.
//
// Every read is bounds-checked and sets a sticky failure flag instead of
// throwing or reading past the end. Riven's shipped data contains records that
// are simply wrong (riven_card.cpp notes an invalid hotspot rect on tspit
// 371/377), so a parser that survives bad input and reports it beats one that
// aborts a whole stack.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace riven
{

class ResourceReader
{
public:
    ResourceReader(const std::uint8_t *data, std::size_t size)
        : data_(data), size_(size)
    {
    }

    explicit ResourceReader(const std::vector<std::uint8_t> &v)
        : data_(v.data()), size_(v.size())
    {
    }

    /// Binding to a temporary would leave every read dangling, and the failure
    /// is silent: the parser sees whatever is now in that freed memory and
    /// usually just returns "no records", which looks exactly like a card that
    /// legitimately has none. `ResourceReader r(set.read("PLST", id));` cost an
    /// afternoon before this overload existed. Keep the bytes in a named local.
    explicit ResourceReader(std::vector<std::uint8_t> &&) = delete;

    bool ok() const { return ok_; }
    std::size_t pos() const { return pos_; }
    std::size_t size() const { return size_; }
    std::size_t remaining() const { return pos_ < size_ ? size_ - pos_ : 0; }
    bool atEnd() const { return pos_ >= size_; }

    void seek(std::size_t p)
    {
        if (p > size_)
        {
            ok_ = false;
            pos_ = size_;
            return;
        }
        pos_ = p;
    }

    void skip(std::size_t n) { seek(pos_ + n); }

    std::uint8_t u8()
    {
        if (!need(1))
            return 0;
        return data_[pos_++];
    }

    std::uint16_t u16()
    {
        if (!need(2))
            return 0;
        const std::uint16_t v = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(data_[pos_]) << 8) | data_[pos_ + 1]);
        pos_ += 2;
        return v;
    }

    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }

    std::uint32_t u32()
    {
        if (!need(4))
            return 0;
        const std::uint32_t v = (static_cast<std::uint32_t>(data_[pos_]) << 24)
                              | (static_cast<std::uint32_t>(data_[pos_ + 1]) << 16)
                              | (static_cast<std::uint32_t>(data_[pos_ + 2]) << 8)
                              | static_cast<std::uint32_t>(data_[pos_ + 3]);
        pos_ += 4;
        return v;
    }

    /// Four-character resource tag, as a plain std::string for comparison.
    std::string tag()
    {
        if (!need(4))
            return {};
        std::string s(reinterpret_cast<const char *>(data_ + pos_), 4);
        pos_ += 4;
        return s;
    }

private:
    bool need(std::size_t n)
    {
        if (pos_ + n > size_)
        {
            ok_ = false;
            pos_ = size_;
            return false;
        }
        return true;
    }

    const std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace riven
