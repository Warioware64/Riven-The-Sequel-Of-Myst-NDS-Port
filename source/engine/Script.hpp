#pragma once

// Riven's script interpreter.
//
// One handler per opcode, one-to-one with ScummVM's table
// (riven_scripts.cpp:428-489) and cited per opcode below, because that table is
// the specification for what Riven's data means and none of it is guessable from
// the bytes.
//
// The whole table is here rather than the subset the boot path needs. It is
// thirty entries, the aspit menu already reaches a third of them, and a partial
// table's failure mode is a card that half-works -- which is much harder to
// recognise than an opcode that says it is not implemented.
//
// The two irregular encodings are unpacked by the accessors already in the
// shared schema, not re-derived here: opcode 8 (Switch) through
// Command::switchVar / branches and kSwitchDefaultValue, and opcode 27
// (ChangeStack) through changeStackNameId / changeStackGlobalCardId
// (RivenData.hpp:293-306).

#include <vector>

#include "RivenData.hpp"

namespace rivenrt
{

class Engine;

/// Run one command list to completion against `engine`.
void runCommandList(Engine &engine, const std::vector<rivendata::Command> &commands);

/// Dispatch one external command by its NAME 2 index. Implemented per stack;
/// an unknown name logs once and does nothing, so an unported command is a dead
/// control rather than a crash.
void runExternalCommand(Engine &engine, std::uint16_t nameIndex,
                        const std::uint16_t *args, std::size_t argCount);

} // namespace rivenrt
