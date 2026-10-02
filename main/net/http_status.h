#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start HTTP server on port 80:
 *   GET /            - HTML status page (AJAX polling)
 *   GET /api/status  - JSON status (wifi, ip, baud, rx, tx, uptime, tcp)
 *   GET /api/baud?b= - Set baud rate
 *   GET /api/capture - timestamped, direction-tagged capture history
 *                      (?since=&max=&fmt=text|hex|csv&want=chunks|meta)
 *   POST /api/send   - Send data to DUT
 */
esp_err_t http_status_start(void);

#ifdef __cplusplus
}
#endif
