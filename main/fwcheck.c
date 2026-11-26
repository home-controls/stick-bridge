#include "fwcheck.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/md5.h"

size_t fw_header_len(void) {
    return sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
}

const char *fw_check_header(const uint8_t *buf, size_t len, char *ver_out) {
    if (len < fw_header_len()) return "Not an ESP32 firmware image";
    const esp_image_header_t *ih = (const esp_image_header_t *)buf;
    const esp_app_desc_t *ad = (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC) return "Not an ESP32 firmware image";
    if (ih->chip_id != ESP_CHIP_ID_ESP32P4) return "Firmware is for a different chip";
    if (ad->magic_word != ESP_APP_DESC_MAGIC_WORD) return "Firmware has no app description";
    if (strncmp(ad->project_name, esp_app_get_description()->project_name, sizeof(ad->project_name)) != 0)
        return "Firmware is not a Zigbee Ethernet bridge image";
    if (ver_out) {
        memcpy(ver_out, ad->version, sizeof(ad->version));
        ver_out[sizeof(ad->version)] = 0;
    }
    return NULL;
}

static void parse(const char *s, long out[3]) {
    out[0] = out[1] = out[2] = 0;
    for (int i = 0; i < 3 && s && *s && *s != '+' && *s != '-'; i++) {
        char *end;
        out[i] = strtol(s, &end, 10);
        s = *end == '.' ? end + 1 : end;
        if (*end != '.') break;
    }
}

int fw_version_cmp(const char *a, const char *b) {
    long x[3], y[3];
    parse(a, x);
    parse(b, y);
    for (int i = 0; i < 3; i++) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

static portMUX_TYPE s_write_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_writing;

bool fw_write_lock(void) {
    portENTER_CRITICAL(&s_write_mux);
    bool ok = !s_writing;
    s_writing = true;
    portEXIT_CRITICAL(&s_write_mux);
    return ok;
}

void fw_write_unlock(void) {
    portENTER_CRITICAL(&s_write_mux);
    s_writing = false;
    portEXIT_CRITICAL(&s_write_mux);
}

const char *fw_readback_md5(const esp_partition_t *part, size_t len, const char *md5_hex) {
    uint8_t *buf = malloc(4096);
    if (!buf) return "out of memory";
    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);
    const char *fail = NULL;
    for (size_t off = 0; off < len && !fail; off += 4096) {
        size_t n = len - off < 4096 ? len - off : 4096;
        if (esp_partition_read(part, off, buf, n) != ESP_OK) fail = "reading the written firmware back failed";
        else mbedtls_md5_update(&md5, buf, n);
    }
    uint8_t d[16];
    mbedtls_md5_finish(&md5, d);
    mbedtls_md5_free(&md5);
    free(buf);
    if (fail) return fail;
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        if (md5_hex[i * 2] != hx[d[i] >> 4] || md5_hex[i * 2 + 1] != hx[d[i] & 15])
            return "the firmware read back from flash doesn't match (bad flash write)";
    }
    return NULL;
}