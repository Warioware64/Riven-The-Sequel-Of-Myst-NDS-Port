#include "Global.hpp"

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

    NEA_Init3D();
    NEA_MainScreenSetOnBottom();
    swiWaitForVBlank();
    swiWaitForVBlank();
    NEA_SetTexPaletteBank(static_cast<NEA_VRAMBankFlags>(NEA_VRAM_F | NEA_VRAM_G));
    NEA_TextureSystemReset(0, 0, static_cast<NEA_VRAMBankFlags>(NEA_VRAM_AB));

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
    hw2dCfg.main_bg = static_cast<NEA_VRAMBankFlags>(0);
    // Bank E drives main-engine OBJ sprites (the touch cursor). NEA only allows
    // main OBJ in banks A/B/E; A/B are the texture pool, so E (free — C is sub bg,
    // F/G are texture palette) is the one usable choice. main_obj=D fails
    // NEA_Hw2DInit (D can't be MAIN_SPRITE) and takes the sub bg down with it.
    //
    // D is deliberately left unclaimed here. The FULL-profile video decoder wants
    // D (+E) for its residual scratch, and it takes them only for the duration of
    // a fullscreen movie, when no card and no cursor are on screen.
    hw2dCfg.main_obj = NEA_VRAM_E;
    hw2dCfg.sub_bg = NEA_VRAM_C;
    hw2dCfg.sub_obj = static_cast<NEA_VRAMBankFlags>(0);
    NEA_Hw2DInit(&hw2dCfg);

    // NEA_Hw2DInit leaves the sub engine in MODE_0_2D (all four layers tiled),
    // and NEA only auto-upgrades the mode for bitmap BGs on the *main* engine.
    // The top screen needs a bitmap layer for the inventory strip and status
    // text, so switch to mode 5: BG0/BG1 stay tiled, BG2/BG3 become
    // extended-rotation bitmaps.
    videoSetModeSub(MODE_5_2D);

    // Black clear color for the main 3D engine — the letterbox bands above and
    // below the 256x165 card view on the 256x192 bottom screen.
    NEA_ClearColorSet(NEA_Black, 31, 63);
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
