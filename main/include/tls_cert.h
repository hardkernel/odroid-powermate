/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef POWERMATE_TLS_CERT_H
#define POWERMATE_TLS_CERT_H

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "esp_err.h"

// The HTTPS certificate and private key live in their own NVS partition ("tls"), outside
// the image that a firmware update writes, so the certificate a browser was told to trust
// survives updates and settings resets.

#define TLS_CERT_MAX_PEM 8192 ///< Certificate chain, PEM
#define TLS_KEY_MAX_PEM 4096  ///< Private key, PEM

/**
 * Opens the certificate store and makes sure it holds a usable pair. When it is empty or
 * its pair does not load, a self-signed EC P-256 certificate is created. Call after Wi-Fi
 * is started (the RNG needs the radio) and with the network hostname known.
 */
esp_err_t tls_cert_init(void);

/** True when the store holds a certificate and key that load and match. */
bool tls_cert_ready(void);

/**
 * Copies the certificate chain and key as NUL-terminated PEM into new heap buffers. The
 * lengths include the NUL. The caller frees both, and must not log or return the key.
 */
esp_err_t tls_cert_load(char** cert, size_t* cert_len, char** key, size_t* key_len);

/** Replaces the pair with a new self-signed EC P-256 certificate for this device. */
esp_err_t tls_cert_generate(void);

/**
 * Checks and stores a user certificate chain and its private key (PEM). The key must be
 * unencrypted, match the first certificate and be EC P-256 or RSA-2048. On failure
 * nothing is stored and *reason, when given, says why.
 */
esp_err_t tls_cert_install(const char* cert, const char* key, const char** reason);

/** Adds the stored certificate's details (never the key) to @p obj. */
void tls_cert_describe(cJSON* obj);

#endif // POWERMATE_TLS_CERT_H
