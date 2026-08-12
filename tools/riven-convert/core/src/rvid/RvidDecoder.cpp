#include "riven/Rvid.hpp"

#include <algorithm>

#include "riven/Bits.hpp"

using namespace rivendata;

namespace riven
{
namespace
{
    constexpr int kBlock = kVideoBlock;
    constexpr int kGroups = 16;
    constexpr int kBlockCoefs = kGroups * 4;

    inline int flatIndex(int group, int coef) { return coef * kGroups + group; }

    /// Read one block's coefficients. Mirrors encodeCoefficients exactly --
    /// the two are the only places the symbol layout is written down, and they
    /// are checked against each other by test_video's round trip.
    bool decodeCoefficients(BitReader &r, int v[kBlockCoefs])
    {
        for (int i = 0; i < kBlockCoefs; ++i)
            v[i] = 0;

        int pos = 0;
        for (;;)
        {
            const int last = r.getBit();
            const std::uint32_t run = r.getUE();
            const std::int32_t level = r.getSE();
            if (!r.ok())
                return false;

            pos += static_cast<int>(run);
            if (pos >= kBlockCoefs)
                return level == 0 && last != 0; // the empty-block symbol
            v[pos] = level;
            ++pos;

            if (last != 0)
                return true;
            if (pos >= kBlockCoefs)
                return false; // ran past the end without a last flag
        }
    }

    inline int mcSample(const RvidFrame &ref, int plane, int x, int y, int fx, int fy)
    {
        const std::size_t o = static_cast<std::size_t>(y) * ref.width + x;
        const int a = ref.plane[plane][o];
        if (fx == 0 && fy == 0)
            return a;
        const int b = ref.plane[plane][o + static_cast<std::size_t>(fy) * ref.width + fx];
        return blendHalf(a, b);
    }

    /// Reconstruct one 8x8 block of one plane from its levels. `frame` already
    /// holds the prediction for an inter block; for an intra block the
    /// predictor comes from the pixels this function is writing.
    void reconstruct(RvidFrame &frame, int plane, int bx, int by, const int levels[kBlockCoefs],
                     const int step[4], bool intra)
    {
        const auto at = [&](int p, int x, int y) -> std::uint8_t & {
            return frame.plane[p][static_cast<std::size_t>(by + y) * frame.width + bx + x];
        };

        for (int g = 0; g < kGroups; ++g)
        {
            int px[4], py[4], pred[4];
            for (int k = 0; k < 4; ++k)
                subBlockPixel(g, k, px[k], py[k]);

            if (intra)
            {
                int dx = 0, dy = 0;
                if (intraPredictorOffset(g, dx, dy))
                    for (int k = 0; k < 4; ++k)
                        pred[k] = at(plane, px[k] + dx, py[k] + dy);
                else if (plane == kPlaneG)
                    for (int k = 0; k < 4; ++k)
                        pred[k] = 16;
                else
                    for (int k = 0; k < 4; ++k)
                        pred[k] = at(kPlaneG, px[k], py[k]);
            }
            else
            {
                for (int k = 0; k < 4; ++k)
                    pred[k] = at(plane, px[k], py[k]);
            }

            int dequant[4];
            for (int c = 0; c < 4; ++c)
                dequant[c] = levels[flatIndex(g, c)] * step[c];

            int out[4];
            inverseHadamard(dequant, pred, out);
            for (int k = 0; k < 4; ++k)
                at(plane, px[k], py[k]) = static_cast<std::uint8_t>(out[k]);
        }
    }
} // namespace

RvidDecoder::RvidDecoder(int width, int height, const RvidSettings &settings)
    : width_(width), height_(height), profile_(settings.profile)
{
    blocksX_ = width_ / kBlock;
    blocksY_ = height_ / kBlock;
    rvidQuantSteps(settings.quality, step_);
    frame_.resize(width_, height_);
    prev_.resize(width_, height_);
}

bool RvidDecoder::decode(const std::uint8_t *data, std::size_t size, bool intra)
{
    error_.clear();
    if (data == nullptr)
    {
        error_ = "no frame data";
        return false;
    }

    prev_ = frame_;

    if (intra)
    {
        BitReader r(data, size);
        int lastDc = 0;

        for (int by = 0; by < blocksY_; ++by)
            for (int bx = 0; bx < blocksX_; ++bx)
                for (int plane = 0; plane < kPlaneCount; ++plane)
                {
                    int levels[kBlockCoefs];
                    if (!decodeCoefficients(r, levels))
                    {
                        error_ = "truncated intra frame";
                        return false;
                    }
                    if (plane == kPlaneG)
                    {
                        levels[0] += lastDc;
                        lastDc = levels[0];
                    }
                    reconstruct(frame_, plane, bx * kBlock, by * kBlock, levels, step_, true);
                }
        return true;
    }

    // --- inter -------------------------------------------------------------
    const bool hasVectors = profile_ == VideoProfile::Full;
    std::size_t dctOffset = 0;
    std::vector<int> mvx(static_cast<std::size_t>(blocksX_) * blocksY_, 0);
    std::vector<int> mvy(mvx.size(), 0);

    if (hasVectors)
    {
        if (size < 2)
        {
            error_ = "inter frame is too short for its vector length";
            return false;
        }
        const std::size_t vectorBytes =
            static_cast<std::size_t>(data[0]) | (static_cast<std::size_t>(data[1]) << 8);
        if (2 + vectorBytes > size)
        {
            error_ = "vector sub-stream runs past the frame";
            return false;
        }
        dctOffset = 2 + vectorBytes;

        BitReader vr(data + 2, vectorBytes);
        for (int by = 0; by < blocksY_; ++by)
            for (int bx = 0; bx < blocksX_; ++bx)
            {
                const std::size_t bi = static_cast<std::size_t>(by) * blocksX_ + bx;

                // The same median predictor the encoder used, from the vectors
                // already decoded this frame.
                int px = 0, py = 0;
                const bool haveLeft = bx > 0;
                const bool haveUp = by > 0;
                const bool haveUpRight = by > 0 && bx + 1 < blocksX_;
                const auto vx = [&](int x, int y) { return mvx[static_cast<std::size_t>(y) * blocksX_ + x]; };
                const auto vy = [&](int x, int y) { return mvy[static_cast<std::size_t>(y) * blocksX_ + x]; };
                if (!haveLeft && !haveUp)
                {
                    px = py = 0;
                }
                else if (!haveUp)
                {
                    px = vx(bx - 1, by);
                    py = vy(bx - 1, by);
                }
                else if (!haveLeft)
                {
                    const int ax = vx(bx, by - 1), ay = vy(bx, by - 1);
                    const int bx2 = haveUpRight ? vx(bx + 1, by - 1) : ax;
                    const int by2 = haveUpRight ? vy(bx + 1, by - 1) : ay;
                    px = std::max(ax, bx2);
                    py = std::max(ay, by2);
                }
                else if (!haveUpRight)
                {
                    px = std::max(vx(bx - 1, by), vx(bx, by - 1));
                    py = std::max(vy(bx - 1, by), vy(bx, by - 1));
                }
                else
                {
                    const auto median = [](int p, int q, int r2) {
                        return std::max(std::min(p, q), std::min(std::max(p, q), r2));
                    };
                    px = median(vx(bx - 1, by), vx(bx, by - 1), vx(bx + 1, by - 1));
                    py = median(vy(bx - 1, by), vy(bx, by - 1), vy(bx + 1, by - 1));
                }

                if (vr.getBit() != 0)
                {
                    mvx[bi] = px;
                    mvy[bi] = py;
                }
                else
                {
                    mvx[bi] = px + vr.getSE();
                    mvy[bi] = py + vr.getSE();
                }
                if (!vr.ok())
                {
                    error_ = "truncated vector sub-stream";
                    return false;
                }
            }
    }

    BitReader r(data + dctOffset, size - dctOffset);
    for (int by = 0; by < blocksY_; ++by)
        for (int bx = 0; bx < blocksX_; ++bx)
        {
            const std::size_t bi = static_cast<std::size_t>(by) * blocksX_ + bx;
            const bool isIntra = r.getBit() != 0;

            // The prediction, either way, comes from the previous frame at the
            // block's motion vector. An intra block overwrites it entirely.
            const int ix = mvx[bi] >> 1;
            const int iy = mvy[bi] >> 1;
            const int fx = mvx[bi] & 1;
            const int fy = mvy[bi] & 1;
            for (int plane = 0; plane < kPlaneCount; ++plane)
                for (int y = 0; y < kBlock; ++y)
                    for (int x = 0; x < kBlock; ++x)
                    {
                        const int dstX = bx * kBlock + x;
                        const int dstY = by * kBlock + y;
                        const int sx = dstX + ix;
                        const int sy = dstY + iy;
                        int value = 0;
                        if (sx >= 0 && sy >= 0 && sx + fx < width_ && sy + fy < height_)
                            value = mcSample(prev_, plane, sx, sy, fx, fy);
                        frame_.plane[plane][static_cast<std::size_t>(dstY) * width_ + dstX] =
                            static_cast<std::uint8_t>(value);
                    }

            for (int plane = 0; plane < kPlaneCount; ++plane)
            {
                int levels[kBlockCoefs];
                if (isIntra)
                {
                    if (!decodeCoefficients(r, levels))
                    {
                        error_ = "truncated intra block in an inter frame";
                        return false;
                    }
                    reconstruct(frame_, plane, bx * kBlock, by * kBlock, levels, step_, true);
                }
                else
                {
                    const bool coded = r.getBit() != 0;
                    if (!coded)
                        continue; // the prediction stands
                    if (!decodeCoefficients(r, levels))
                    {
                        error_ = "truncated inter block";
                        return false;
                    }
                    reconstruct(frame_, plane, bx * kBlock, by * kBlock, levels, step_, false);
                }
            }
        }

    return r.ok();
}

} // namespace riven
