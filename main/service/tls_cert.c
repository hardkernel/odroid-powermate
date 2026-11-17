/*
 * SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "tls_cert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "mbedtls/pk.h"
#include "mbedtls/oid.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "nconfig.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include "wifi.h"

static const char* TAG = "tls_cert";

#define TLS_PARTITION "tls"
#define TLS_NAMESPACE "tls"
// One blob holds "<cert PEM>\0<key PEM>\0", so a power cut can never leave a certificate
// next to another certificate's key: NVS replaces a blob in one step.
#define TLS_KEY_PAIR "pair"
#define TLS_KEY_SOURCE "source"

enum { SOURCE_SELF_SIGNED = 0, SOURCE_USER = 1 };

static SemaphoreHandle_t s_lock;
static bool s_ready;

// Key generation, certificate parsing and signing need several KB of stack; the main task
// and httpd (in HTTP mode) have less to spare, so these run in a short-lived task.
typedef struct
{
    esp_err_t (*fn)(void*);
    void* arg;
    esp_err_t result;
    SemaphoreHandle_t done;
} tls_job_t;

static void tls_job_task(void* p)
{
    tls_job_t* job = p;
    job->result = job->fn(job->arg);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static esp_err_t run_job(esp_err_t (*fn)(void*), void* arg)
{
    tls_job_t job = {.fn = fn, .arg = arg, .result = ESP_FAIL, .done = xSemaphoreCreateBinary()};
    if (!job.done)
        return ESP_ERR_NO_MEM;
    if (xTaskCreate(tls_job_task, "tls_job", 8192, &job, 5, NULL) != pdPASS)
    {
        vSemaphoreDelete(job.done);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    return job.result;
}

static esp_err_t store_write(const char* cert, const char* key, uint8_t source)
{
    size_t cert_len = strlen(cert) + 1, key_len = strlen(key) + 1;
    char* blob = malloc(cert_len + key_len);
    if (!blob)
        return ESP_ERR_NO_MEM;
    memcpy(blob, cert, cert_len);
    memcpy(blob + cert_len, key, key_len);

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(TLS_PARTITION, TLS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK)
    {
        err = nvs_set_blob(h, TLS_KEY_PAIR, blob, cert_len + key_len);
        if (err == ESP_OK)
            err = nvs_set_u8(h, TLS_KEY_SOURCE, source);
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    memset(blob, 0, cert_len + key_len);
    free(blob);
    return err;
}

/** Reads the stored pair into one heap buffer; *key points into it. */
static esp_err_t store_read(char** blob_out, size_t* cert_len, char** key, size_t* key_len, uint8_t* source)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(TLS_PARTITION, TLS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;
    size_t len = 0;
    err = nvs_get_blob(h, TLS_KEY_PAIR, NULL, &len);
    char* blob = NULL;
    if (err == ESP_OK && (len < 4 || len > TLS_CERT_MAX_PEM + TLS_KEY_MAX_PEM))
        err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && !(blob = malloc(len + 1)))
        err = ESP_ERR_NO_MEM;
    if (err == ESP_OK)
        err = nvs_get_blob(h, TLS_KEY_PAIR, blob, &len);
    if (err == ESP_OK && source && nvs_get_u8(h, TLS_KEY_SOURCE, source) != ESP_OK)
        *source = SOURCE_SELF_SIGNED;
    nvs_close(h);
    if (err != ESP_OK)
    {
        free(blob);
        return err;
    }
    blob[len] = '\0';
    size_t clen = strnlen(blob, len) + 1;
    if (clen >= len)
    {
        free(blob);
        return ESP_ERR_INVALID_STATE;
    }
    *blob_out = blob;
    *cert_len = clen;
    *key = blob + clen;
    *key_len = strnlen(*key, len - clen) + 1;
    return ESP_OK;
}

static void wipe_free(char* p)
{
    if (p)
    {
        memset(p, 0, strlen(p));
        free(p);
    }
}

/**
 * Parses a PEM chain and key and checks that they belong together and fit a handshake.
 * Returns ESP_ERR_NO_MEM when it ran out of memory (the pair may be fine) and
 * ESP_ERR_INVALID_ARG with *reason when the pair is unusable.
 */
static esp_err_t check_pair(const char* cert, size_t cert_len, const char* key, size_t key_len, bool restrict_types,
                            const char** reason)
{
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_init(&pk);
    esp_err_t err = ESP_OK;
    *reason = NULL;

    int r = mbedtls_x509_crt_parse(&crt, (const unsigned char*)cert, cert_len);
    if (r == PSA_ERROR_INSUFFICIENT_MEMORY)
        err = ESP_ERR_NO_MEM;
    else if (r != 0)
        *reason = "certificate is not a valid PEM certificate";
    if (err == ESP_OK && !*reason)
    {
        // The server sends the whole chain in one TLS record of MBEDTLS_SSL_OUT_CONTENT_LEN
        // bytes; a longer chain would fail every handshake after the next boot.
        size_t total = 7;
        for (const mbedtls_x509_crt* c = &crt; c && c->raw.p; c = c->next)
            total += 3 + c->raw.len;
        if (total > MBEDTLS_SSL_OUT_CONTENT_LEN)
            *reason = "certificate chain is too large: leave out the root and send at most the certificate and "
                      "its intermediates (about 4 KB in all)";
    }
    if (err == ESP_OK && !*reason)
    {
        r = mbedtls_pk_parse_key(&pk, (const unsigned char*)key, key_len, NULL, 0);
        if (r == PSA_ERROR_INSUFFICIENT_MEMORY)
            err = ESP_ERR_NO_MEM;
        else if (r == MBEDTLS_ERR_PK_PASSWORD_REQUIRED || r == MBEDTLS_ERR_PK_PASSWORD_MISMATCH)
            *reason = "private key is encrypted; remove the passphrase (openssl pkey -in key.pem -out plain.pem)";
        else if (r != 0)
            *reason = "private key is not a valid PEM key";
    }
    if (err == ESP_OK && !*reason && restrict_types)
    {
        psa_key_type_t type = mbedtls_pk_get_key_type(&pk);
        size_t bits = mbedtls_pk_get_bitlen(&pk);
        bool ec_p256 = PSA_KEY_TYPE_IS_ECC(type) && PSA_KEY_TYPE_ECC_GET_FAMILY(type) == PSA_ECC_FAMILY_SECP_R1 &&
            bits == 256;
        bool rsa_2048 = PSA_KEY_TYPE_IS_RSA(type) && bits == 2048;
        if (!ec_p256 && !rsa_2048)
            *reason = "key must be EC P-256 (recommended) or RSA-2048";
    }
    // The pair matches when the key's public half is the certificate's public key.
    // (mbedtls_pk_check_pair reported valid RSA pairs as mismatched in mbedTLS 4.1.)
    if (err == ESP_OK && !*reason)
    {
        unsigned char* der = malloc(2 * 600);
        if (!der)
            err = ESP_ERR_NO_MEM;
        else
        {
            int a = mbedtls_pk_write_pubkey_der(&crt.pk, der, 600);
            int b = mbedtls_pk_write_pubkey_der(&pk, der + 600, 600);
            // The DER is written at the end of each buffer.
            if (a == PSA_ERROR_INSUFFICIENT_MEMORY || b == PSA_ERROR_INSUFFICIENT_MEMORY)
                err = ESP_ERR_NO_MEM;
            else if (a <= 0 || b <= 0 || a != b || memcmp(der + 600 - a, der + 1200 - b, a) != 0)
                *reason = "private key does not match the first certificate";
            free(der);
        }
    }

    mbedtls_pk_free(&pk);
    mbedtls_x509_crt_free(&crt);
    if (err == ESP_OK && *reason)
        err = ESP_ERR_INVALID_ARG;
    if (err == ESP_ERR_NO_MEM)
        *reason = "out of memory while checking the certificate";
    return err;
}

static esp_err_t verify_stored_job(void* arg)
{
    char* blob = NULL;
    char* key;
    size_t cert_len, key_len;
    esp_err_t err = store_read(&blob, &cert_len, &key, &key_len, NULL);
    if (err != ESP_OK)
        return err;
    const char* reason;
    err = check_pair(blob, cert_len, key, key_len, false, &reason);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "stored certificate %s: %s", err == ESP_ERR_NO_MEM ? "not checked" : "unusable", reason);
    memset(blob, 0, cert_len + key_len);
    free(blob);
    // Only a pair that is definitely unusable is replaced; running out of memory keeps it.
    return err == ESP_ERR_INVALID_ARG ? ESP_ERR_INVALID_STATE : err;
}

static esp_err_t generate_job(void* arg)
{
    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS)
        return ESP_FAIL;

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH | PSA_KEY_USAGE_EXPORT);
    psa_key_id_t id = 0;
    if (psa_generate_key(&attr, &id) != PSA_SUCCESS)
        return ESP_FAIL;

    mbedtls_pk_context pk;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_crt_init(&crt);
    char* cert_pem = calloc(1, 2048);
    char* key_pem = calloc(1, 512);
    int r = (cert_pem && key_pem) ? mbedtls_pk_copy_from_psa(id, &pk) : -1;

    // Names a browser or a client may check: <hostname>.local, the setup AP address and,
    // when one is configured, the static STA address.
    const char* hostname = wifi_hostname();
    char dns_local[48], subject[96];
    snprintf(dns_local, sizeof(dns_local), "%s.local", hostname);
    snprintf(subject, sizeof(subject), "CN=%s,O=ODROID PowerMate", hostname);
    uint8_t ap_ip[4] = {192, 168, 4, 1};
    uint8_t sta_ip[4];
    char buf[20] = "";
    bool have_sta_ip = false;
    if (nconfig_read(NETIF_TYPE, buf, sizeof(buf)) == ESP_OK && strcmp(buf, "static") == 0 &&
        nconfig_read(NETIF_IP, buf, sizeof(buf)) == ESP_OK)
    {
        ip4_addr_t ip;
        if (ip4addr_aton(buf, &ip))
        {
            memcpy(sta_ip, &ip.addr, 4); // network byte order
            have_sta_ip = memcmp(sta_ip, ap_ip, 4) != 0;
        }
    }
    mbedtls_x509_san_list san[3] = {0};
    san[0].node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san[0].node.san.unstructured_name.p = (unsigned char*)dns_local;
    san[0].node.san.unstructured_name.len = strlen(dns_local);
    san[0].next = &san[1];
    san[1].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
    san[1].node.san.unstructured_name.p = ap_ip;
    san[1].node.san.unstructured_name.len = 4;
    if (have_sta_ip)
    {
        san[1].next = &san[2];
        san[2].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
        san[2].node.san.unstructured_name.p = sta_ip;
        san[2].node.san.unstructured_name.len = 4;
    }

    if (r == 0)
    {
        unsigned char serial[16];
        esp_fill_random(serial, sizeof(serial));
        serial[0] = (serial[0] & 0x7f) | 0x01; // positive, no leading zero byte
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &pk);
        mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
        r = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    }
    if (r == 0)
        r = mbedtls_x509write_crt_set_subject_name(&crt, subject);
    if (r == 0)
        r = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
    // There is no clock at first boot, so the validity is fixed and long.
    if (r == 0)
        r = mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20491231235959");
    if (r == 0)
        r = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    // serverAuth: Apple platforms refuse TLS server certificates without it.
    mbedtls_asn1_sequence eku = {
        .buf = {.tag = MBEDTLS_ASN1_OID,
                .len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH),
                .p = (unsigned char*)MBEDTLS_OID_SERVER_AUTH},
    };
    if (r == 0)
        r = mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku);
    if (r == 0)
        r = mbedtls_x509write_crt_set_subject_alternative_name(&crt, san);
    if (r == 0)
        r = mbedtls_x509write_crt_pem(&crt, (unsigned char*)cert_pem, 2048);
    if (r == 0)
        r = mbedtls_pk_write_key_pem(&pk, (unsigned char*)key_pem, 512);

    esp_err_t err = r == 0 ? store_write(cert_pem, key_pem, SOURCE_SELF_SIGNED) : ESP_FAIL;
    if (r != 0)
        ESP_LOGE(TAG, "self-signed certificate failed: -0x%04x", -r);
    else if (err == ESP_OK)
        ESP_LOGI(TAG, "created a self-signed EC P-256 certificate for %s", dns_local);

    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    psa_destroy_key(id);
    free(cert_pem);
    wipe_free(key_pem);
    return err;
}

esp_err_t tls_cert_init(void)
{
    if (!s_lock && !(s_lock = xSemaphoreCreateMutex()))
        return ESP_ERR_NO_MEM;

    esp_err_t err = nvs_flash_init_partition(TLS_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // Only this partition: the settings NVS is never touched here.
        ESP_LOGW(TAG, "formatting the %s partition", TLS_PARTITION);
        err = nvs_flash_erase_partition(TLS_PARTITION);
        if (err == ESP_OK)
            err = nvs_flash_init_partition(TLS_PARTITION);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "certificate store unavailable: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = run_job(verify_stored_job, NULL);
    // Replace the pair only when there is none or it is definitely unusable. Any other
    // failure (memory, task creation) keeps it; the device serves HTTP this boot.
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_SIZE)
        err = run_job(generate_job, NULL);
    s_ready = err == ESP_OK;
    xSemaphoreGive(s_lock);
    return err;
}

bool tls_cert_ready(void)
{
    return s_ready;
}

esp_err_t tls_cert_load(char** cert, size_t* cert_len, char** key, size_t* key_len)
{
    if (!s_lock)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char* blob = NULL;
    char* stored_key;
    size_t clen, klen;
    esp_err_t err = store_read(&blob, &clen, &stored_key, &klen, NULL);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK)
        return err;
    *cert = malloc(clen);
    *key = malloc(klen);
    if (!*cert || !*key)
    {
        free(*cert);
        free(*key);
        memset(blob, 0, clen + klen);
        free(blob);
        return ESP_ERR_NO_MEM;
    }
    memcpy(*cert, blob, clen);
    memcpy(*key, stored_key, klen);
    *cert_len = clen;
    *key_len = klen;
    memset(blob, 0, clen + klen);
    free(blob);
    return ESP_OK;
}

esp_err_t tls_cert_generate(void)
{
    if (!s_lock)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = run_job(generate_job, NULL);
    if (err == ESP_OK)
        s_ready = true;
    xSemaphoreGive(s_lock);
    return err;
}

typedef struct
{
    const char* cert;
    const char* key;
    const char* reason;
} install_args_t;

static esp_err_t install_job(void* arg)
{
    install_args_t* a = arg;
    size_t cert_len = strlen(a->cert) + 1, key_len = strlen(a->key) + 1;
    if (cert_len > TLS_CERT_MAX_PEM || key_len > TLS_KEY_MAX_PEM)
    {
        a->reason = "certificate or key is too large";
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t check = check_pair(a->cert, cert_len, a->key, key_len, true, &a->reason);
    if (check != ESP_OK)
        return check;
    esp_err_t err = store_write(a->cert, a->key, SOURCE_USER);
    if (err != ESP_OK)
        a->reason = "storing the certificate failed";
    return err;
}

esp_err_t tls_cert_install(const char* cert, const char* key, const char** reason)
{
    if (!s_lock)
        return ESP_ERR_INVALID_STATE;
    install_args_t args = {.cert = cert, .key = key};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = run_job(install_job, &args);
    if (err == ESP_OK)
        s_ready = true;
    xSemaphoreGive(s_lock);
    if (reason)
        *reason = args.reason;
    return err;
}

static void add_time(cJSON* obj, const char* name, const mbedtls_x509_time* t)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", t->year, t->mon, t->day, t->hour, t->min, t->sec);
    cJSON_AddStringToObject(obj, name, buf);
}

static esp_err_t describe_job(void* arg)
{
    cJSON* obj = arg;
    char* blob = NULL;
    char* key;
    size_t cert_len, key_len;
    uint8_t source = SOURCE_SELF_SIGNED;
    esp_err_t err = store_read(&blob, &cert_len, &key, &key_len, &source);
    if (err != ESP_OK)
        return err;
    memset(key, 0, key_len); // not needed here

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char*)blob, cert_len) == 0)
    {
        char buf[160];
        cJSON_AddStringToObject(obj, "source", source == SOURCE_USER ? "user" : "self-signed");
        if (mbedtls_x509_dn_gets(buf, sizeof(buf), &crt.subject) > 0)
            cJSON_AddStringToObject(obj, "subject", buf);
        if (mbedtls_x509_dn_gets(buf, sizeof(buf), &crt.issuer) > 0)
            cJSON_AddStringToObject(obj, "issuer", buf);
        add_time(obj, "not_before", &crt.valid_from);
        add_time(obj, "not_after", &crt.valid_to);

        psa_key_type_t type = mbedtls_pk_get_key_type(&crt.pk);
        snprintf(buf, sizeof(buf), "%s %u", PSA_KEY_TYPE_IS_RSA(type) ? "RSA" : PSA_KEY_TYPE_IS_ECC(type) ? "EC" : "?",
                 (unsigned)mbedtls_pk_get_bitlen(&crt.pk));
        cJSON_AddStringToObject(obj, "key", buf);

        uint8_t hash[32];
        size_t hash_len = 0;
        if (psa_hash_compute(PSA_ALG_SHA_256, crt.raw.p, crt.raw.len, hash, sizeof(hash), &hash_len) == PSA_SUCCESS)
        {
            char fp[32 * 3];
            for (size_t i = 0; i < hash_len; i++)
                snprintf(fp + i * 3, 4, i + 1 < hash_len ? "%02X:" : "%02X", hash[i]);
            cJSON_AddStringToObject(obj, "sha256", fp);
        }

        cJSON* names = cJSON_AddArrayToObject(obj, "names");
        for (const mbedtls_x509_sequence* s = &crt.subject_alt_names; s && s->buf.p; s = s->next)
        {
            mbedtls_x509_subject_alternative_name san;
            if (mbedtls_x509_parse_subject_alt_name(&s->buf, &san) != 0)
                continue;
            const mbedtls_x509_buf* v = &san.san.unstructured_name;
            if (san.type == MBEDTLS_X509_SAN_DNS_NAME && v->len < sizeof(buf))
            {
                memcpy(buf, v->p, v->len);
                buf[v->len] = '\0';
                cJSON_AddItemToArray(names, cJSON_CreateString(buf));
            }
            else if (san.type == MBEDTLS_X509_SAN_IP_ADDRESS && v->len == 4)
            {
                snprintf(buf, sizeof(buf), "%u.%u.%u.%u", v->p[0], v->p[1], v->p[2], v->p[3]);
                cJSON_AddItemToArray(names, cJSON_CreateString(buf));
            }
            mbedtls_x509_free_subject_alt_name(&san);
        }
        int chain = 0;
        for (const mbedtls_x509_crt* c = &crt; c && c->raw.p; c = c->next)
            chain++;
        cJSON_AddNumberToObject(obj, "chain_length", chain);
    }
    else
    {
        err = ESP_ERR_INVALID_STATE;
    }
    mbedtls_x509_crt_free(&crt);
    memset(blob, 0, cert_len);
    free(blob);
    return err;
}

void tls_cert_describe(cJSON* obj)
{
    if (!s_lock)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    run_job(describe_job, obj);
    xSemaphoreGive(s_lock);
}
