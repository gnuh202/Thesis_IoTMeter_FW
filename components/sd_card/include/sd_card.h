#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sd_card_detect_init(void);
bool sd_card_is_inserted(void);

#ifdef __cplusplus
}
#endif
