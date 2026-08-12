#include "riven/Rvid.hpp"

#include <algorithm>
#include <cstdlib>

#include "riven/Bits.hpp"

using namespace rivendata;

namespace riven
{
namespace
{
    constexpr int kBlock = kVideoBlock;      // 8
    constexpr int kGroups = 16;              // 2x2 groups per 8x8 block
    constexpr int kCoefs = 4;                // per group
    constexpr int kBlockCoefs = kGroups * kCoefs; // 64

    /// Rate-distortion weight, in SAD units per bit. Low enough that quality
    /// leads and high enough that a block which codes to nothing is preferred
    /// when it looks the same.
    constexpr long kLambda = 6;

    /// Quantise one coefficient. The bias is about a third of a step, which
    /// makes a small dead zone around zero -- the single most effective thing
    /// a quantiser can do for a codec whose residuals are mostly noise.
    inline int quantise(int c, int step)
    {
        const int a = c < 0 ? -c : c;
        const int q = (a + step / 3) / step;
        return c < 0 ? -q : q;
    }

    /// Flatten 16 groups x 4 coefficients into the 64-vector the entropy coder
    /// scans, grouped BY COEFFICIENT: all sixteen DCs, then all sixteen
    /// vertical terms, and so on. That clusters the energy at the front and
    /// leaves one long run of zeroes at the back, which is exactly what a
    /// run/level coder wants.
    inline int flatIndex(int group, int coef) { return coef * kGroups + group; }

    void encodeCoefficients(BitWriter &w, const int v[kBlockCoefs])
    {
        int last = -1;
        for (int i = kBlockCoefs - 1; i >= 0; --i)
            if (v[i] != 0)
            {
                last = i;
                break;
            }

        if (last < 0)
        {
            // An empty block still needs one symbol. (last=1, run=0, level=0)
            // is the shortest thing that cannot be confused with a real one.
            w.putBit(1);
            w.putUE(0);
            w.putSE(0);
            return;
        }

        int run = 0;
        for (int i = 0; i <= last; ++i)
        {
            if (v[i] == 0)
            {
                ++run;
                continue;
            }
            w.putBit(i == last ? 1 : 0);
            w.putUE(static_cast<std::uint32_t>(run));
            w.putSE(v[i]);
            run = 0;
        }
    }

    int costCoefficients(const int v[kBlockCoefs])
    {
        int last = -1;
        for (int i = kBlockCoefs - 1; i >= 0; --i)
            if (v[i] != 0)
            {
                last = i;
                break;
            }
        if (last < 0)
            return 1 + BitWriter::costUE(0) + BitWriter::costSE(0);

        int bits = 0;
        int run = 0;
        for (int i = 0; i <= last; ++i)
        {
            if (v[i] == 0)
            {
                ++run;
                continue;
            }
            bits += 1 + BitWriter::costUE(static_cast<std::uint32_t>(run))
                  + BitWriter::costSE(v[i]);
            run = 0;
        }
        return bits;
    }
} // namespace

void rvidQuantSteps(int quality, int step[4])
{
    const int q = std::clamp(quality, 10, 400);

    // The DC step does NOT follow the quality setting. It carries the group's
    // mean, so an error there moves a whole flat area rather than blurring a
    // detail -- and Riven's movies are letterboxed, so a DC step that cannot
    // represent black exactly turns every border into dark grey. At step 4 a
    // flat block quantises exactly; at step 10 (quality 40) it lands one level
    // off, which measured WORSE than quality 25 whose step happened to divide
    // evenly. Quality scales the three AC terms, which is where the bits are.
    step[0] = kQuantStep[0];
    for (int i = 1; i < 4; ++i)
        step[i] = std::max(1, (kQuantStep[i] * 100 + q / 2) / q);
}

RvidFrame texelsToFrame(const std::vector<Texel> &texels, int w, int h)
{
    RvidFrame f;
    if (w <= 0 || h <= 0 || texels.size() < static_cast<std::size_t>(w) * h)
        return f;

    const int pw = (w + kBlock - 1) / kBlock * kBlock;
    const int ph = (h + kBlock - 1) / kBlock * kBlock;
    f.resize(pw, ph);

    for (int y = 0; y < ph; ++y)
    {
        // Pad by repeating the edge: black would put a hard edge inside the
        // last block of every row and cost real bits to code.
        const int sy = std::min(y, h - 1);
        for (int x = 0; x < pw; ++x)
        {
            const int sx = std::min(x, w - 1);
            const Texel t = texels[static_cast<std::size_t>(sy) * w + sx];
            const std::size_t o = static_cast<std::size_t>(y) * pw + x;
            f.plane[kPlaneR][o] = static_cast<std::uint8_t>(t & 0x1F);
            f.plane[kPlaneG][o] = static_cast<std::uint8_t>((t >> 5) & 0x1F);
            f.plane[kPlaneB][o] = static_cast<std::uint8_t>((t >> 10) & 0x1F);
        }
    }
    return f;
}

std::vector<Texel> frameToTexels(const RvidFrame &f, int w, int h)
{
    std::vector<Texel> out;
    if (f.empty() || w <= 0 || h <= 0)
        return out;
    out.resize(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h && y < f.height; ++y)
        for (int x = 0; x < w && x < f.width; ++x)
        {
            const std::size_t o = static_cast<std::size_t>(y) * f.width + x;
            out[static_cast<std::size_t>(y) * w + x] = static_cast<Texel>(
                0x8000 | (f.plane[kPlaneB][o] << 10) | (f.plane[kPlaneG][o] << 5)
                | f.plane[kPlaneR][o]);
        }
    return out;
}

// ---------------------------------------------------------------------------

RvidEncoder::RvidEncoder(int width, int height, const RvidSettings &settings)
    : width_(width), height_(height), settings_(settings)
{
    blocksX_ = width_ / kBlock;
    blocksY_ = height_ / kBlock;
    rvidQuantSteps(settings_.quality, step_);
    ref_.resize(width_, height_);
    working_.resize(width_, height_);
    vectors_.assign(static_cast<std::size_t>(blocksX_) * blocksY_, Vector{});
}

namespace
{
    /// Encode one 8x8 block of one plane against a prediction that is already
    /// in `work`, writing the reconstruction back into `work`.
    ///
    /// `intra` decides where the prediction comes from: from neighbouring
    /// pixels of this same block (and, for R and B, from the reconstructed G
    /// underneath), or from the motion-compensated pixels the caller has
    /// already written into `work`.
    struct BlockCoder
    {
        const RvidFrame &src;
        const int *step;
        int bx = 0;
        int by = 0;

        int levels[kBlockCoefs] = {};

        /// The block's three planes, 8x8 each. Everything happens in here.
        ///
        /// This is the whole reason the format predicts only from inside the
        /// block: a candidate encoding needs no more state than 192 bytes, so
        /// trying intra and inter and keeping the better one costs two small
        /// buffers instead of two copies of the frame. Copying the frame per
        /// candidate -- which is what this did first -- was 170 MB of memcpy
        /// per frame and made a single stack take longer than the whole rest
        /// of the conversion.
        std::uint8_t scratch[kPlaneCount][kBlock * kBlock] = {};

        std::uint8_t &workAt(int plane, int x, int y)
        {
            return scratch[plane][static_cast<std::size_t>(y) * kBlock + x];
        }
        std::uint8_t srcAt(int plane, int x, int y) const
        {
            return src.plane[plane][static_cast<std::size_t>(by + y) * src.width + bx + x];
        }

        /// Load the motion-compensated prediction as the starting point for an
        /// inter block.
        void loadPrediction(const RvidFrame &frame)
        {
            for (int plane = 0; plane < kPlaneCount; ++plane)
                for (int y = 0; y < kBlock; ++y)
                    std::copy_n(&frame.plane[plane][static_cast<std::size_t>(by + y)
                                                        * frame.width + bx],
                                kBlock, &scratch[plane][static_cast<std::size_t>(y) * kBlock]);
        }

        /// Write the finished block back into the frame.
        void store(RvidFrame &frame) const
        {
            for (int plane = 0; plane < kPlaneCount; ++plane)
                for (int y = 0; y < kBlock; ++y)
                    std::copy_n(&scratch[plane][static_cast<std::size_t>(y) * kBlock], kBlock,
                                &frame.plane[plane][static_cast<std::size_t>(by + y)
                                                        * frame.width + bx]);
        }

        /// Sum of absolute differences between the reconstruction and the
        /// source, over the whole block.
        long distortion() const
        {
            long d = 0;
            for (int plane = 0; plane < kPlaneCount; ++plane)
                for (int y = 0; y < kBlock; ++y)
                    for (int x = 0; x < kBlock; ++x)
                        d += std::abs(
                            static_cast<int>(scratch[plane][static_cast<std::size_t>(y) * kBlock + x])
                            - static_cast<int>(srcAt(plane, x, y)));
            return d;
        }

        /// Returns the number of non-zero levels, so the caller can tell an
        /// empty block from a coded one without rescanning.
        int code(int plane, bool intra)
        {
            for (int i = 0; i < kBlockCoefs; ++i)
                levels[i] = 0;

            for (int g = 0; g < kGroups; ++g)
            {
                int px[4], py[4], pred[4], resid[4];
                for (int k = 0; k < 4; ++k)
                    subBlockPixel(g, k, px[k], py[k]);

                if (intra)
                {
                    int dx = 0, dy = 0;
                    if (intraPredictorOffset(g, dx, dy))
                    {
                        for (int k = 0; k < 4; ++k)
                            pred[k] = workAt(plane, px[k] + dx, py[k] + dy);
                    }
                    else if (plane == kPlaneG)
                    {
                        // Nothing above or to the left inside the block, and G
                        // goes first, so it starts from the middle of the
                        // range rather than from black -- that halves the DC
                        // the first group has to carry.
                        for (int k = 0; k < 4; ++k)
                            pred[k] = 16;
                    }
                    else
                    {
                        // R and B predict from the reconstructed G underneath:
                        // the three planes of a natural image are strongly
                        // correlated, and this is what makes a plane-per-block
                        // codec competitive with a luma/chroma one.
                        for (int k = 0; k < 4; ++k)
                            pred[k] = workAt(kPlaneG, px[k], py[k]);
                    }
                }
                else
                {
                    // Inter: the prediction is already in `work`.
                    for (int k = 0; k < 4; ++k)
                        pred[k] = workAt(plane, px[k], py[k]);
                }

                for (int k = 0; k < 4; ++k)
                    resid[k] = static_cast<int>(srcAt(plane, px[k], py[k])) - pred[k];

                int coef[4];
                forwardHadamard(resid, coef);

                int dequant[4];
                for (int c = 0; c < 4; ++c)
                {
                    const int level = quantise(coef[c], step[c]);
                    levels[flatIndex(g, c)] = level;
                    dequant[c] = level * step[c];
                }

                int out[4];
                inverseHadamard(dequant, pred, out);
                for (int k = 0; k < 4; ++k)
                    workAt(plane, px[k], py[k]) = static_cast<std::uint8_t>(out[k]);
            }

            int nonZero = 0;
            for (int i = 0; i < kBlockCoefs; ++i)
                nonZero += levels[i] != 0 ? 1 : 0;
            return nonZero;
        }
    };
} // namespace

RvidEncodedFrame RvidEncoder::encodeIntra(const RvidFrame &src)
{
    RvidEncodedFrame out;
    out.intra = true;
    out.intraBlocks = blocksX_ * blocksY_;

    BitWriter w;
    int lastDc = 0;

    for (int by = 0; by < blocksY_; ++by)
    {
        for (int bx = 0; bx < blocksX_; ++bx)
        {
            BlockCoder coder{src, step_, bx * kBlock, by * kBlock};
            for (int plane = 0; plane < kPlaneCount; ++plane)
            {
                coder.code(plane, true);

                int v[kBlockCoefs];
                std::copy(std::begin(coder.levels), std::end(coder.levels), v);
                if (plane == kPlaneG)
                {
                    // The G plane's first DC walks along the frame in raster
                    // order: neighbouring blocks of a photograph have similar
                    // brightness, and this is the only prediction that crosses
                    // a block boundary.
                    const int dc = v[0];
                    v[0] = dc - lastDc;
                    lastDc = dc;
                }
                encodeCoefficients(w, v);
            }
            coder.store(working_);
        }
    }

    w.flush();
    out.bytes = w.bytes();

    ref_ = working_;
    haveRef_ = true;
    std::fill(vectors_.begin(), vectors_.end(), Vector{});
    return out;
}

bool RvidEncoder::vectorLegal(int bx, int by, const Vector &v) const
{
    // Every sample the prediction touches must be inside the reference frame,
    // including the +1 a half-pel offset reaches for. Relying on edge clamping
    // instead would tie the encoder to a particular texture layout on the DS.
    const int ix = v.x >> 1;
    const int iy = v.y >> 1;
    const int fx = v.x & 1;
    const int fy = v.y & 1;
    const int x0 = bx * kBlock + ix;
    const int y0 = by * kBlock + iy;
    return x0 >= 0 && y0 >= 0 && x0 + kBlock + fx <= width_ && y0 + kBlock + fy <= height_;
}

namespace
{
    /// One motion-compensated sample, at half-pel, from `ref`.
    inline int mcSample(const RvidFrame &ref, int plane, int x, int y, int fx, int fy)
    {
        const std::size_t o = static_cast<std::size_t>(y) * ref.width + x;
        const int a = ref.plane[plane][o];
        if (fx == 0 && fy == 0)
            return a;
        // Both axes half-pel takes the DIAGONAL neighbour, not a four-tap
        // average: the DS gets half-pel from one 50% blend of two texels, so
        // there are only ever two samples involved.
        const int b = ref.plane[plane][o + static_cast<std::size_t>(fy) * ref.width + fx];
        return blendHalf(a, b);
    }
} // namespace

long RvidEncoder::blockSad(const RvidFrame &src, int bx, int by, const Vector &v) const
{
    const int ix = v.x >> 1;
    const int iy = v.y >> 1;
    const int fx = v.x & 1;
    const int fy = v.y & 1;

    long sad = 0;
    for (int plane = 0; plane < kPlaneCount; ++plane)
    {
        for (int y = 0; y < kBlock; ++y)
        {
            for (int x = 0; x < kBlock; ++x)
            {
                const int px = bx * kBlock + x;
                const int py = by * kBlock + y;
                const int a = src.plane[plane][static_cast<std::size_t>(py) * src.width + px];
                const int b = mcSample(ref_, plane, px + ix, py + iy, fx, fy);
                sad += std::abs(a - b);
            }
        }
    }
    return sad;
}

RvidEncoder::Vector RvidEncoder::predictVector(int bx, int by) const
{
    const auto at = [&](int x, int y) { return vectors_[static_cast<std::size_t>(y) * blocksX_ + x]; };

    const bool haveLeft = bx > 0;
    const bool haveUp = by > 0;
    const bool haveUpRight = by > 0 && bx + 1 < blocksX_;

    if (!haveLeft && !haveUp)
        return {};
    if (!haveUp)
        return at(bx - 1, by);
    if (!haveLeft)
    {
        const Vector a = at(bx, by - 1);
        const Vector b = haveUpRight ? at(bx + 1, by - 1) : a;
        return {std::max(a.x, b.x), std::max(a.y, b.y)};
    }
    if (!haveUpRight)
    {
        const Vector a = at(bx - 1, by);
        const Vector b = at(bx, by - 1);
        return {std::max(a.x, b.x), std::max(a.y, b.y)};
    }

    const Vector a = at(bx - 1, by);
    const Vector b = at(bx, by - 1);
    const Vector c = at(bx + 1, by - 1);
    const auto median = [](int p, int q, int r) {
        return std::max(std::min(p, q), std::min(std::max(p, q), r));
    };
    return {median(a.x, b.x, c.x), median(a.y, b.y, c.y)};
}

RvidEncoder::Vector RvidEncoder::searchVector(const RvidFrame &src, int bx, int by,
                                              const Vector &predictor) const
{
    const auto cost = [&](const Vector &v) -> long {
        if (!vectorLegal(bx, by, v))
            return -1;
        const int bits = (v.x == predictor.x && v.y == predictor.y)
                             ? 1
                             : 1 + BitWriter::costSE(v.x - predictor.x)
                                   + BitWriter::costSE(v.y - predictor.y);
        return blockSad(src, bx, by, v) + kLambda * bits;
    };

    Vector best{};
    long bestCost = cost(best);
    if (bestCost < 0)
        bestCost = 1L << 60;

    if (const long c = cost(predictor); c >= 0 && c < bestCost)
    {
        best = predictor;
        bestCost = c;
    }

    // Diamond search, large steps then small. Bounded by an evaluation budget
    // rather than a window: a locked-off Riven shot converges in a handful of
    // steps and a panning one is allowed to walk as far as it needs.
    static const int kLarge[8][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2},
                                     {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    static const int kSmall[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

    for (int pass = 0; pass < 2; ++pass)
    {
        const int (*pattern)[2] = pass == 0 ? kLarge : kSmall;
        const int points = pass == 0 ? 8 : 4;
        int budget = 128;

        bool improved = true;
        while (improved && budget > 0)
        {
            improved = false;
            for (int i = 0; i < points && budget > 0; ++i, --budget)
            {
                const Vector cand{best.x + pattern[i][0], best.y + pattern[i][1]};
                const long c = cost(cand);
                if (c >= 0 && c < bestCost)
                {
                    best = cand;
                    bestCost = c;
                    improved = true;
                }
            }
        }
    }

    return best;
}

RvidEncodedFrame RvidEncoder::encodeInter(const RvidFrame &src)
{
    RvidEncodedFrame out;
    out.intra = false;

    const bool searchMotion = settings_.profile == VideoProfile::Full;

    BitWriter vectorBits;
    BitWriter dctBits;

    for (int by = 0; by < blocksY_; ++by)
    {
        for (int bx = 0; bx < blocksX_; ++bx)
        {
            const std::size_t bi = static_cast<std::size_t>(by) * blocksX_ + bx;
            const Vector predictor = searchMotion ? predictVector(bx, by) : Vector{};
            const Vector mv = searchMotion ? searchVector(src, bx, by, predictor) : Vector{};

            // --- the motion-compensated prediction --------------------------
            const int ix = mv.x >> 1;
            const int iy = mv.y >> 1;
            const int fx = mv.x & 1;
            const int fy = mv.y & 1;

            // --- try inter -------------------------------------------------
            BlockCoder inter{src, step_, bx * kBlock, by * kBlock};
            for (int plane = 0; plane < kPlaneCount; ++plane)
                for (int y = 0; y < kBlock; ++y)
                    for (int x = 0; x < kBlock; ++x)
                        inter.workAt(plane, x, y) = static_cast<std::uint8_t>(mcSample(
                            ref_, plane, bx * kBlock + x + ix, by * kBlock + y + iy, fx, fy));

            int interBits = 1; // the I/P flag
            int interLevels[kPlaneCount][kBlockCoefs];
            bool planeCoded[kPlaneCount] = {false, false, false};
            for (int plane = 0; plane < kPlaneCount; ++plane)
            {
                const int nonZero = inter.code(plane, false);
                std::copy(std::begin(inter.levels), std::end(inter.levels),
                          interLevels[plane]);
                planeCoded[plane] = nonZero > 0;
                interBits += 1; // the per-plane "has coefficients" flag
                if (nonZero > 0)
                    interBits += costCoefficients(interLevels[plane]);
            }
            if (searchMotion)
                interBits += (mv.x == predictor.x && mv.y == predictor.y)
                                 ? 1
                                 : 1 + BitWriter::costSE(mv.x - predictor.x)
                                       + BitWriter::costSE(mv.y - predictor.y);

            // --- try intra -------------------------------------------------
            BlockCoder intra{src, step_, bx * kBlock, by * kBlock};
            int intraBits = 1;
            int intraLevels[kPlaneCount][kBlockCoefs];
            for (int plane = 0; plane < kPlaneCount; ++plane)
            {
                intra.code(plane, true);
                std::copy(std::begin(intra.levels), std::end(intra.levels),
                          intraLevels[plane]);
                intraBits += costCoefficients(intraLevels[plane]);
            }
            if (searchMotion)
                intraBits += 1; // intra still writes "vector equals prediction"

            // Straight rate-distortion: whichever of the two costs less, where
            // a bit is worth kLambda units of absolute error.
            const long interCost = inter.distortion() + kLambda * interBits;
            const long intraCost = intra.distortion() + kLambda * intraBits;
            const bool useIntra = intraCost < interCost;

            if (useIntra)
            {
                ++out.intraBlocks;
                intra.store(working_);
                vectors_[bi] = predictor; // keep neighbours consistent
                if (searchMotion)
                    vectorBits.putBit(1);
                dctBits.putBit(1);
                for (int plane = 0; plane < kPlaneCount; ++plane)
                    encodeCoefficients(dctBits, intraLevels[plane]);
            }
            else
            {
                inter.store(working_);
                vectors_[bi] = mv;
                if (searchMotion)
                {
                    if (mv.x == predictor.x && mv.y == predictor.y)
                        vectorBits.putBit(1);
                    else
                    {
                        vectorBits.putBit(0);
                        vectorBits.putSE(mv.x - predictor.x);
                        vectorBits.putSE(mv.y - predictor.y);
                    }
                }
                dctBits.putBit(0);
                for (int plane = 0; plane < kPlaneCount; ++plane)
                {
                    dctBits.putBit(planeCoded[plane] ? 1 : 0);
                    if (planeCoded[plane])
                        encodeCoefficients(dctBits, interLevels[plane]);
                }
            }
        }
    }

    vectorBits.flush();
    dctBits.flush();

    // A P-frame is the vector sub-stream then the coefficient sub-stream, with
    // a length so the decoder can find the second without decoding the first.
    // LITE has no vectors at all, so it has no prefix either.
    const auto &vb = vectorBits.bytes();
    const auto &db = dctBits.bytes();
    if (searchMotion)
    {
        out.bytes.reserve(2 + vb.size() + db.size());
        out.bytes.push_back(static_cast<std::uint8_t>(vb.size() & 0xFF));
        out.bytes.push_back(static_cast<std::uint8_t>((vb.size() >> 8) & 0xFF));
        out.bytes.insert(out.bytes.end(), vb.begin(), vb.end());
    }
    out.bytes.insert(out.bytes.end(), db.begin(), db.end());

    ref_ = working_;
    return out;
}

RvidEncodedFrame RvidEncoder::encode(const RvidFrame &src, bool forceIntra)
{
    if (!haveRef_ || forceIntra)
        return encodeIntra(src);

    RvidEncodedFrame inter = encodeInter(src);

    // Scene change: if most of the frame gave up on prediction, the reference
    // is worthless and an I-frame is both smaller and a seek point.
    const int blocks = blocksX_ * blocksY_;
    if (blocks > 0
        && static_cast<double>(inter.intraBlocks) > settings_.intraRatio * blocks)
    {
        working_ = ref_; // discard the P-frame's reconstruction
        return encodeIntra(src);
    }
    return inter;
}

} // namespace riven
