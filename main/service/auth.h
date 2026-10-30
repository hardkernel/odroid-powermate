/*
 * SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#define MAX_TOKENS 4
#define TOKEN_LENGTH 33 // 32 characters + null terminator

// Function to initialize the authentication module
void auth_init(void);

// Function to generate a new token
char* auth_generate_token(void);

// Function to validate a token
bool auth_validate_token(const char* token);

// Function to invalidate a token (e.g., on logout)
void auth_invalidate_token(const char* token);

// Function to clean up expired tokens (if any)
void auth_cleanup_expired_tokens(void);

esp_err_t api_auth_check(httpd_req_t* req);

#endif // AUTH_H
