#include "DebugLog.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

// fsync/fileno. See emit(): fflush alone does not reach the card.
#include <unistd.h>

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
    /// Startup notices emitted this run. Only settleNotices reads it, to decide
    /// whether anything is worth pausing for.
    int g_notices = 0;
    /// Frames the status line still has to live.
    int g_statusFrames = 0;
    /// The buttons that were down last time pollHotkeys() looked, so it can see
    /// a press without borrowing libnds's scanKeys bookkeeping. See there.
    std::uint32_t g_lastKeys = 0;
    /// How many HotkeyHold guards are alive. Non-zero means a screen that needs
    /// L and R for itself is up.
    int g_hotkeyHold = 0;

    /// The ten buttons REG_KEYINPUT carries, active LOW.
    constexpr std::uint32_t kKeyMask = 0x3FF;
    std::uint32_t keysNow()
    {
        return static_cast<std::uint32_t>(~REG_KEYINPUT) & kKeyMask;
    }

    /// One line's worth. The console is 32 columns, so anything past this is
    /// already wrapping; the file gets the same text so both agree.
    constexpr int kLineMax = 256;

    /// The console Global::Init set up: BgSize_T_256x256 with an 8x8 font.
    constexpr int kConsoleW = 32;
    constexpr int kConsoleH = 24;
    /// The status line goes on the LAST row, out of the way of a trace that
    /// scrolls from the top.
    constexpr int kStatusRow = kConsoleH - 1;

    /// About two seconds at 59.83 Hz. Long enough to read four words while
    /// still playing, short enough not to sit on the splash.
    constexpr int kStatusHoldFrames = 120;
    /// The same, for the boot notices -- but only paid when there are any.
    constexpr int kNoticeFrames = 120;

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
    log("L screenshot, R vram dump -- any time, cutscenes included");
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
    /// The three destinations, and the ONE place it is decided which of them a
    /// line reaches. Everything printed by this file goes through here, so the
    /// console, the emulator and the log never disagree about a line.
    ///
    /// `toConsole` is the whole of the policy in the header: false for the trace
    /// with debug mode off, true for a notice. The other two destinations are
    /// never gated -- nocashMessage is inert on hardware and free under an
    /// emulator, and g_file only exists when the trace is on -- which is why
    /// silencing the console loses nothing a developer was relying on.
    void emit(const char *line, bool toConsole)
    {
        if (toConsole)
            std::printf("%s\n", line);
        nocashMessage(line);
        nocashMessage("\n");

        if (g_file != nullptr)
        {
            std::fprintf(g_file, "%s\n", line);
            // Per line. A DS is switched off rather than shut down, so a buffer
            // that has not reached the card is a log that ends before the thing
            // being debugged -- which is the only part anyone wanted.
            //
            // AND fsync, which is the whole reason this used to produce an
            // empty file. fflush only pushes stdio's buffer into the filesystem
            // layer; BlocksDS says so in as many words -- "fflush() doesn't
            // currently guarantee a flush to the disk, so fsync(fileno(fp)) can
            // be used instead" (BlocksDS changelog). Neither the data nor, more
            // to the point, the directory entry that carries the file's LENGTH
            // reaches the card until this. So every run that ended the way a DS
            // run ends -- the power switch -- left a debug.log of zero bytes,
            // and the one run that did not (quitting through the menu, where
            // end() calls fclose) was the only one that ever worked.
            std::fflush(g_file);
            fsync(fileno(g_file));
        }
    }

    void emitv(const char *fmt, std::va_list ap, bool toConsole)
    {
        char line[kLineMax];
        const int n = std::vsnprintf(line, sizeof(line), fmt, ap);
        if (n > 0)
            emit(line, toConsole);
    }
} // namespace

void log(const char *fmt, ...)
{
    if (!g_on)
        return;

    std::va_list ap;
    va_start(ap, fmt);
    emitv(fmt, ap, true);
    va_end(ap);
}

void warn(const char *fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    emitv(fmt, ap, g_on);
    va_end(ap);
}

void note(const char *fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    emitv(fmt, ap, g_on);
    va_end(ap);
}

void notice(const char *fmt, ...)
{
    ++g_notices;
    std::va_list ap;
    va_start(ap, fmt);
    emitv(fmt, ap, true);
    va_end(ap);
}

int noticeCount() { return g_notices; }

void settleNotices()
{
    // With the trace on the console IS the top screen, and there is no picture
    // behind it to protect -- clearing it would only throw away the boot lines.
    if (g_on || global.console == nullptr)
        return;

    // Only if there was something to read. The last notices come from inside
    // Engine::boot(), after the menu the earlier ones sat behind, so without a
    // dwell here a missing cursor set would flash past in one frame. A complete
    // conversion reports nothing and pays none of this.
    if (g_notices > 0)
        for (int i = 0; i < kNoticeFrames; ++i)
            swiWaitForVBlank();

    consoleSelect(global.console);
    consoleSetWindow(global.console, 0, 0, kConsoleW, kConsoleH);
    consoleClear();
    g_statusFrames = 0;
}

void consoleRow(int row, const char *text)
{
    PrintConsole *const con = global.console;
    if (con == nullptr)
        return;

    // Everything consoleSetWindow is about to overwrite. The cursor is the part
    // that matters and the part that is easy to miss -- see the header.
    const int wx = con->windowX;
    const int wy = con->windowY;
    const int ww = con->windowWidth;
    const int wh = con->windowHeight;
    const int cx = con->cursorX;
    const int cy = con->cursorY;

    consoleSelect(con);
    consoleSetWindow(con, 0, row, kConsoleW, 1);
    consoleSetCursor(con, 0, 0);
    std::printf("%-*.*s", kConsoleW - 1, kConsoleW - 1, text != nullptr ? text : "");

    consoleSetWindow(con, wx, wy, ww, wh);
    consoleSetCursor(con, cx, cy);
}

void status(const char *text)
{
    if (global.console == nullptr)
        return;

    consoleRow(kStatusRow, text);
    g_statusFrames = (text != nullptr && text[0] != '\0') ? kStatusHoldFrames : 0;

    // The trace wants it in the scrollback too: on the top screen it is one row
    // that is about to be overwritten, and in debug.log it is the only record
    // that the player did this at all.
    if (g_on && text != nullptr && text[0] != '\0')
        emit(text, false);
}

void pumpStatus()
{
    if (g_statusFrames == 0)
        return;
    if (--g_statusFrames == 0)
        status("");
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

HotkeyHold::HotkeyHold() { ++g_hotkeyHold; }

HotkeyHold::~HotkeyHold()
{
    if (--g_hotkeyHold < 0)
        g_hotkeyHold = 0;
}

void pollHotkeys()
{
    // keysNow() AND OUR OWN EDGE, not scanKeys()/keysDown().
    //
    // This is called from every loop the engine has -- the card loop, the idle
    // loop a blocking movie spins, the interactive loop a drag spins -- and
    // those disagree about who calls scanKeys(). Some call it (frame,
    // playMovieBlocking), some never do (the effect waits). scanKeys() twice in
    // one frame is not harmless: the second call computes the edge against the
    // held mask the FIRST one stored, so it reports nothing pressed and eats
    // the caller's own B/START. Reading the register and keeping a private
    // `last` costs two instructions and cannot interfere with anything.
    const std::uint32_t now = keysNow();
    const std::uint32_t pressed = now & ~g_lastKeys;
    // ALWAYS, even on the paths that do nothing below. The state has to keep
    // following the buttons while the hotkeys are off, or a press made inside
    // the notebook would be waiting as a fresh edge the moment it closes.
    g_lastKeys = now;

    if (!g_on || g_hotkeyHold > 0)
        return;

    // BARE L AND R, and only in debug mode. They are the notebook's buttons the
    // rest of the time, and taking them is the point: the chords they replace
    // (SELECT+L, SELECT+R) needed three fingers on a machine being held in two
    // hands, which is exactly the wrong thing to ask for at the moment worth
    // photographing. Debug mode is where that trade is the right way round.
    //
    // Both of these stall for as long as an SD write takes -- a fifth of a
    // second for the shot, a second or more for the dump -- so a movie's
    // soundtrack will skip when one is taken during a cutscene. That is the
    // price of being able to take one AT ALL during a cutscene, which is where
    // the interesting frames are.
    if ((pressed & KEY_L) != 0)
        screenshot();
    if ((pressed & KEY_R) != 0)
        vramDump();
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
