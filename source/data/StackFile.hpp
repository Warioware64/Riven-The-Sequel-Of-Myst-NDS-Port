#pragma once

// Loading stacks/<stack>.bin -- the card graph the converter yas-serialized.
//
// This is the file that replaces the Myst port's whole JSON pipeline. There,
// Python wrote nodes/<stack>.json, the DS parsed it with nlohmann on first boot,
// baked it to a yas binary and offered to delete the JSON afterwards. Here the
// converter linked shared/RivenData.hpp -- the same header this compiles -- and
// wrote the archive directly, so loading a stack is a header check and one
// deserialize.
//
// The 16-byte StackFileHeader in front of the payload is what makes that safe.
// yas binary archives are not self-describing: without a version to check, a ROM
// and a converter that disagree about a struct would deserialize garbage in
// silence rather than fail.

#include <string>

#include "RivenData.hpp"

namespace rivenrt
{

/// Read `path` into `out`. False (with `error` set) on a missing file, a bad
/// magic, a schema version this build was not built against, or a payload that
/// does not deserialize.
bool loadStackFile(const std::string &path, rivendata::Stack &out, std::string &error);

} // namespace rivenrt
