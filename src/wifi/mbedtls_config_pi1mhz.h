/* mbedtls_config_pi1mhz.h - mbedTLS 3.6 for https:// on the Pi (lwIP's
   altcp_tls): a TLS 1.2 client and nothing else.

   - Key exchange ECDHE, signed by RSA or ECDSA; AES-GCM or ChaCha20-Poly1305.
     That is what every current web server offers over TLS 1.2.
   - Certificates are verified against the CA bundle on the card (see
     net_service.c), including the host name.  A root this build cannot
     parse is skipped, not fatal (lwip-altcp-tls-mbedtls3.patch).  There is no clock
     (MBEDTLS_HAVE_TIME_DATE is off), so validity dates are not checked -
     dp111's choice, 2026-09-26.
   - Entropy comes only from the BCM2835 hardware RNG (rpi/hwrng.c).
   - Memory is newlib's calloc/free, not lwIP's MEM_SIZE heap.
   Selected from mbedtls/include/mbedtls/mbedtls_config.h; everything not
   named here is off. */
#ifndef MBEDTLS_CONFIG_PI1MHZ_H
#define MBEDTLS_CONFIG_PI1MHZ_H

/* platform */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* hashes */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C     /* to PARSE the bundle's SHA-1 self-signed roots (3 of 121
                              fail without it, MEASURED); SHA-1 signatures in a chain
                              are still refused by the default X.509 profile */
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* ciphers */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

/* public key */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED     /* a few roots are P-521 (e-Szigno 2023) */
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

/* random */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* certificates */
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
/* A server may send full 16 KB records; we send small ones. */
#define MBEDTLS_SSL_IN_CONTENT_LEN    16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN   4096

#endif
