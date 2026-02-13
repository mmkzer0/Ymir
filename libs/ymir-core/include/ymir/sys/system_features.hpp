#pragma once

namespace ymir::sys {

struct SystemFeatures {
    bool enableDebugTracing = false;
    bool emulateSH2Cache = false;
    bool enableBlockCache = false;
    bool enableBlockBurst = false;
};

} // namespace ymir::sys
