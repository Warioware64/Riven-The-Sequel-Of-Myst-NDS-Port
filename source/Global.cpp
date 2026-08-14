#include "Global.hpp"

#include "DebugLog.hpp"
#include "RivenData.hpp"

Global global;

void Global::Init()
{
    irqEnable(IRQ_HBLANK);
    irqSet(IRQ_VBLANK, NEA_VBLFunc);
    irqSet(IRQ_HBLANK, NEA_HBLFunc);

    // ROM filesystem (UI font + menu backgrounds only). Harmless if it fails;
    // recorded for callers.
    hasNitroFS = nitroFSInit(nullptr);

    // SD / flashcard FAT access — every byte of game content comes from here.
    const bool fatOk = fatInitDefault();
    fatDevice = fatGetDefaultDrive();
    hasFat = fatOk && (fatDevice != nullptr);

    if (hasFat)
    {
        fatDeviceCPP = fatDevice;
        EnsureDataDirs();
    }

    // NEA is still initialised, but only for what is not the 3D engine: the
    // cooperative thread pool, the maxmod stream, NEA_WaitForVBL and the vblank
    // handler all key off ne_execution_mode being set. Nothing draws through it
    // any more -- the card view and the movies are bitmap backgrounds now (see
    // source/render/BgSurface.hpp for why).
    NEA_Init3D();
    swiWaitForVBlank();
    swiWaitForVBlank();

    // NEA_Init3D claims A, B, C and D for the texture pool and E for texture
    // palettes. Give all of it back: with nothing textured, every one of those
    // banks is wanted for something better.
    NEA_TextureSystemEnd();
    NEA_SetTexPaletteBank(static_cast<NEA_VRAMBankFlags>(0));

    // Background-task pool (NEAThread.h). Cooperative, single CPU: this buys
    // RESPONSIVENESS, never throughput — a task runs only while the main thread is
    // parked in NEA_WaitForVBL, so the frame keeps drawing and the audio keeps
    // streaming around a long SD read instead of the game freezing on it.
    //
    // ONE worker on purpose: it is what guarantees no two tasks are inside libfat
    // at the same time, and the main thread is already reading the card from
    // AudioStream::pump and the video pumps. 32 KB of stack because the header
    // asks for 16 KB or more once a task touches the filesystem.
    //
    // Completions are run by NEA_ThreadProcess() from the engine's frame tail,
    // NOT via NEA_UPDATE_TASKS on the vblank wait — see the ordering note there.
    NEA_ThreadSystemReset(1, 32 * 1024);

    NEA_Hw2DVRAMConfig hw2dCfg = {};
    // main_bg stays 0: NEA's background allocator only models 8 blocks of 16 KB
    // per engine and reserves the first for maps, so it cannot hand out the
    // 128 KB-aligned bitmap bases BgSurface needs -- and it cannot address bank D
    // at all. The main backgrounds are claimed by hand below instead.
    hw2dCfg.main_bg = static_cast<NEA_VRAMBankFlags>(0);
    // Bank E drives main-engine OBJ sprites (the touch cursor). NEA only allows
    // main OBJ in banks A/B/E; A/B are the texture pool, so E (free — C is sub bg,
    // F/G are texture palette) is the one usable choice. main_obj=D fails
    // NEA_Hw2DInit (D can't be MAIN_SPRITE) and takes the sub bg down with it.
    //
    hw2dCfg.main_obj = NEA_VRAM_E;
    hw2dCfg.sub_bg = NEA_VRAM_C;
    hw2dCfg.sub_obj = static_cast<NEA_VRAMBankFlags>(0);
    NEA_Hw2DInit(&hw2dCfg);

    // The bottom screen: video mode 5, so BG2 and BG3 can be extended-rotation
    // bitmaps. Three things here are load-bearing and each of them fails quietly:
    //
    //  * The mode must change with a READ-MODIFY-WRITE. videoSetMode() assigns
    //    the whole of DISPCNT and would wipe the sprite-enable and 1D-128 mapping
    //    bits the oamInit() inside NEA_Hw2DInit has just set.
    //  * The mode must be 5 BEFORE the first bgInit. libnds latches whether a
    //    layer is text from the video mode at bgInit time, so a bitmap layer set
    //    up while the mode is still 0 is silently treated as tiled.
    //  * lcdMainOnBottom() has to be called here. NEA_MainScreenSetOnBottom only
    //    sets a flag that ne_process_common() applies, and ne_process_common runs
    //    from NEA_Process, which this port no longer calls. Without this line the
    //    game draws on the top screen and the console on the touch screen.
    videoBgDisable(0); // MODE_0_3D left BG0 enabled for the 3D output
    REG_DISPCNT = (REG_DISPCNT & ~(7u | ENABLE_3D | DISPLAY_BG0_ACTIVE))
                  | DISPLAY_VIDEO_MODE(5);
    lcdMainOnBottom();

    // The three card buffers. Main background VRAM is one contiguous window from
    // 0x06000000, so these land on bitmap bases 0, 8 and 16 -- see BgSurface.hpp.
    // Bank C is not in that window; it belongs to the sub engine.
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    vramSetBankB(VRAM_B_MAIN_BG_0x06020000);
    vramSetBankD(VRAM_D_MAIN_BG_0x06040000);
    // Freed along with the texture system: nothing has a palette to hold now.
    vramSetBankF(VRAM_F_LCD);
    vramSetBankG(VRAM_G_LCD);

    // The backdrop, which is what the letterbox bands above and below the
    // 256x165 card view show: both backgrounds are transparent there.
    BG_PALETTE[0] = 0;

    // NEA_Hw2DInit leaves the sub engine in MODE_0_2D (all four layers tiled),
    // and NEA only auto-upgrades the mode for bitmap BGs on the *main* engine.
    // The top screen needs a bitmap layer for the inventory strip and status
    // text, so switch to mode 5: BG0/BG1 stay tiled, BG2/BG3 become
    // extended-rotation bitmaps.
    videoSetModeSub(MODE_5_2D);

    // Bank C is whatever was in it at power-on, and the top screen is pointed at
    // it, so without this the player's first sight of the game is a screenful of
    // noise. Cleared to zero with the backdrop black, which reads the same whether
    // the layer showing it ends up paletted or direct-colour.
    dmaFillWords(0, VRAM_C, 128 * 1024);
    BG_PALETTE_SUB[0] = 0;

    // The top screen's console. Every diagnostic in this port reaches it through
    // DebugLog, which decides -- in one function -- which of them a player with
    // debug mode off actually sees: startup notices and a transient status line,
    // and nothing else, because the rest of the screen is Riven's own splash and
    // console glyphs are opaque white on top of it. See DebugLog.hpp.
    //
    // Standing it up here rather than in DebugLog costs one call and means the
    // console exists before anything can fail, which is what lets the "cannot
    // start" screens in main() have somewhere to print.
    //
    // Mode 5 keeps BG0 and BG1 tiled (only BG2/BG3 became bitmaps above), so the
    // console takes BG0. Map base 31 puts the map at the top of the 128 KB bank,
    // well clear of the 3 KB of font at tile base 0.
    //
    // That gap is not spare room, it is the debug console's: BG1 is the other
    // tiled layer and the libnds keyboard goes there, 27 KB of tiles at tile
    // base 1 and its map at base 28. See the VRAM table in
    // engine/DebugConsole.cpp -- these three numbers and TopBg's bitmap base are
    // the whole of what claims VRAM_C, and they are chosen together.
    console = consoleInit(nullptr, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false,
                          true);
}

void Global::EnsureDataDirs()
{
    if (!hasFat)
    {
        return;
    }
    // Only the directories the GAME writes to. Everything else is the converter's
    // to create, and creating them here would mask a missing conversion as an
    // empty one. create_directories makes every missing parent in one call; the
    // error_code overload keeps a full or write-protected card from throwing.
    std::error_code ec;
    std::filesystem::create_directories(savesDir(), ec);
    std::filesystem::create_directories(notesDir(), ec);
}

Global::DataStatus Global::CheckData() const
{
    if (!hasFat)
        return DataStatus::NoCard;

    std::error_code ec;
    if (!std::filesystem::exists(dataDir(), ec))
        return DataStatus::NoDataDir;

    if (!std::filesystem::exists(stacksDir(), ec))
        return DataStatus::NoStacks;

    // aspit is the stack the game boots into: the main menu, Atrus's book and
    // every linking book live there. Without it there is nothing to start.
    const std::string aspit =
        stacksDir() + rivendata::stackFileName(rivendata::StackId::Aspit);
    if (!std::filesystem::exists(aspit, ec))
        return DataStatus::NoAspit;

    // A graph without art means the conversion was interrupted. Probing the
    // directory rather than a specific id keeps this from breaking when the
    // converter's naming changes.
    if (!std::filesystem::exists(picsDir() + "aspit", ec))
        return DataStatus::NoImages;

    return DataStatus::Ok;
}

void Global::ReportOptionalData() const
{
    if (!hasFat)
        return;

    std::error_code ec;
    if (!std::filesystem::exists(videoDir(), ec))
    {
        // The single most likely explanation for "the game shows no video", and
        // nothing downstream can tell it from a card that simply has no movie on
        // it: activateMlst only ever learns about one missing file at a time.
        rivenrt::DebugLog::notice("no video/ on the card: converted with --no-video?");
    }
    if (!std::filesystem::exists(cursorsDir(), ec))
        rivenrt::DebugLog::notice("no cursors/ on the card: plain pointer");
}

const char *Global::DataStatusTitle(DataStatus s)
{
    switch (s)
    {
    case DataStatus::NoCard:    return "No SD card found";
    case DataStatus::NoDataDir: return "Cannot write to the card";
    case DataStatus::NoStacks:  return "No Riven data on the card";
    case DataStatus::NoAspit:   return "Riven data is incomplete";
    case DataStatus::NoImages:  return "Riven data has no images";
    default:                    return "";
    }
}

const char *Global::DataStatusHint(DataStatus s)
{
    switch (s)
    {
    case DataStatus::NoCard:
        return "Insert a card the console can read.";
    case DataStatus::NoDataDir:
        return "It may be write-protected or full.";
    case DataStatus::NoStacks:
        return "Run the converter: _nds/riven_nds/data";
    case DataStatus::NoAspit:
        return "The aspit stack is missing. Convert again.";
    case DataStatus::NoImages:
        return "Convert again with images enabled.";
    default:
        return "";
    }
}
