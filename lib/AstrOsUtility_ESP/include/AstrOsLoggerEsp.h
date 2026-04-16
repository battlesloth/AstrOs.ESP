#pragma once

#ifdef __cplusplus

#include <AstrOsLogger.hpp>

// Returns an AstrOsLogger whose function pointers forward each level to
// esp_log_writev(), so PURE libs that accept a logger can emit diagnostics
// through the normal ESP-IDF logging pipeline when running on-target.
//
// On the native host this header is not included; pure libs receive a
// default-constructed AstrOsLogger whose pointers are null, turning every
// log call into a no-op.
AstrOsLogger makeEspLogger();

#endif
