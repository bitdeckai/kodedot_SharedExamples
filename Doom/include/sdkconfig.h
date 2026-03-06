#pragma once

/*
 * Forward to the framework-provided sdkconfig.h.
 *
 * A local empty sdkconfig.h shadows the real ESP-IDF/Arduino config and
 * breaks many CONFIG_* definitions used by framework headers.
 */
#if defined(__GNUC__)
#include_next <sdkconfig.h>
#endif
