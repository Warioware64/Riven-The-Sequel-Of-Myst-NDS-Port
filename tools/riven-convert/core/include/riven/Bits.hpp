#pragma once

// Bit IO for the RVID bitstream.
//
// MSB-first into a plain byte stream. Not the little-endian-halfword packing
// the reference player uses -- that is an ARM load-and-shift optimisation, and
// choosing it here would fix the DS decoder's inner loop before it exists.
// Byte order costs nothing to change later and the format carries a version.
//
// The codes are Exp-Golomb rather than a Huffman table, which is a licensing
// decision as much as a design one: see docs/video.md. Both ends are ours, so
// there is nothing to be compatible with, and a structured code needs no table
// to ship, to load, or to have been copied from somewhere.

#include <cstdint>
#include <vector>

namespace riven
{

class BitWriter
{
public:
    void putBit(int bit)
    {
        acc_ = (acc_ << 1) | (bit != 0 ? 1u : 0u);
        if (++bits_ == 8)
        {
            bytes_.push_back(static_cast<std::uint8_t>(acc_));
            acc_ = 0;
            bits_ = 0;
        }
    }

    void put(std::uint32_t value, int n)
    {
        for (int i = n - 1; i >= 0; --i)
            putBit(static_cast<int>((value >> i) & 1u));
    }

    /// Unsigned Exp-Golomb: `n` zeroes, then value+1 in n+1 bits.
    void putUE(std::uint32_t value)
    {
        const std::uint32_t v = value + 1;
        const int n = bitLength(v) - 1;
        for (int i = 0; i < n; ++i)
            putBit(0);
        put(v, n + 1);
    }

    /// Signed, folded to unsigned as 0, -1, 1, -2, 2, ...
    void putSE(std::int32_t value)
    {
        const std::uint32_t folded =
            value == 0 ? 0u
                       : (value > 0 ? static_cast<std::uint32_t>(2 * value - 1)
                                    : static_cast<std::uint32_t>(-2 * value));
        putUE(folded);
    }

    /// Pad to a byte boundary with zeroes.
    void flush()
    {
        while (bits_ != 0)
            putBit(0);
    }

    std::size_t sizeBits() const { return bytes_.size() * 8 + bits_; }
    const std::vector<std::uint8_t> &bytes() const { return bytes_; }

    static int bitLength(std::uint32_t v)
    {
        int n = 0;
        while (v != 0)
        {
            ++n;
            v >>= 1;
        }
        return n;
    }

    /// What putUE would cost, without writing anything. The rate term of every
    /// rate-distortion decision in the encoder.
    static int costUE(std::uint32_t value) { return 2 * (bitLength(value + 1) - 1) + 1; }

    static int costSE(std::int32_t value)
    {
        const std::uint32_t folded =
            value == 0 ? 0u
                       : (value > 0 ? static_cast<std::uint32_t>(2 * value - 1)
                                    : static_cast<std::uint32_t>(-2 * value));
        return costUE(folded);
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint32_t acc_ = 0;
    int bits_ = 0;
};

class BitReader
{
public:
    BitReader(const std::uint8_t *data, std::size_t size) : data_(data), size_(size) {}

    bool ok() const { return ok_; }

    int getBit()
    {
        if (pos_ >= size_ * 8)
        {
            ok_ = false;
            return 0;
        }
        const int bit = (data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
        ++pos_;
        return bit;
    }

    std::uint32_t get(int n)
    {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | static_cast<std::uint32_t>(getBit());
        return v;
    }

    std::uint32_t getUE()
    {
        int n = 0;
        while (ok_ && getBit() == 0)
        {
            if (++n > 31)
            {
                ok_ = false;
                return 0;
            }
        }
        if (!ok_)
            return 0;
        const std::uint32_t v = (1u << n) | get(n);
        return v - 1;
    }

    std::int32_t getSE()
    {
        const std::uint32_t folded = getUE();
        if (folded == 0)
            return 0;
        return (folded & 1) != 0 ? static_cast<std::int32_t>((folded + 1) / 2)
                                 : -static_cast<std::int32_t>(folded / 2);
    }

    /// Skip to the next byte boundary.
    void align() { pos_ = (pos_ + 7) & ~std::size_t(7); }

    std::size_t bytesRead() const { return (pos_ + 7) / 8; }

private:
    const std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace riven
