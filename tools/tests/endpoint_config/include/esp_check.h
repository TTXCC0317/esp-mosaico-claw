#pragma once
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { esp_err_t e = (expr); if (e != ESP_OK) return e; } while (0)
#define ESP_RETURN_ON_FALSE(test, err, tag, ...) do { if (!(test)) return (err); } while (0)
