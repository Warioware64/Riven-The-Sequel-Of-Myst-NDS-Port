#include "Vars.hpp"

#include <cstdlib>

namespace rivenrt
{
namespace
{
    /// ScummVM's own comment on this is worth keeping: the dome combination is
    /// deliberately NOT uniformly random. The first slider position is in [2, 10],
    /// the second and third in [11, 15], and the fourth and fifth in [16, 24];
    /// each bit of the word is one slider position, with (1 << 24) meaning 1.
    /// riven_vars.cpp:357-366.
    int rng(int low, int high)
    {
        return low + static_cast<int>(std::rand() % (high - low + 1));
    }

    std::uint32_t twoRandomDomePositions(int low, int high)
    {
        const int first = rng(low, high);
        int second = rng(low, high);
        while (second == first)
            second = rng(low, high);
        return (1u << (25 - first)) | (1u << (25 - second));
    }

    using rivendata::VarId;

    /// The non-zero defaults, verbatim from riven_vars.cpp:300-338. Anything not
    /// here starts at zero.
    struct Default
    {
        VarId id;
        std::uint32_t value;
    };

    constexpr Default kDefaults[] = {
        {VarId::TTelescope, 5},   {VarId::TGateState, 1},
        {VarId::JBridge1, 1},     {VarId::JBridge4, 1},
        {VarId::JGallows, 1},     {VarId::JIconCorrectOrder, 12068577},
        {VarId::JCrg, 1},         {VarId::JWharkPos, 1},
        {VarId::BBlrValve, 1},    {VarId::BBlrWtr, 1},
        {VarId::BFans, 1},        {VarId::BYTrap, 2},
        {VarId::AAtrusBook, 1},   {VarId::ACathBook, 1},
        {VarId::BHeat, 1},        {VarId::BLabPage, 1},
        {VarId::BIdVlv, 1},       {VarId::BVise, 1},
        {VarId::WaterEnabled, 1}, {VarId::OGehnPage, 1},
        {VarId::BBlrSw, 1},       {VarId::OCage, 1},
        {VarId::JBeetle, 1},      {VarId::TDl, 1},
        {VarId::BMagCar, 1},      {VarId::GNMagCar, 1},
        {VarId::GEMagCar, 1},     {VarId::GImageCurr, 1},
        {VarId::GImageMax, 1},    {VarId::GImageRot, 1},
        {VarId::GLkBridge, 1},    {VarId::GRViewPos, 2},
        {VarId::GPinPos, 1},      {VarId::GRViewMPos, 1617},
        {VarId::OMusicPlayer, 1}, {VarId::TDomeElev, 1},
        // kRivenTransitionModeFastest -- 8 blend steps over 300 ms
        // (riven_graphics.cpp:505-508). This is the original's own setting for
        // machines that cannot afford the slower dissolves, and it is what
        // BgSurface's transition timings are derived from.
        {VarId::TransitionMode, 5001},
    };
} // namespace

std::string Vars::normalise(const std::string &name)
{
    std::string out;
    out.reserve(name.size());
    for (const char c : name)
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    return out;
}

void Vars::startNewGame()
{
    vars_.clear();

    for (const Default &d : kDefaults)
        vars_[d.id] = d.value;

    // The telescope combination: five buttons, five digits.
    std::uint32_t tele = 0;
    for (int i = 0; i < 5; ++i)
    {
        tele *= 10;
        tele += static_cast<std::uint32_t>(rng(1, 5));
    }
    vars_[VarId::TCorrectOrder] = tele;

    // The prison combination: three sounds, five digits.
    std::uint32_t prison = 0;
    for (int i = 0; i < 5; ++i)
    {
        prison *= 10;
        prison += static_cast<std::uint32_t>(rng(1, 3));
    }
    vars_[VarId::PCorrectOrder] = prison;

    std::uint32_t dome = 0;
    dome |= 1u << (25 - rng(2, 10));
    dome |= twoRandomDomePositions(11, 15);
    dome |= twoRandomDomePositions(16, 24);
    vars_[VarId::ADomeCombo] = dome;
}

} // namespace rivenrt
