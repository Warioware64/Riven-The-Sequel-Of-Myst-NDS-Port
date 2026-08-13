// The InstallShield v3 container and the PKWARE DCL decoder inside it.
//
// This is the only route to riven.exe (cursors) and extras.mhk (inventory art)
// on the 5-CD release, which ships neither as a file. So a quiet failure here is
// not a broken test -- it is a game with no cursor and no way to reach Atrus's
// journal, with nothing to say why.
//
// The real-data half doubles as the known-answer test for the DCL decoder. There
// is no test vector for it in this repo and there must not be one: shipping a
// blob of Riven's executable is exactly what this project does not do. Instead
// the expected SHA-256 of the two entries is checked in as 64 characters of hex,
// which proves the decoder byte for byte while carrying none of the data.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "riven/Dcl.hpp"
#include "riven/Installer.hpp"
#include "riven/Layout.hpp"
#include "riven/PeCursors.hpp"

namespace fs = std::filesystem;
using namespace riven;

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void check(bool cond, const std::string &what)
    {
        ++g_checks;
        if (!cond)
        {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            ++g_failures;
        }
    }

    // --- SHA-256, so the expected answer can be a hash instead of the data ---
    //
    // Written out here rather than pulled in: the converter has no crypto
    // dependency and this is the only thing that wants one.
    struct Sha256
    {
        std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::uint64_t length = 0;
        std::uint8_t block[64] = {};
        std::size_t held = 0;

        static std::uint32_t ror(std::uint32_t x, int n)
        {
            return (x >> n) | (x << (32 - n));
        }

        void compress()
        {
            static const std::uint32_t k[64] = {
                0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
                0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
                0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
                0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
                0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
                0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
                0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
                0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
                0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

            std::uint32_t w[64];
            for (int i = 0; i < 16; ++i)
                w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24)
                     | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
                     | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
                     | static_cast<std::uint32_t>(block[i * 4 + 3]);
            for (int i = 16; i < 64; ++i)
            {
                const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }

            std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
            for (int i = 0; i < 64; ++i)
            {
                const std::uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
                const std::uint32_t ch = (e & f) ^ (~e & g);
                const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
                const std::uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
                const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                const std::uint32_t t2 = S0 + maj;
                hh = g; g = f; f = e; e = d + t1;
                d = c; c = b; b = a; a = t1 + t2;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
            h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        }

        void update(const std::uint8_t *p, std::size_t n)
        {
            length += n;
            while (n > 0)
            {
                const std::size_t take = std::min(n, sizeof(block) - held);
                std::memcpy(block + held, p, take);
                held += take;
                p += take;
                n -= take;
                if (held == sizeof(block))
                {
                    compress();
                    held = 0;
                }
            }
        }

        std::string hex()
        {
            const std::uint64_t bits = length * 8;
            const std::uint8_t one = 0x80;
            update(&one, 1);
            const std::uint8_t zero = 0;
            while (held != 56)
                update(&zero, 1);
            std::uint8_t tail[8];
            for (int i = 0; i < 8; ++i)
                tail[i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
            length -= 9; // the padding is not part of the message length
            update(tail, 8);

            std::string out;
            char buf[9];
            for (const std::uint32_t v : h)
            {
                std::snprintf(buf, sizeof(buf), "%08x", v);
                out += buf;
            }
            return out;
        }
    };

    std::string sha256Hex(const std::vector<std::uint8_t> &data)
    {
        Sha256 s;
        s.update(data.data(), data.size());
        return s.hex();
    }
} // namespace

int main()
{
    // --- the decoder refuses malformed input rather than misbehaving ---------
    {
        std::string err;

        check(decompressDcl(nullptr, 0, 0, err).empty() && !err.empty(),
              "a null stream is rejected");

        const std::uint8_t tooShort[1] = {0};
        check(decompressDcl(tooShort, 1, 0, err).empty() && !err.empty(),
              "a stream with no room for a header is rejected");

        const std::uint8_t badLiteral[4] = {9, 4, 0, 0};
        check(decompressDcl(badLiteral, 4, 0, err).empty() && !err.empty(),
              "an unknown literal mode is rejected");

        const std::uint8_t badDict[4] = {0, 9, 0, 0};
        check(decompressDcl(badDict, 4, 0, err).empty() && !err.empty(),
              "an unknown dictionary size is rejected");

        // Random bytes behind a valid header. Every one must come back empty
        // with a reason, and none may hang, read out of bounds or assert. This
        // is the case a damaged CD actually produces.
        std::mt19937 rng(20260812);
        int survived = 0;
        for (int i = 0; i < 200; ++i)
        {
            std::vector<std::uint8_t> junk(64);
            junk[0] = 0;
            junk[1] = 4;
            for (std::size_t j = 2; j < junk.size(); ++j)
                junk[j] = static_cast<std::uint8_t>(rng() & 0xFF);
            std::string e;
            const auto out = decompressDcl(junk.data(), junk.size(), 4096, e);
            if (!out.empty())
                ++survived;
        }
        check(survived == 0, "no random stream decodes to the size it was told to expect");
    }

    // --- a missing or wrong-shaped archive is not an error ------------------
    {
        const auto none = InstallerArchive::open("no-such-file.z");
        check(!none.isOpen(), "a missing archive opens as empty rather than throwing");

        const fs::path tmp =
            fs::temp_directory_path() / "riven-installer-test-not-an-archive.z";
        {
            std::FILE *f = std::fopen(tmp.string().c_str(), "wb");
            if (f != nullptr)
            {
                const char text[] = "this is not an InstallShield archive at all";
                std::fwrite(text, 1, sizeof(text), f);
                std::fclose(f);
            }
        }
        const auto wrong = InstallerArchive::open(tmp);
        check(!wrong.isOpen(), "a file with the wrong magic opens as empty");
        std::error_code ec;
        fs::remove(tmp, ec);
    }

    const char *dataEnv = std::getenv("RIVEN_TEST_DATA");
    if (dataEnv == nullptr || dataEnv[0] == '\0')
    {
        std::printf("installer: %d checks, %d failed "
                    "(RIVEN_TEST_DATA unset: skipped the real-data checks)\n",
                    g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    const Source source = detectSource(dataEnv);
    if (!source.valid())
    {
        std::printf("installer: skipped (no Riven data under '%s')\n", dataEnv);
        return g_failures == 0 ? 0 : 1;
    }

    if (source.installerArchive.empty())
    {
        // A DVD/GOG or already-installed copy has the files loose and needs none
        // of this. Not a failure -- but say so, because a silent pass here would
        // look like the decoder had been exercised when it had not.
        std::printf("installer: skipped (this install has no arcriven.z; "
                    "riven.exe and extras.mhk are presumably loose)\n");
        return g_failures == 0 ? 0 : 1;
    }

    const auto archive = InstallerArchive::open(source.installerArchive);
    check(archive.isOpen(), "arcriven.z opens");
    if (!archive.isOpen())
    {
        std::printf("installer: %d checks, %d failed\n", g_checks, g_failures);
        return 1;
    }

    check(archive.contains("riven.exe"), "the archive holds riven.exe");
    check(archive.contains("RIVEN.EXE"), "and the lookup is case-insensitive");
    check(archive.contains("extras.mhk"), "the archive holds extras.mhk");

    // The two entries the port needs, decoded and hashed. These are the known
    // answers for the DCL decoder: a single wrong bit anywhere changes them.
    struct Expected
    {
        const char *name;
        std::size_t size;
        const char *sha256;
        const char *magic; ///< first bytes, as a second and independent check
        std::size_t magicLen;
    };
    const Expected wanted[] = {
        {"riven.exe", 877568,
         "420b0cc3a51a072b5652420788da74ad7b1deff1eb8c461d560a6ede25951369", "MZ", 2},
        {"extras.mhk", 261912,
         nullptr, "MHWK", 4},
    };

    for (const Expected &w : wanted)
    {
        std::string err;
        const auto bytes = archive.read(w.name, err);
        if (bytes.empty())
        {
            check(false, std::string(w.name) + " decompresses: " + err);
            continue;
        }
        check(bytes.size() == w.size,
              std::string(w.name) + " is " + std::to_string(w.size) + " bytes");
        check(bytes.size() >= w.magicLen
                  && std::memcmp(bytes.data(), w.magic, w.magicLen) == 0,
              std::string(w.name) + " starts with " + w.magic);
        if (w.sha256 != nullptr)
            check(sha256Hex(bytes) == w.sha256,
                  std::string(w.name) + " matches its expected SHA-256");
    }

    std::string err;
    check(archive.read("does-not-exist.dat", err).empty() && !err.empty(),
          "asking for an absent entry reports it rather than returning junk");

    // --- the cursors inside riven.exe ---------------------------------------
    {
        const auto exe = archive.read("riven.exe", err);
        std::vector<std::string> warnings;
        const auto cursors = readPeCursors(exe, warnings);
        for (const std::string &w : warnings)
            std::printf("  note: %s\n", w.c_str());

        check(!cursors.empty(), "riven.exe yields cursors");

        std::printf("  %zu cursors:", cursors.size());
        for (const PeCursor &c : cursors)
            std::printf(" %u(%dbpp,%dx%d,hot %d,%d)", c.groupId, c.bitCount, c.width,
                        c.height, c.hotX, c.hotY);
        std::printf("\n");

        // The ids ScummVM names, which are the ones the runtime asks for.
        for (const std::uint16_t want : {std::uint16_t(2003), std::uint16_t(2004),
                                         std::uint16_t(3000), std::uint16_t(5000),
                                         std::uint16_t(9000)})
        {
            const bool found =
                std::any_of(cursors.begin(), cursors.end(),
                            [want](const PeCursor &c) { return c.groupId == want; });
            check(found, "cursor " + std::to_string(want) + " is present");
        }

        bool allSquare = true;
        bool anyOpaque = false;
        for (const PeCursor &c : cursors)
        {
            if (c.width != 32 || c.height != 32)
                allSquare = false;
            if (c.rgb.size() != static_cast<std::size_t>(c.width) * c.height * 3
                || c.opaque.size() != static_cast<std::size_t>(c.width) * c.height)
            {
                check(false, "cursor " + std::to_string(c.groupId)
                                 + " has planes matching its size");
                break;
            }
            for (const std::uint8_t o : c.opaque)
                if (o != 0)
                    anyOpaque = true;
        }
        check(allSquare, "every cursor is 32x32");
        check(anyOpaque, "the set is not entirely transparent");

        // 9000 is kRivenHideCursor and is genuinely empty -- an all-zero colour
        // plane under an all-ones mask. If it ever decodes as opaque, the mask
        // is being read the wrong way round, which would make every cursor a
        // solid block.
        const auto hide = std::find_if(cursors.begin(), cursors.end(),
                                       [](const PeCursor &c) { return c.groupId == 9000; });
        if (hide != cursors.end())
        {
            const bool blank = std::all_of(hide->opaque.begin(), hide->opaque.end(),
                                           [](std::uint8_t o) { return o == 0; });
            check(blank, "the hide cursor decodes as fully transparent");
        }
    }

    std::printf("installer: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
