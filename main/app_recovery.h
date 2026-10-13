// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"

/* Call only after local startup succeeds. Does not start a transport or writer. */
esp_err_t app_recovery_mark_healthy(void);
