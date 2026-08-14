#include "DebugLog.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "Global.hpp"
#include "RivenImage.hpp"
#include "Settings.hpp"
#include "global_header.hpp"
#include "render/BgSurface.hpp"
#include "render/TopBg.hpp"
#include "tonccpy.h"

namespace rivenrt
{
namespace DebugLog
{
namespace
{
    bool g_on = false;
    std::FILE *g_file = nullptr;
    int g_shotCounter = 0;
    int g_dumpCounter = 0;

    /// One line's worth. The console is 32 columns, so anything past this is
    /// already wrapping; the file gets the same text so both agree.
    constexpr int kLineMax = 256;

    std::string logPath() { return global.dataDir() + "debug.log"; }

    /// The banks this port maps, and where the CPU sees them (Global::Init).
    ///
    /// None of them needs the remap-to-LCD dance the Myst port's dump does.
    /// That is required only for TEXTURE banks, which are not in a CPU-readable
    /// window; this port ended the texture system and put the card view on
    /// bitmap backgrounds, so every bank below can simply be read. F and G were
    /// freed to LCD, which is CPU-readable too.
    struct Bank
    {
        char name;
        const void *base;
        std::size_t bytes;
        const char *what;
    };

    /// Addressed through the window each bank is MAPPED into, not through the
    /// VRAM_x LCD pointers -- those are only where a bank appears while it is
    /// mapped as LCD, which here is true of F and G alone. Main BG VRAM is one
    /// contiguous window from BG_GFX, which is why A, B and D are offsets into
    /// it (Global::Init, BgSurface.hpp).
    const Bank kBanks[] = {
        {'A', BG_GFX, 128 * 1024, "card buffer 0"},
        {'B', BG_GFX + 0x20000 / 2, 128 * 1024, "card buffer 1"},
        {'D', BG_GFX + 0x40000 / 2, 128 * 1024, "card buffer 2"},
        {'C', BG_GFX_SUB, 128 * 1024, "sub bg: console + picture"},
        {'E', SPRITE_GFX, 64 * 1024, "main obj: cursor, inventory"},
        {'F', VRAM_F, 16 * 1024, "LCD, unused"},
        {'G', VRAM_G, 16 * 1024, "LCD, unused"},
    };

    /// A name that does not exist yet, so a session's dumps do not overwrite the
    /// one before it.
    std::string freeName(const char *fmt, int &counter)
    {
        std::error_code ec;
        char name[32];
        std::string path;
        do
        {
            std::snprintf(name, sizeof(name), fmt, counter++);
            path = global.dataDir() + name;
        } while (std::filesystem::exists(path, ec));
        return name;
    }

    void put16(std::uint8_t *p, unsigned v)
    {
        p[0] = static_cast<std::uint8_t>(v & 0xFF);
        p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    }

    void put32(std::uint8_t *p, unsigned v)
    {
        for (int i = 0; i < 4; ++i)
            p[i] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF);
    }
} // namespace

void begin()
{
    g_on = settings.debugMode;
    if (!g_on)
        return;

    // No consoleInit: unlike Myst's, this port's top screen has been a console
    // since Global::Init. What could be in the way is the picture put BEHIND it,
    // which a full-rate trace is unreadable over -- TopBg::load skips itself
    // when this is on, and this covers the case of the picture already being up.
    topBg.setVisible(false);

    if (global.hasFat)
    {
        // "w", not "a": a trace is of ONE run. An appended log would put the run
        // being debugged under every run before it, and on a card that never
        // gets cleaned out that is the log growing without bound as well.
        g_file = std::fopen(logPath().c_str(), "w");
        if (g_file == nullptr)
            std::printf("debug: cannot write debug.log\n");
    }

    log("== Riven DS debug log ==");
    log("SELECT+L screenshot, SELECT+R vram dump");
}

void end()
{
    if (g_file != nullptr)
    {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_on = false;
}

bool enabled() { return g_on; }

namespace
{
    /// The three destinations. Everything printed here goes through this, so the
    /// console, the emulator and the file never disagree about a line.
    void emit(const char *line)
    {
        std::printf("%s\n", line);
        // Free under an emulator and inert on hardware, so not worth gating.
        nocashMessage(line);
        nocashMessage("\n");

        if (g_file != nullptr)
        {
            std::fprintf(g_file, "%s\n", line);
            // Per line. A DS is switched off rather than shut down, so a buffer
            // that has not reached the card is a log that ends before the thing
            // being debugged -- which is the only part anyone wanted.
            std::fflush(g_file);
        }
    }

    void emitv(const char *fmt, std::va_list ap)
    {
        char line[kLineMax];
        const int n = std::vsnprintf(line, sizeof(line), fmt, ap);
        if (n > 0)
            emit(line);
    }
} // namespace

void log(const char *fmt, ...)
{
    if (!g_on)
        return;

    std::va_list ap;
    va_start(ap, fmt);
    emitv(fmt, ap);
    va_end(ap);
}

void warn(const char *fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    emitv(fmt, ap); // g_file is null unless the trace is on, so this splits itself
    va_end(ap);
}

void note(const char *fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    emitv(fmt, ap);
    va_end(ap);
}

void screenshot()
{
    if (!global.hasFat || !bgs.exists())
        return;

    // The whole buffer, not the 192 rows on screen. Row 0 of the buffer is row
    // kViewOffsetY of the display and the rows past the card view hold the
    // transparent band a pan slides through, so a 192-row crop would be a
    // picture of neither one thing nor the other.
    constexpr int kW = BgSurface::kBufW;
    constexpr int kH = BgSurface::kBufH;
    const rivendata::Texel *const src = BgSurface::pixels(bgs.frontBuffer());

    // 24-bit BGR, bottom-up, no palette: the least a BMP can be and still open
    // anywhere. Rows are already a multiple of four bytes at this width.
    constexpr std::size_t kRow = static_cast<std::size_t>(kW) * 3;
    constexpr std::size_t kPixels = kRow * kH;
    std::vector<std::uint8_t> file(54 + kPixels);
    std::uint8_t *const h = file.data();
    h[0] = 'B';
    h[1] = 'M';
    put32(h + 2, static_cast<unsigned>(file.size()));
    put32(h + 10, 54);
    put32(h + 14, 40);
    put32(h + 18, kW);
    put32(h + 22, kH);
    put16(h + 26, 1);
    put16(h + 28, 24);
    put32(h + 34, static_cast<unsigned>(kPixels));

    for (int y = 0; y < kH; ++y)
    {
        const rivendata::Texel *in = src + static_cast<std::size_t>(y) * kW;
        std::uint8_t *out = h + 54 + static_cast<std::size_t>(kH - 1 - y) * kRow;
        for (int x = 0; x < kW; ++x, out += 3)
        {
            const rivendata::Texel t = in[x];
            // 5 bits back to 8 the way the hardware expands them.
            const int r = (t & 31), g = (t >> 5) & 31, b = (t >> 10) & 31;
            out[0] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
            out[1] = static_cast<std::uint8_t>((g << 3) | (g >> 2));
            out[2] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
        }
    }

    const std::string name = freeName("shot%03d.bmp", g_shotCounter);
    std::FILE *f = std::fopen((global.dataDir() + name).c_str(), "wb");
    const bool ok = f != nullptr && std::fwrite(file.data(), 1, file.size(), f) == file.size();
    if (f != nullptr)
        std::fclose(f);
    log("SHOT %s %s", name.c_str(), ok ? "ok" : "FAILED");
}

void vramDump()
{
    if (!global.hasFat)
        return;

    std::error_code ec;
    const std::string dirname = freeName("vram%03d", g_dumpCounter);
    const std::string dir = global.dataDir() + dirname + "/";
    std::filesystem::create_directories(dir, ec);

    // One buffer, reused. The copy out of VRAM is a memcpy and the slow part is
    // the card write, which happens with nothing held.
    std::vector<std::uint8_t> ram(128 * 1024);
    for (const Bank &bk : kBanks)
    {
        tonccpy(ram.data(), bk.base, bk.bytes);

        char file[40];
        std::snprintf(file, sizeof(file), "bank%c.bin", bk.name);
        if (std::FILE *f = std::fopen((dir + file).c_str(), "wb"))
        {
            std::fwrite(ram.data(), 1, bk.bytes, f);
            std::fclose(f);
        }
        log("  bank%c %zu KB -- %s", bk.name, bk.bytes / 1024, bk.what);
    }

    log("VRAM -> %s", dirname.c_str());
}

} // namespace DebugLog
} // namespace rivenrt
