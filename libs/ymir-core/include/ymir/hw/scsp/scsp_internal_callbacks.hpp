#pragma once

/**
@file
@brief Internal callback definitions used by the SCSP.
*/

#include <ymir/core/types.hpp>
#include <ymir/util/callback.hpp>

namespace ymir::scsp {

/// @brief Invoked when the SCSP needs to raise the SCU sound request interrupt signal.
using CBTriggerSoundRequestInterrupt = util::RequiredCallback<void(bool level)>;

/// @brief Invoked when SCSP WRAM is modified outside the main bus write path.
using CBSCSPWRAMWrite = util::OptionalCallback<void(uint32 address, uint32 size)>;

} // namespace ymir::scsp
