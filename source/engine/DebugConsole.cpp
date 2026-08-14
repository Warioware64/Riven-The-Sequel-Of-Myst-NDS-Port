// SELECT+START: a command prompt, on the machine, with a keyboard.
//
// ScummVM's Riven console (console.cpp) on a DS. DebugLog answers "what did the
// game just do"; this answers "what does it do if I ask it THIS" -- go to that
// card, set that variable, list the hotspots I cannot see. On a PC that is a
// debugger and a console window. Here there is neither, and a bug that needs a
// particular card reached through forty minutes of play is a bug nobody
// investigates twice.
//
// Defined out of line as Engine members, exactly as the Myst port defines its
// DebugConsole.cpp, and for the reason its header gives: every command reaches
// stack_, card_, hotspotEnabled_, zipDests_ and idleFrame() directly, so NOT ONE
// ACCESSOR IS WIDENED for the sake of a debug tool.
//
// THE KEYBOARD, AND WHY THE OBVIOUS CALL IS WRONG.
//
// keyboardDemoInit() is unusable in this port. It is documented (keyboard.h) as
//
//     keyboardInit(NULL, 3, BgType_Text4bpp, BgSize_T_256x512, 20, 0, false, true)
//
// and every one of those last four arguments collides with something here. Sub
// layer 3 is TopBg's 8bpp bitmap; tile base 0 is the console font. Worse, the
// sub engine runs in MODE_5_2D (Global::Init), where layers 2 and 3 are
// extended-rotation and CANNOT be text -- libnds says so in background.c's
// bgInit validator, but that validator is inside #ifndef NDEBUG, so a release
// build does not assert. It programs BG3CNT as a rotation layer and the keyboard
// comes up as garbage with no diagnostic at all.
//
// So the keyboard is brought up by hand on BG1, the other layer mode 5 keeps
// tiled, with bases picked to fit around what is already in VRAM_C:
//
//     bytes         owner                            set by
//     0 - 3 K       console font (tile base 0)       Global::Init
//     3 K - 16 K    free
//     16 K - 43 K   keyboard tiles (tile base 1)     here, per session
//     43 K - 56 K   free
//     56 K - 60 K   keyboard map (map base 28)       here, per session
//     60 K - 62 K   free
//     62 K - 64 K   console map (map base 31)        Global::Init
//     64 K - 128 K  TopBg picture (bitmap base 4)    TopBg::load
//
// The keyboard's tile data is 27,520 bytes -- measured, not the "up to 44 KB"
// keyboard.h's comment claims -- and its map is two screen blocks for a
// 256x512 text background even though only 640 bytes of that are written.
// NOTHING IN Global::Init MOVES to make room, which is the point of choosing
// base 1 and base 28 over the 3/22 pairing the Myst port uses: Myst had to
// relocate its console because its keyboard sat at tile base 0, and here the
// console was already out of the way.
//
// PER SESSION, NOT PERSISTENT, for one reason that is not about space:
// keyboardInit unconditionally DMAs its sixteen palette entries over
// BG_PALETTE_SUB[0..15], and the console's own text colour is index 15. They
// are saved and put back around the session. TopBg's 240 entries are in that
// range too, but TopBg is not even loaded in debug mode, which is a second
// reason this is gated on the debug setting rather than merely useful there.
//
// THE SCREENS SWAP, because a keyboard has to be touchable. lcdMainOnTop() and
// lcdMainOnBottom(), NOT the NEA wrappers: Global.cpp:82-85 already records that
// NEA_MainScreenSetOnBottom only raises a flag ne_process_common() consumes, and
// this port never calls NEA_Process. The swap is one bit in REG_POWERCNT and
// moves nothing else -- the card buffers, the banks and BgSurface's flip are all
// untouched, and the digitizer is wired to the lower panel whatever is being
// displayed on it. So the card stays live on the top screen while it is typed
// at, which is the whole point: a command's effect is visible while the next one
// is written.
//
// THE FRAME. idleFrame() and not frame(): it pumps audio, movies and water but
// deliberately does NOT call processInput, so a stylus press meant for a key
// cannot also land on a hotspot on the card underneath.
//
// NO EXCEPTIONS. The ARM9 is built -fno-exceptions (data/StackFile.cpp:59-62),
// so every argument here is parsed with strtol and a full-consumption check --
// never a throwing conversion.

#include "Engine.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "BookNotes.hpp"
#include "DebugLog.hpp"
#include "Global.hpp"
#include "MainMenu.hpp"
#include "SaveGame.hpp"
#include "Settings.hpp"
#include "global_header.hpp"
#include "render/BgSurface.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    // The keyboard's own geometry decides the console's. Its lowerCase keymap is
    // 32x5 grid spaces of 8x16 pixels, so it is 256x80 and libnds rests it at
    // offset_y = -192 + 80 = -112: it covers screen rows 112..191, which are
    // text rows 14..23. Rows 0..11 scroll, row 12 is the prompt, row 13 is the
    // gutter that keeps the prompt off the keyboard's top edge.
    constexpr int kConsoleW = 32;
    constexpr int kConsoleH = 24;
    constexpr int kOutRows = 12;
    constexpr int kPromptRow = 12;

    constexpr int kKeyboardLayer = 1;
    constexpr int kKeyboardMapBase = 28;
    constexpr int kKeyboardTileBase = 1;

    constexpr std::size_t kMaxLine = 60;
    constexpr int kHistory = 8;

    /// One line of output, to the scrolling window AND to debug.log.
    ///
    /// Both, always. Twelve rows is not enough to read a `hotspots` dump on a
    /// card with fourteen of them, and the file is where it actually gets read.
    __attribute__((format(printf, 1, 2))) void out(const char *fmt, ...)
    {
        char buf[128];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);

        // NO consoleSetWindow here. The scrolling window is set once, when the
        // console opens, and drawPrompt puts it back exactly as it found it --
        // because consoleSetWindow zeroes the cursor, so re-aiming it before
        // every line wrote all of them on top of each other in row 0.
        //
        // And no printf either: this used to print AND call note(), which prints
        // again through the same console, so every line appeared twice.
        //
        // note() and not log(): these lines were asked for by hand, so they are
        // worth the file whether or not the trace is switched on. (The console
        // only opens in debug mode, so the gating on note() is moot here.)
        if (global.console != nullptr)
            consoleSelect(global.console);
        DebugLog::note("%s", buf);
    }

    /// Repaint the input line, in a window of its OWN one row.
    ///
    /// Its own window because printing into the scrolling window would scroll
    /// the output up by a line on every keystroke. And the last column is left
    /// unwritten on purpose: filling the final cell of a row wraps the console,
    /// and in a one-row window a wrap scrolls the line that was just drawn
    /// straight off it.
    void drawPrompt(const std::string &line, std::string &shown)
    {
        if (global.console == nullptr || line == shown)
            return;
        shown = line;

        // Tail-scroll: the end of the line is the part being typed.
        const std::size_t room = kConsoleW - 3;
        const char *tail = line.c_str();
        if (line.size() > room)
            tail += line.size() - room;

        // Through DebugLog::consoleRow, which saves and restores the scrolling
        // window AND its cursor. Doing this by hand is what broke out(): the
        // widen-again call at the end zeroes the cursor, so the next line of
        // command output landed back at the top of the screen.
        std::string row = "> ";
        row += tail;
        DebugLog::consoleRow(kPromptRow, row.c_str());
    }

    /// strtol with base 0, so 0x1F works, plus a FULL-CONSUMPTION check -- so
    /// `card banana` complains instead of teleporting to card 0.
    bool parseNum(const std::string &s, long &v)
    {
        if (s.empty())
            return false;
        char *end = nullptr;
        v = std::strtol(s.c_str(), &end, 0);
        return end != nullptr && *end == '\0';
    }

    std::vector<std::string> tokenize(const std::string &line)
    {
        std::vector<std::string> out;
        std::size_t i = 0;
        while (i < line.size())
        {
            while (i < line.size() && line[i] == ' ')
                ++i;
            const std::size_t start = i;
            while (i < line.size() && line[i] != ' ')
                ++i;
            if (i > start)
                out.push_back(line.substr(start, i - start));
        }
        return out;
    }

    struct HelpLine
    {
        const char *name;
        const char *usage;
    };

    /// The command table. kCommands is what TAB completes against, so the two
    /// lists are kept parallel rather than one being derived from the other --
    /// a command with no help line is a command nobody can discover.
    const HelpLine kHelp[] = {
        {"help", "help [cmd]"},
        {"where", "where - stack, card, picture, mode"},
        {"card", "card [id] - show or go to"},
        {"stack", "stack <name|1-8> [card]"},
        {"var", "var <name> [value]"},
        {"vars", "vars - every non-zero variable"},
        {"hotspots", "hotspots - every hotspot on this card"},
        {"zips", "zips - zip destinations visited"},
        {"zip", "zip - toggle zip mode"},
        {"sound", "sound <twav> [vol] - play an effect"},
        {"stopsound", "stopsound - silence everything"},
        {"movie", "movie <mlst code> - play"},
        {"refresh", "refresh - redraw the card"},
        {"save", "save <1-5>"},
        {"load", "load <1-5>"},
        {"notes", "notes - open the notebook"},
        {"shot", "shot - screenshot to the card"},
        {"vram", "vram - dump the VRAM banks"},
        {"resume", "resume - close the console"},
    };
    constexpr int kHelpCount = static_cast<int>(sizeof(kHelp) / sizeof(kHelp[0]));
} // namespace

bool Engine::runConsoleCommand(const std::string &line)
{
    const std::vector<std::string> tok = tokenize(line);
    if (tok.empty())
        return false;
    const std::string &cmd = tok[0];
    const std::size_t argc = tok.size() - 1;
    long n = 0;

    if (cmd == "help")
    {
        if (argc >= 1)
        {
            for (const HelpLine &h : kHelp)
                if (tok[1] == h.name)
                {
                    out("%s", h.usage);
                    return false;
                }
            out("no such command: %s", tok[1].c_str());
            return false;
        }
        // Packed several to a row: twelve rows of one command each would not fit
        // and the list is what tells anyone what is here.
        std::string row;
        for (int i = 0; i < kHelpCount; ++i)
        {
            row += kHelp[i].name;
            row += ' ';
            if (row.size() > 24 || i == kHelpCount - 1)
            {
                out("%s", row.c_str());
                row.clear();
            }
        }
        return false;
    }

    if (cmd == "where")
    {
        out("%s/%u global %lu", stackName(stack_.id), cardId_,
            static_cast<unsigned long>(globalCardId(cardId_)));
        out("pic %u  hs %zu  mode %s", cardPicture_,
            card_ != nullptr ? card_->hotspots.size() : 0u,
            mode_ == Mode::Zoom ? "zoom" : "card");
        out("ambient %ld  vars %zu  zips %zu", static_cast<long>(mainAmbientId_),
            vars_.size(), zipDests_.size());
        return false;
    }

    if (cmd == "card")
    {
        if (argc == 0)
        {
            out("%s/%u", stackName(stack_.id), cardId_);
            return false;
        }
        if (!parseNum(tok[1], n))
        {
            out("not a number: %s", tok[1].c_str());
            return false;
        }
        if (stack_.findCard(static_cast<std::uint16_t>(n)) == nullptr)
        {
            out("%s has no card %ld", stackName(stack_.id), n);
            return false;
        }
        // Through changeToCard, so the card being left still runs its leave
        // script -- a teleport that skipped it would leave the engine in a state
        // no route through the game can produce, and debugging that is worse
        // than not having the command.
        changeToCard(static_cast<std::uint16_t>(n));
        out("-> %s/%ld", stackName(stack_.id), n);
        return false;
    }

    if (cmd == "stack")
    {
        if (argc == 0)
        {
            out("%s (%d)", stackName(stack_.id), static_cast<int>(stack_.id));
            return false;
        }
        StackId target = parseStackName(tok[1]);
        if (target == StackId::None && parseNum(tok[1], n) && n > 0
            && n <= static_cast<long>(StackId::Aspit))
            target = static_cast<StackId>(n);
        if (target == StackId::None)
        {
            out("no stack called %s", tok[1].c_str());
            return false;
        }
        // The contract at Engine.hpp:76-85. The console runs from processInput,
        // outside the interpreter, so this should never fire -- but a refusal
        // that says so beats a deferral that happens after the console is gone.
        if (scriptDepth_ > 0)
        {
            out("a script is running; not now");
            return false;
        }
        if (!changeToStack(target))
        {
            out("%s would not load", stackName(target));
            return false;
        }
        std::uint16_t card = stack_.cards.empty() ? 1 : stack_.cards.front().id;
        if (argc >= 2 && parseNum(tok[2], n))
            card = static_cast<std::uint16_t>(n);
        if (!changeToCard(card))
            out("%s has no card %u", stackName(target), card);
        else
            out("-> %s/%u", stackName(target), card);
        return false;
    }

    if (cmd == "var")
    {
        if (argc == 0)
        {
            out("var <name> [value]");
            return false;
        }
        // BY NAME, which is the one place this is nicer than the Myst port's
        // console: RivenVars.hpp already ships both directions of the mapping,
        // so nothing had to be built for it and nobody has to look up a number.
        const VarId id = parseVarName(Vars::normalise(tok[1]));
        if (id == VarId::Unknown)
        {
            out("no variable called %s", tok[1].c_str());
            return false;
        }
        if (argc >= 2)
        {
            if (!parseNum(tok[2], n))
            {
                out("not a number: %s", tok[2].c_str());
                return false;
            }
            vars_.set(id, static_cast<std::uint32_t>(n));
        }
        // Read BACK rather than echoing what was written: if anything ever
        // routes a variable away from the flat map, a silent no-op has to be
        // visible here rather than look like a success.
        out("%s = %lu", varName(id), static_cast<unsigned long>(vars_.get(id)));
        return false;
    }

    if (cmd == "vars")
    {
        // Walked over the ENUM and not the map, so the order is stable between
        // runs and nothing has to widen Vars to iterate it.
        std::string row;
        int shown = 0;
        for (int i = 1; i < kVarCount; ++i)
        {
            const VarId id = static_cast<VarId>(i);
            const std::uint32_t v = vars_.get(id);
            if (v == 0)
                continue;
            char cell[32];
            std::snprintf(cell, sizeof(cell), "%s=%lu ", varName(id),
                          static_cast<unsigned long>(v));
            row += cell;
            ++shown;
            if (row.size() > 24)
            {
                out("%s", row.c_str());
                row.clear();
            }
        }
        if (!row.empty())
            out("%s", row.c_str());
        out("%d non-zero", shown);
        return false;
    }

    if (cmd == "hotspots")
    {
        if (card_ == nullptr)
        {
            out("no card");
            return false;
        }
        for (std::size_t i = 0; i < card_->hotspots.size(); ++i)
        {
            const Hotspot &h = card_->hotspots[i];
            const bool on = i < hotspotEnabled_.size() && hotspotEnabled_[i];
            // Card coordinates, because that is what the data holds; the hit
            // test converts the POINT rather than pre-scaling the rect.
            out("#%zu %s id%u (%d,%d %dx%d)%s", i, on ? "on " : "off", h.blstId,
                h.rect.left, h.rect.top, h.rect.right - h.rect.left,
                h.rect.bottom - h.rect.top, (h.flags & kHotspotZip) != 0 ? " zip" : "");
            const std::string name = nameFromList(kHotspotNames, h.nameRes);
            if (!name.empty())
                out("   %s cur%u", name.c_str(), h.cursor);
        }
        out("%zu hotspots", card_->hotspots.size());
        return false;
    }

    if (cmd == "zips")
    {
        for (const ZipDest &z : zipDests_)
            out("%s/%u %s", stackName(z.stack), z.cardId, z.name.c_str());
        out("%zu zip destinations", zipDests_.size());
        return false;
    }

    if (cmd == "zip")
    {
        settings.zipMode = !settings.zipMode;
        settings.save();
        // apply() is what writes VarId::AZip, which is where Riven's own scripts
        // read the setting from -- toggling the Settings field alone would
        // change nothing the game can see.
        settings.apply();
        out("zip mode %s", settings.zipMode ? "on" : "off");
        return false;
    }

    if (cmd == "sound")
    {
        if (argc == 0 || !parseNum(tok[1], n))
        {
            out("sound <twav> [vol]");
            return false;
        }
        long vol = 256;
        if (argc >= 2 && !parseNum(tok[2], vol))
            vol = 256;
        playEffect(static_cast<std::uint16_t>(n), static_cast<int>(vol));
        return false;
    }

    if (cmd == "stopsound")
    {
        stopEffects();
        stopAllAmbient();
        out("silent");
        return false;
    }

    if (cmd == "movie")
    {
        if (argc == 0 || !parseNum(tok[1], n))
        {
            out("movie <mlst code>");
            return false;
        }
        // Non-blocking: a blocking play would spin its own loop and take the
        // input away from the console that started it.
        playMovie(static_cast<std::uint16_t>(n), false);
        return false;
    }

    if (cmd == "refresh")
    {
        surface_.markAll();
        refreshCard();
        return false;
    }

    if (cmd == "save" || cmd == "load")
    {
        if (argc == 0 || !parseNum(tok[1], n) || !SaveGame::validSlot(static_cast<int>(n)))
        {
            out("%s <1-%d>", cmd.c_str(), SaveGame::kSlotCount);
            return false;
        }
        const int slot = static_cast<int>(n);
        if (cmd == "save")
        {
            out("slot %d %s", slot,
                SaveGame::writeSlot(slot, buildSaveState()) ? "written" : "FAILED");
            return false;
        }
        SaveGame::SaveState state;
        if (!SaveGame::readSlot(slot, state))
        {
            const SaveGame::SlotInfo info = SaveGame::readSlotInfo(slot);
            out("slot %d %s", slot,
                info.damaged ? "is damaged" : (info.used ? "would not read" : "is empty"));
            return false;
        }
        if (!restoreFrom(state))
        {
            out("slot %d could not be entered", slot);
            return false;
        }
        // The card underneath has been replaced, so there is nothing left for
        // the console to be looking at.
        return true;
    }

    if (cmd == "notes")
    {
        // The notebook draws on the MAIN engine, which the swap has put on the
        // top screen -- so the screens go back before it opens and are swapped
        // again after.
        lcdMainOnBottom();
        runNotebook();
        lcdMainOnTop();
        return false;
    }

    if (cmd == "shot")
    {
        DebugLog::screenshot();
        return false;
    }
    if (cmd == "vram")
    {
        DebugLog::vramDump();
        return false;
    }

    if (cmd == "resume" || cmd == "exit" || cmd == "quit")
        return true;

    out("no such command: %s", cmd.c_str());
    return false;
}

void Engine::runDebugConsole()
{
    PrintConsole *const con = global.console;
    if (con == nullptr || !DebugLog::enabled())
        return;

    // The pointer is a main-engine sprite, and the main engine is about to be on
    // the other screen. Scope-bound, so every path out of here unwinds it.
    CursorHide cursorGuard{*this};

    // The keyboard DMAs its palette over these (keyboard.c), and entry 15 is the
    // console's text colour -- so without putting them back, closing the console
    // would leave the top screen's log unreadable for the rest of the session.
    std::uint16_t savedPalette[16];
    for (int i = 0; i < 16; ++i)
        savedPalette[i] = BG_PALETTE_SUB[i];

    lcdMainOnTop();
    Keyboard *const kbd =
        keyboardInit(nullptr, kKeyboardLayer, BgType_Text4bpp, BgSize_T_256x512,
                     kKeyboardMapBase, kKeyboardTileBase, /*mainDisplay*/ false,
                     /*loadGraphics*/ true);
    if (kbd != nullptr)
        // No slide-in. This is a tool, not a transition.
        kbd->scrollSpeed = 0;
    keyboardShow();

    // The log's own scrollback is behind the keyboard now and cannot be read, so
    // it is cleared rather than left as half-covered noise under the prompt.
    consoleSelect(con);
    consoleSetWindow(con, 0, 0, kConsoleW, kConsoleH);
    consoleClear();
    consoleSetWindow(con, 0, 0, kConsoleW, kOutRows);
    out("-- console: help, B closes --");
    out("%s/%u", stackName(stack_.id), cardId_);

    std::string line;
    // An impossible value, so the first drawPrompt always paints.
    std::string shown = "\x01";
    std::string history[kHistory];
    int historyCount = 0;
    int historyPos = -1;
    bool done = false;

    while (!done)
    {
        drawPrompt(line, shown);
        idleFrame();
        scanKeys();
        if ((keysDown() & (KEY_B | KEY_SELECT)) != 0)
            break;

        const s16 c = keyboardUpdate();
        switch (c)
        {
        case NOKEY:
            break;

        case DVK_FOLD:
            done = true;
            break;

        case DVK_ENTER:
        {
            out("> %s", line.c_str());
            if (!line.empty())
            {
                // Newest first, and not repeated: holding a command down the
                // history to run it twice should not push everything else off.
                if (historyCount == 0 || history[0] != line)
                {
                    for (int i = kHistory - 1; i > 0; --i)
                        history[i] = history[i - 1];
                    history[0] = line;
                    if (historyCount < kHistory)
                        ++historyCount;
                }
                const std::string run = line;
                line.clear();
                historyPos = -1;
                if (runConsoleCommand(run))
                    done = true;
            }
            break;
        }

        case DVK_BACKSPACE:
            if (!line.empty())
                line.pop_back();
            break;

        case DVK_UP:
            if (historyPos + 1 < historyCount)
                line = history[++historyPos];
            break;

        case DVK_DOWN:
            if (historyPos > 0)
                line = history[--historyPos];
            else if (historyPos == 0)
            {
                historyPos = -1;
                line.clear();
            }
            break;

        case DVK_TAB:
        {
            // Complete the command NAME only. Arguments are card ids and
            // variable names; the ids cannot be completed and the variables are
            // 182 entries of mostly-shared prefixes, which would list more often
            // than it would finish anything.
            const std::vector<std::string> tok = tokenize(line);
            if (tok.size() != 1 || line.back() == ' ')
                break;
            const HelpLine *hit = nullptr;
            int matches = 0;
            for (const HelpLine &h : kHelp)
            {
                if (std::strncmp(h.name, tok[0].c_str(), tok[0].size()) == 0)
                {
                    hit = &h;
                    ++matches;
                }
            }
            if (matches == 1 && hit != nullptr)
            {
                line = hit->name;
                line += ' ';
            }
            else if (matches > 1)
            {
                std::string row;
                for (const HelpLine &h : kHelp)
                    if (std::strncmp(h.name, tok[0].c_str(), tok[0].size()) == 0)
                    {
                        row += h.name;
                        row += ' ';
                    }
                out("%s", row.c_str());
            }
            break;
        }

        default:
            if (c >= 32 && c < 127 && line.size() < kMaxLine)
                line.push_back(static_cast<char>(c));
            break;
        }
    }

    keyboardHide();
    keyboardExit();
    // keyboardExit only hides the background and clears a flag; the layer stays
    // enabled and would show its tiles over the log.
    videoBgDisableSub(kKeyboardLayer);
    for (int i = 0; i < 16; ++i)
        BG_PALETTE_SUB[i] = savedPalette[i];

    // Widen back to the whole screen, keeping the row the session's output
    // reached. consoleSetWindow zeroes the cursor, so without carrying it over
    // the trace would resume at the top of the screen and overwrite everything
    // the console had just been used to find out.
    consoleSelect(con);
    const int reachedY = con->cursorY;
    consoleSetWindow(con, 0, 0, kConsoleW, kConsoleH);
    consoleSetCursor(con, 0, reachedY < kConsoleH ? reachedY : kConsoleH - 1);

    // The layer goes down before the screens go back, so no frame shows a
    // half-torn-down keyboard on the touch panel.
    lcdMainOnBottom();
    DebugLog::note("-- resume %s/%u --", stackName(stack_.id), cardId_);
}

} // namespace rivenrt
