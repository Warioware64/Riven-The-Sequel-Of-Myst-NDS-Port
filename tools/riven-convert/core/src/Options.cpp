#include "riven/Options.hpp"

namespace riven
{

const char *stageName(Stage s)
{
    switch (s)
    {
    case Stage::Cards:  return "cards";
    case Stage::Images: return "images";
    case Stage::Hires:  return "zoom art";
    case Stage::Water:  return "water effects";
    case Stage::Audio:  return "audio";
    case Stage::Video:  return "video";
    case Stage::Cursors: return "cursors";
    case Stage::Extras: return "inventory, marble and credits art";
    }
    return "?";
}

bool stageImplemented(Stage)
{
    // Every stage is built now. The hook stays because the UI asks the
    // question, and the next one to be added will want it again.
    return true;
}

bool Options::enabled(Stage s) const
{
    switch (s)
    {
    case Stage::Cards:  return cards;
    case Stage::Images: return images;
    case Stage::Hires:  return hires;
    case Stage::Water:  return water;
    case Stage::Audio:  return audio;
    case Stage::Video:  return video;
    case Stage::Cursors: return cursors;
    case Stage::Extras: return extras;
    }
    return false;
}

void Options::setEnabled(Stage s, bool on)
{
    switch (s)
    {
    case Stage::Cards:  cards = on; break;
    case Stage::Images: images = on; break;
    case Stage::Hires:  hires = on; break;
    case Stage::Water:  water = on; break;
    case Stage::Audio:  audio = on; break;
    case Stage::Video:  video = on; break;
    case Stage::Cursors: cursors = on; break;
    case Stage::Extras: extras = on; break;
    }
}

void Options::normalise()
{
    // The hi-res twins are an addition to the display stills, not a substitute:
    // pics_hi/ alone gives a game whose every card is missing. Asking for them
    // therefore implies the normal images too.
    if (hires)
        images = true;
}

namespace
{
    Options everything(Options o)
    {
        o.cards = o.images = o.hires = o.water = o.audio = o.video = true;
        o.cursors = o.extras = true;
        o.normalise();
        return o;
    }

    Options smaller(Options o)
    {
        o.cards = o.images = o.water = o.audio = o.video = true;
        o.cursors = o.extras = true;
        o.hires = false;
        o.normalise();
        return o;
    }

    Options cardsOnly(Options o)
    {
        o.cards = true;
        o.images = o.hires = o.water = o.audio = o.video = false;
        o.cursors = o.extras = false;
        o.normalise();
        return o;
    }

    Options custom(Options o) { return o; }
} // namespace

const std::vector<Preset> &presets()
{
    static const std::vector<Preset> kPresets = {
        {"Everything", "Cards, art, zoom art, water effects, sound and movies.",
         &everything},
        {"Smaller card",
         "Skips the full-resolution zoom art, which is the largest single thing "
         "on the card. Small print in puzzles becomes hard to read.",
         &smaller},
        {"Cards only",
         "Just the card graph and scripts. Fast, and enough to test navigation.",
         &cardsOnly},
        {"Custom", "Your own selection.", &custom},
    };
    return kPresets;
}

std::string matchingPresetName(const Options &o)
{
    for (const auto &p : presets())
    {
        if (std::string(p.name) == "Custom")
            continue;
        Options probe = p.apply(o);
        if (probe.cards == o.cards && probe.images == o.images && probe.hires == o.hires
            && probe.water == o.water && probe.audio == o.audio
            && probe.video == o.video && probe.cursors == o.cursors
            && probe.extras == o.extras)
            return p.name;
    }
    return "Custom";
}

} // namespace riven
