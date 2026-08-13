#include "riven/Dcl.hpp"

#include <array>

namespace riven
{
namespace
{
    // --- the fixed tables ---------------------------------------------------
    //
    // Run-length-encoded code LENGTHS, one run per byte: (repeat - 1) << 4 with
    // the length in the low nibble. These are the format's own tables, the same
    // numbers in every implementation of it; there is no choice to be made here.

    /// Literal lengths, 256 symbols in 98 bytes. Only consulted when the header
    /// says the stream codes its literals; Riven's entries do not, but the mode
    /// exists and a reader without it would fail on somebody else's archive.
    constexpr std::uint8_t kLiteralLengths[] = {
        11,  124, 8,   7,   28,  7,   188, 13,  76,  4,   10,  8,   12,  10,  12,  10,
        8,   23,  8,   9,   7,   6,   7,   8,   7,   6,   55,  8,   23,  24,  12,  11,
        7,   9,   11,  12,  6,   7,   22,  5,   7,   24,  6,   11,  9,   6,   7,   22,
        7,   11,  38,  7,   9,   8,   25,  11,  8,   11,  9,   12,  8,   12,  5,   38,
        5,   38,  5,   11,  7,   5,   6,   21,  6,   10,  53,  8,   7,   24,  10,  27,
        44,  253, 253, 253, 252, 252, 252, 13,  12,  45,  12,  45,  12,  61,  12,  45,
        44,  173};

    /// Match lengths, 16 symbols in 6 bytes.
    constexpr std::uint8_t kLengthLengths[] = {2, 35, 36, 53, 38, 23};

    /// Distance high bits, 64 symbols in 7 bytes.
    constexpr std::uint8_t kDistanceLengths[] = {2, 20, 53, 230, 247, 151, 248};

    /// Base length for each of the 16 length symbols, and how many extra bits
    /// follow it. Symbol 15 with its 8 extra bits reaches 519, which is the
    /// end-of-stream marker rather than a real length.
    constexpr std::uint16_t kLengthBase[16] = {3,  2,  4,  5,  6,  7,   8,   9,
                                               10, 12, 16, 24, 40, 72, 136, 264};
    constexpr std::uint8_t kLengthExtra[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};

    /// One decoded Huffman table: for each symbol, its code and code length.
    /// Small enough (256 max) that decoding bit by bit against a linear scan
    /// would work, but the counts/offsets form is what makes it O(bits).
    struct Huffman
    {
        /// Number of codes of each length, 1..16.
        std::array<std::uint16_t, 17> count{};
        /// Symbols ordered by code length, then by symbol.
        std::vector<std::uint16_t> symbol;
        /// The table expanded to the symbol count the format says it has.
        bool valid = false;
    };

    /// Expand a run-length-encoded length table and build the decode structure.
    Huffman buildHuffman(const std::uint8_t *rle, std::size_t rleBytes, std::size_t symbols)
    {
        std::vector<std::uint8_t> lengths;
        lengths.reserve(symbols);
        for (std::size_t i = 0; i < rleBytes; ++i)
        {
            const int repeat = (rle[i] >> 4) + 1;
            const std::uint8_t len = rle[i] & 0x0F;
            for (int r = 0; r < repeat; ++r)
                lengths.push_back(len);
        }

        Huffman h;
        // The tables above are transcribed constants and a miscount in one is
        // silent: the stream still decodes, into rubbish. Checking here turns
        // that into a decoder that refuses to run at all.
        h.valid = lengths.size() == symbols;
        if (!h.valid)
            return h;

        for (const std::uint8_t len : lengths)
            ++h.count[len];

        // Offsets of each length's first symbol, then fill.
        std::array<std::uint16_t, 17> offset{};
        for (int len = 1; len < 16; ++len)
            offset[len + 1] = static_cast<std::uint16_t>(offset[len] + h.count[len]);

        h.symbol.assign(lengths.size(), 0);
        for (std::size_t s = 0; s < lengths.size(); ++s)
            if (lengths[s] != 0)
                h.symbol[offset[lengths[s]]++] = static_cast<std::uint16_t>(s);
        return h;
    }

    /// LSB-first bit reader over the compressed bytes.
    class BitReader
    {
    public:
        BitReader(const std::uint8_t *data, std::size_t size) : data_(data), size_(size) {}

        /// `n` bits, LSB first. Sets overrun() and returns 0 past the end.
        int bits(int n)
        {
            int value = 0;
            for (int i = 0; i < n; ++i)
            {
                if (held_ == 0)
                {
                    if (pos_ >= size_)
                    {
                        overrun_ = true;
                        return 0;
                    }
                    bitBuf_ = data_[pos_++];
                    held_ = 8;
                }
                value |= (bitBuf_ & 1) << i;
                bitBuf_ >>= 1;
                --held_;
            }
            return value;
        }

        /// One Huffman symbol. The code bits arrive one at a time and are
        /// accumulated MSB-first with each bit INVERTED -- that inversion is
        /// part of the format, not a transcription slip, and without it the
        /// stream still decodes and is nonsense.
        int symbol(const Huffman &h)
        {
            int code = 0;
            int first = 0;
            int index = 0;
            for (int len = 1; len <= 16; ++len)
            {
                code |= bits(1) ^ 1;
                if (overrun_)
                    return -1;
                const int count = h.count[len];
                if (code - first < count)
                    return h.symbol[static_cast<std::size_t>(index + (code - first))];
                index += count;
                first = (first + count) << 1;
                code <<= 1;
            }
            return -1; // no code that long: the stream is not what it claims
        }

        bool overrun() const { return overrun_; }

    private:
        const std::uint8_t *data_;
        std::size_t size_;
        std::size_t pos_ = 0;
        std::uint8_t bitBuf_ = 0;
        int held_ = 0;
        bool overrun_ = false;
    };
} // namespace

std::vector<std::uint8_t> decompressDcl(const std::uint8_t *data, std::size_t size,
                                        std::size_t expected, std::string &error)
{
    error.clear();
    if (data == nullptr || size < 2)
    {
        error = "the compressed stream is too short to have a header";
        return {};
    }

    const int literalMode = data[0];
    const int dictBits = data[1];
    if (literalMode != 0 && literalMode != 1)
    {
        error = "unknown DCL literal mode";
        return {};
    }
    if (dictBits < 4 || dictBits > 6)
    {
        error = "unknown DCL dictionary size";
        return {};
    }

    static const Huffman literals =
        buildHuffman(kLiteralLengths, sizeof(kLiteralLengths), 256);
    static const Huffman lengths = buildHuffman(kLengthLengths, sizeof(kLengthLengths), 16);
    static const Huffman distances =
        buildHuffman(kDistanceLengths, sizeof(kDistanceLengths), 64);
    if (!literals.valid || !lengths.valid || !distances.valid)
    {
        error = "the DCL tables in this build are wrong";
        return {};
    }

    BitReader in(data + 2, size - 2);
    std::vector<std::uint8_t> out;
    out.reserve(expected);

    while (true)
    {
        // 1 = a back reference, 0 = a literal.
        const int isMatch = in.bits(1);
        if (in.overrun())
        {
            error = "the compressed stream ends in the middle of a token";
            return {};
        }

        if (isMatch == 0)
        {
            const int lit = literalMode ? in.symbol(literals) : in.bits(8);
            if (lit < 0 || in.overrun())
            {
                error = "the compressed stream ends in the middle of a literal";
                return {};
            }
            out.push_back(static_cast<std::uint8_t>(lit));
        }
        else
        {
            const int lenSym = in.symbol(lengths);
            if (lenSym < 0 || lenSym > 15)
            {
                error = "the compressed stream has an invalid length code";
                return {};
            }
            const int len = kLengthBase[lenSym] + in.bits(kLengthExtra[lenSym]);
            if (len == 519)
                break; // end of stream

            // A two-byte match uses a 2-bit low field whatever the dictionary
            // size is; everything longer uses the full dictionary width.
            const int lowBits = len == 2 ? 2 : dictBits;
            const int distSym = in.symbol(distances);
            if (distSym < 0 || in.overrun())
            {
                error = "the compressed stream ends in the middle of a match";
                return {};
            }
            const std::size_t distance =
                (static_cast<std::size_t>(distSym) << lowBits)
                + static_cast<std::size_t>(in.bits(lowBits)) + 1;

            if (distance > out.size())
            {
                error = "the compressed stream references data before the start";
                return {};
            }

            // Byte by byte on purpose: DCL matches routinely overlap the output
            // they are still writing, which is how it encodes runs.
            std::size_t from = out.size() - distance;
            for (int i = 0; i < len; ++i)
                out.push_back(out[from++]);
        }

        if (expected != 0 && out.size() > expected)
        {
            error = "the compressed stream expands past the size it claims";
            return {};
        }
    }

    if (expected != 0 && out.size() != expected)
    {
        error = "the compressed stream is the wrong length when decoded";
        return {};
    }
    return out;
}

} // namespace riven
