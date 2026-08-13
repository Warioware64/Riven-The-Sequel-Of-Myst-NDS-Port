#pragma once

// Riven's game variables, as a closed enum shared verbatim between the host
// converter (tools/riven-convert) and the ARM9 runtime.
//
// The scripts reference a variable by an INDEX into the stack's own NAME 4 list,
// so the same number means different variables in different stacks and the NAME
// string is the only stable identity. That used to make the runtime store a
// std::map<std::string, u32> and pay a string allocation, a case-fold and a run
// of string compares on every script variable access.
//
// It does not have to. The set of names is closed -- Riven has shipped since
// 1997 and no card can invent one -- so the strings can be resolved ONCE, on the
// host, at conversion time: the converter turns each stack's NAME 4 list into a
// std::vector<VarId> (RivenData.hpp: Stack::variableIds) and the runtime indexes
// that. On the device a variable is an integer and nothing else.
//
// THE LIST IS THE UNION OF TWO SOURCES, and both halves are load-bearing:
//
//   * 163 distinct case-folded names across the eight stacks' NAME 4 resources
//     (222 entries in total: aspit 2, bspit 48, gspit 29, jspit 52, ospit 17,
//     pspit 24, rspit 7, tspit 43).
//   * 18 names that appear in NO stack's NAME 4 list and are referenced only
//     from C++ -- ScummVM's initVars defaults (riven_vars.cpp:300-366), the two
//     "where am I" variables Engine::enterCard writes, this port's own
//     inventory plumbing, and "azip", which is not a game variable at all: it
//     is the player's Zip Mode setting, which ScummVM likewise keeps in the
//     variable map because the card scripts read it (riven.cpp:803). The
//     converter cannot discover these, which is why this file is hand-
//     maintained rather than generated.
//
// ORDER IS STABLE and the strings are the contract. Append new names at the end;
// never renumber, and never edit a string without bumping kSchemaVersion, because
// Stack::variableIds is serialized as raw integers.
//
// Depends only on the standard library -- no <nds.h>, no yas -- for the same
// reason RivenData.hpp does: the converter and the ROM compile it identically.

#include <cstddef>
#include <cstdint>
#include <string>

namespace rivendata
{

/// identifier, on-disc name case-folded to lower case.
#define RIVEN_VAR_LIST(X)                  \
    X(AAtrusBook,        "aatrusbook")     \
    X(ACathBook,         "acathbook")      \
    X(ACathState,        "acathstate")     \
    X(ADoIt,             "adoit")          \
    X(ADomeCombo,        "adomecombo")     \
    X(AGehn,             "agehn")          \
    X(AOva,              "aova")           \
    X(APower,            "apower")         \
    X(ARaw,              "araw")           \
    X(ATemp,             "atemp")          \
    X(ATrap,             "atrap")          \
    X(ATrapBook,         "atrapbook")      \
    X(BBackLock,         "bbacklock")      \
    X(BBait,             "bbait")          \
    X(BBigBridge,        "bbigbridge")     \
    X(BBirds,            "bbirds")         \
    X(BBlrArm,           "bblrarm")        \
    X(BBlrDoor,          "bblrdoor")       \
    X(BBlrGrt,           "bblrgrt")        \
    X(BBlrSw,            "bblrsw")         \
    X(BBlrValve,         "bblrvalve")      \
    X(BBlrWtr,           "bblrwtr")        \
    X(BBook,             "bbook")          \
    X(BBrLever,          "bbrlever")       \
    X(BCaveDoor,         "bcavedoor")      \
    X(BCPipeGr,          "bcpipegr")       \
    X(BCraterGg,         "bcratergg")      \
    X(BDome,             "bdome")          \
    X(BDrwr,             "bdrwr")          \
    X(BFans,             "bfans")          \
    X(BFmDoor,           "bfmdoor")        \
    X(BFrontLock,        "bfrontlock")     \
    X(BHeat,             "bheat")          \
    X(BIdVlv,            "bidvlv")         \
    X(BLab,              "blab")           \
    X(BLabBackDr,        "blabbackdr")     \
    X(BLabBook,          "blabbook")       \
    X(BLabEye,           "blabeye")        \
    X(BLabFrontDr,       "blabfrontdr")    \
    X(BLabPage,          "blabpage")       \
    X(BMagCar,           "bmagcar")        \
    X(BPipDr,            "bpipdr")         \
    X(BPrs,              "bprs")           \
    X(BStove,            "bstove")         \
    X(BTrap,             "btrap")          \
    X(BValve,            "bvalve")         \
    X(BVise,             "bvise")          \
    X(BYTram,            "bytram")         \
    X(BYTramTime,        "bytramtime")     \
    X(BYTrap,            "bytrap")         \
    X(BYTrapped,         "bytrapped")      \
    X(CurrentCardId,     "currentcardid")  \
    X(CurrentStackId,    "currentstackid") \
    X(DomeCheck,         "domecheck")      \
    X(ElevBtn1,          "elevbtn1")       \
    X(ElevBtn2,          "elevbtn2")       \
    X(ElevBtn3,          "elevbtn3")       \
    X(GBook,             "gbook")          \
    X(GCathState,        "gcathstate")     \
    X(GCathTime,         "gcathtime")      \
    X(GDome,             "gdome")          \
    X(GEMagCar,          "gemagcar")       \
    X(GImageCurr,        "gimagecurr")     \
    X(GImageMax,         "gimagemax")      \
    X(GImageRot,         "gimagerot")      \
    X(GLkBridge,         "glkbridge")      \
    X(GLkBtns,           "glkbtns")        \
    X(GLView,            "glview")         \
    X(GLViewPos,         "glviewpos")      \
    X(GNMagCar,          "gnmagcar")       \
    X(GNMagRot,          "gnmagrot")       \
    X(GPinPos,           "gpinpos")        \
    X(GPinUp,            "gpinup")         \
    X(GRView,            "grview")         \
    X(GRViewMPos,        "grviewmpos")     \
    X(GRViewPos,         "grviewpos")      \
    X(GScribe,           "gscribe")        \
    X(GSubDr,            "gsubdr")         \
    X(GSubElev,          "gsubelev")       \
    X(GWhark,            "gwhark")         \
    X(GWharkTime,        "gwharktime")     \
    X(JBeetle,           "jbeetle")        \
    X(JBeetlePool,       "jbeetlepool")    \
    X(JBook,             "jbook")          \
    X(JBridge1,          "jbridge1")       \
    X(JBridge2,          "jbridge2")       \
    X(JBridge3,          "jbridge3")       \
    X(JBridge4,          "jbridge4")       \
    X(JBridge5,          "jbridge5")       \
    X(JCcb,              "jccb")           \
    X(JCrg,              "jcrg")           \
    X(JDome,             "jdome")          \
    X(JDrain,            "jdrain")         \
    X(JGallows,          "jgallows")       \
    X(JGate,             "jgate")          \
    X(JGirl,             "jgirl")          \
    X(JIconCorrectOrder, "jiconcorrectorder") \
    X(JLadder,           "jladder")        \
    X(JLeftPos,          "jleftpos")       \
    X(JPeek,             "jpeek")          \
    X(JPlayBeetle,       "jplaybeetle")    \
    X(JPRebel,           "jprebel")        \
    X(JPrisonDr,         "jprisondr")      \
    X(JPrisonSecDr,      "jprisonsecdr")   \
    X(JRBook,            "jrbook")         \
    X(JRightPos,         "jrightpos")      \
    X(JSchoolDr,         "jschooldr")      \
    X(JSouthPathDr,      "jsouthpathdr")   \
    X(JSub,              "jsub")           \
    X(JSubDir,           "jsubdir")        \
    X(JSubHatch,         "jsubhatch")      \
    X(JSubSw,            "jsubsw")         \
    X(JSunners,          "jsunners")       \
    X(JSunnerTime,       "jsunnertime")    \
    X(JThroneDr,         "jthronedr")      \
    X(JTunnelDr,         "jtunneldr")      \
    X(JTunnelLamps,      "jtunnellamps")   \
    X(JVillagePeople,    "jvillagepeople") \
    X(JWarning,          "jwarning")       \
    X(JWharkPos,         "jwharkpos")      \
    X(JWharkRam,         "jwharkram")      \
    X(JWMagCar,          "jwmagcar")       \
    X(JWMouth,           "jwmouth")        \
    X(JYMagCar,          "jymagcar")       \
    X(OAmbient,          "oambient")       \
    X(OButton,           "obutton")        \
    X(OCage,             "ocage")          \
    X(ODeskBook,         "odeskbook")      \
    X(OGehnPage,         "ogehnpage")      \
    X(OMusicPlayer,      "omusicplayer")   \
    X(OStandDrawer,      "ostanddrawer")   \
    X(OStove,            "ostove")         \
    X(PBook,             "pbook")          \
    X(PCage,             "pcage")          \
    X(PCathCheck,        "pcathcheck")     \
    X(PCathTime,         "pcathtime")      \
    X(PCorrectOrder,     "pcorrectorder")  \
    X(PDome,             "pdome")          \
    X(PElevCombo,        "pelevcombo")     \
    X(PTemp,             "ptemp")          \
    X(ReturnCardId,      "returncardid")   \
    X(ReturnStackId,     "returnstackid")  \
    X(RRebel,            "rrebel")         \
    X(RRebelView,        "rrebelview")     \
    X(RVillageTime,      "rvillagetime")   \
    X(TBars,             "tbars")          \
    X(TBeetle,           "tbeetle")        \
    X(TBook,             "tbook")          \
    X(TBookValve,        "tbookvalve")     \
    X(TCage,             "tcage")          \
    X(TCorrectOrder,     "tcorrectorder")  \
    X(TCoverCombo,       "tcovercombo")    \
    X(TDl,               "tdl")            \
    X(TDome,             "tdome")          \
    X(TDomeElev,         "tdomeelev")      \
    X(TDomeElevBtn,      "tdomeelevbtn")   \
    X(TGateBrHandle,     "tgatebrhandle")  \
    X(TGateBridge,       "tgatebridge")    \
    X(TGateState,        "tgatestate")     \
    X(TGriDoor,          "tgridoor")       \
    X(TGrmDoor,          "tgrmdoor")       \
    X(TGroDoor,          "tgrodoor")       \
    X(TGuard,            "tguard")         \
    X(TImageDoor,        "timagedoor")     \
    X(TMagCar,           "tmagcar")        \
    X(TransitionMode,    "transitionmode") \
    X(TSecDoor,          "tsecdoor")       \
    X(TSubBridge,        "tsubbridge")     \
    X(TTeleCover,        "ttelecover")     \
    X(TTeleHandle,       "ttelehandle")    \
    X(TTelePin,          "ttelepin")       \
    X(TTelescope,        "ttelescope")     \
    X(TTeleValve,        "ttelevalve")     \
    X(TTemple,           "ttemple")        \
    X(TTempleDoor,       "ttempledoor")    \
    X(TTunnelDoor,       "ttunneldoor")    \
    X(TViewer,           "tviewer")        \
    X(TWabrValve,        "twabrvalve")     \
    X(TWaffle,           "twaffle")        \
    X(WaterEnabled,      "waterenabled") \
    X(AZip,              "azip")

/// A variable's identity, stable across stacks and across builds of the data.
///
/// Unknown is 0 and means "this NAME 4 entry is not in the list above". It is a
/// real, reachable value -- a script that reads or writes it is silently ignored
/// rather than aliasing some other variable -- and the converter warns once per
/// occurrence so a name added to the game data cannot go missing quietly.
enum class VarId : std::uint16_t
{
    Unknown = 0,
#define RIVEN_VAR_ENUMERATOR(id, str) id,
    RIVEN_VAR_LIST(RIVEN_VAR_ENUMERATOR)
#undef RIVEN_VAR_ENUMERATOR
    kCount
};

/// Including Unknown at index 0.
inline constexpr int kVarCount = static_cast<int>(VarId::kCount);

static_assert(kVarCount == 182, "the variable list changed -- bump kSchemaVersion");

/// The case-folded on-disc name, or "" for Unknown. The inverse of parseVarName.
inline const char *varName(VarId v)
{
    switch (v)
    {
#define RIVEN_VAR_CASE(id, str) \
    case VarId::id:             \
        return str;
        RIVEN_VAR_LIST(RIVEN_VAR_CASE)
#undef RIVEN_VAR_CASE
    default:
        return "";
    }
}

/// Resolve a NAME 4 entry. `lower` must already be case-folded -- the NAME lists
/// are mixed case ("aAtrusBook") and this does not fold for you, because the one
/// caller (the converter) has to fold anyway to build its warning message.
///
/// Linear, like parseStackName: this runs 222 times per conversion, on the host.
inline VarId parseVarName(const std::string &lower)
{
    for (int i = 1; i < kVarCount; ++i)
    {
        const auto id = static_cast<VarId>(i);
        if (lower == varName(id))
            return id;
    }
    return VarId::Unknown;
}

/// The enum is dense and smaller than a hash bucket count, so the identity is
/// the best hash there is; std::hash<VarId> would go through a needless multiply.
struct VarIdHash
{
    std::size_t operator()(VarId v) const noexcept
    {
        return static_cast<std::size_t>(v);
    }
};

} // namespace rivendata
