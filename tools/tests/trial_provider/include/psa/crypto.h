#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int psa_key_attributes_t;
typedef int psa_key_id_t;
typedef int psa_status_t;
#define PSA_KEY_ATTRIBUTES_INIT 0
#define PSA_SUCCESS 0
#define PSA_KEY_USAGE_SIGN_MESSAGE 0
#define PSA_ALG_SHA_256 0
#define PSA_ALG_HMAC(x) 0
#define PSA_KEY_TYPE_HMAC 0
#define PSA_KEY_LIFETIME_ESP_HMAC_VOLATILE 0
#define psa_set_key_usage_flags(...) ((void)0)
#define psa_set_key_algorithm(...) ((void)0)
#define psa_set_key_type(...) ((void)0)
#define psa_set_key_bits(...) ((void)0)
#define psa_set_key_lifetime(...) ((void)0)
#define psa_reset_key_attributes(...) ((void)0)
#define psa_destroy_key(...) ((void)0)
static inline int psa_import_key(const void *a, const uint8_t *b, size_t c, int *out) { *out = 1; return 0; }
static inline int psa_mac_compute(int key, int alg, const uint8_t *data, size_t len,
                                  uint8_t *out, size_t size, size_t *written)
{ memset(out, 0, size); *written = size; return 0; }
