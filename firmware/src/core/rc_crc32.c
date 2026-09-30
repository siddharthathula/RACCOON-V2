/* RACCOON V2 - CRC-32C and the configuration digest. See rc_crc32.h. */
#include "rc/rc_crc32.h"

#define RC_CRC32C_POLY_REFLECTED 0x82F63B78u
#define RC_CRC32_POLY_REFLECTED 0xEDB88320u

/* Tables are built on first use rather than stored: 1 KiB of rodata saved, and the
 * generation is nine lines of obviously-correct arithmetic that the unit tests pin with
 * known vectors. */
static uint32_t s_crc32c_table[256];
static uint32_t s_crc32_table[256];
static int s_tables_ready;

static void build_table(uint32_t *table, uint32_t poly)
{
    for (uint32_t i = 0u; i < 256u; ++i) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = ((value & 1u) != 0u) ? ((value >> 1) ^ poly) : (value >> 1);
        }
        table[i] = value;
    }
}

static void ensure_tables(void)
{
    if (s_tables_ready == 0) {
        build_table(s_crc32c_table, RC_CRC32C_POLY_REFLECTED);
        build_table(s_crc32_table, RC_CRC32_POLY_REFLECTED);
        s_tables_ready = 1;
    }
}

static uint32_t crc_update(uint32_t crc, const uint32_t *table, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0u; i < len; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

uint32_t rc_crc32c(uint32_t crc, const void *data, size_t len)
{
    if (data == NULL || len == 0u) {
        return crc;
    }
    ensure_tables();
    return crc_update(crc, s_crc32c_table, data, len);
}

uint32_t rc_crc32(uint32_t crc, const void *data, size_t len)
{
    if (data == NULL || len == 0u) {
        return crc;
    }
    ensure_tables();
    return crc_update(crc, s_crc32_table, data, len);
}

void rc_digest_init(rc_digest_t *digest)
{
    if (digest == NULL) {
        return;
    }
    digest->crc = 0u;
    digest->length = 0u;
}

void rc_digest_update(rc_digest_t *digest, const void *data, size_t len)
{
    if (digest == NULL || data == NULL || len == 0u) {
        return;
    }
    digest->crc = rc_crc32c(digest->crc, data, len);
    digest->length += (uint32_t)len;
}

uint32_t rc_digest_final(const rc_digest_t *digest)
{
    return (digest != NULL) ? digest->crc : 0u;
}
