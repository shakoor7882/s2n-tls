/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <stdlib.h>

#include "api/s2n.h"
#include "crypto/s2n_fips.h"
#include "s2n_test.h"
#include "testlib/s2n_testlib.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_handshake.h"
#include "tls/s2n_security_policies.h"
#include "utils/s2n_safety.h"

struct host_verify_data {
    uint8_t callback_invoked;
    uint8_t allow;
};

static uint8_t verify_host_fn(const char *host_name, size_t host_name_len, void *data)
{
    struct host_verify_data *verify_data = (struct host_verify_data *) data;
    verify_data->callback_invoked = 1;
    return verify_data->allow;
}

int main(int argc, char **argv)
{
    struct s2n_config *config = NULL;
    const struct s2n_security_policy *default_security_policy = NULL;
    const struct s2n_cipher_preferences *default_cipher_preferences = NULL;
    char *cert_chain_pem = NULL;
    char *private_key_pem = NULL;
    char *dhparams_pem = NULL;
    struct s2n_cert_chain_and_key *chain_and_key = NULL;

    BEGIN_TEST();
    EXPECT_SUCCESS(s2n_disable_tls13_in_test());

    EXPECT_NOT_NULL(cert_chain_pem = malloc(S2N_MAX_TEST_PEM_SIZE));
    EXPECT_NOT_NULL(private_key_pem = malloc(S2N_MAX_TEST_PEM_SIZE));
    EXPECT_NOT_NULL(dhparams_pem = malloc(S2N_MAX_TEST_PEM_SIZE));

    /*
     * Test Mutual Auth using **s2n_connection_set_client_auth_type**
     */

    EXPECT_NOT_NULL(config = s2n_config_new());
    EXPECT_SUCCESS(s2n_read_test_pem(S2N_DEFAULT_TEST_CERT_CHAIN, cert_chain_pem, S2N_MAX_TEST_PEM_SIZE));
    EXPECT_SUCCESS(s2n_read_test_pem(S2N_DEFAULT_TEST_PRIVATE_KEY, private_key_pem, S2N_MAX_TEST_PEM_SIZE));
    EXPECT_SUCCESS(s2n_read_test_pem(S2N_DEFAULT_TEST_DHPARAMS, dhparams_pem, S2N_MAX_TEST_PEM_SIZE));
    /* Later versions of the default security policies include ECDSA, which this test does not handle.
     * We can't just add an ECDSA certificate to the test, because only one cert is allowed in client mode.
     * Freeze the version of the security policy used by this test. */
    EXPECT_SUCCESS(s2n_config_set_cipher_preferences(config, "20170210"));

    EXPECT_NOT_NULL(chain_and_key = s2n_cert_chain_and_key_new());
    EXPECT_SUCCESS(s2n_cert_chain_and_key_load_pem(chain_and_key, cert_chain_pem, private_key_pem));
    EXPECT_SUCCESS(s2n_config_add_cert_chain_and_key_to_store(config, chain_and_key));

    EXPECT_SUCCESS(s2n_config_add_dhparams(config, dhparams_pem));
    EXPECT_NOT_NULL(default_security_policy = config->security_policy);
    EXPECT_NOT_NULL(default_cipher_preferences = default_security_policy->cipher_preferences);

    struct host_verify_data verify_data = { .allow = 1, .callback_invoked = 0 };
    EXPECT_SUCCESS(s2n_config_set_verify_host_callback(config, verify_host_fn, &verify_data));
    EXPECT_SUCCESS(s2n_config_set_verification_ca_location(config, S2N_DEFAULT_TEST_CERT_CHAIN, NULL));

    /* Verify that a handshake succeeds for every cipher in the default list. */
    for (size_t cipher_idx = 0; cipher_idx < default_cipher_preferences->count; cipher_idx++) {
        verify_data.callback_invoked = 0;
        struct s2n_cipher_preferences server_cipher_preferences;
        struct s2n_security_policy server_security_policy;
        struct s2n_connection *client_conn = NULL;
        struct s2n_connection *server_conn = NULL;
        struct s2n_stuffer client_to_server = { 0 };
        struct s2n_stuffer server_to_client = { 0 };

        /* Craft a cipher preference with a cipher_idx cipher */
        EXPECT_MEMCPY_SUCCESS(&server_cipher_preferences, default_cipher_preferences, sizeof(server_cipher_preferences));
        server_cipher_preferences.count = 1;
        struct s2n_cipher_suite *cur_cipher = default_cipher_preferences->suites[cipher_idx];

        if (!cur_cipher->available) {
            /* Skip Ciphers that aren't supported with the linked libcrypto */
            continue;
        }

        server_cipher_preferences.suites = &cur_cipher;

        EXPECT_MEMCPY_SUCCESS(&server_security_policy, default_security_policy, sizeof(server_security_policy));
        server_security_policy.cipher_preferences = &server_cipher_preferences;

        config->security_policy = &server_security_policy;

        EXPECT_NOT_NULL(server_conn = s2n_connection_new(S2N_SERVER));
        EXPECT_SUCCESS(s2n_connection_set_client_auth_type(server_conn, S2N_CERT_AUTH_REQUIRED));
        EXPECT_SUCCESS(s2n_connection_set_config(server_conn, config));

        EXPECT_NOT_NULL(client_conn = s2n_connection_new(S2N_CLIENT));
        EXPECT_SUCCESS(s2n_connection_set_client_auth_type(client_conn, S2N_CERT_AUTH_REQUIRED));
        EXPECT_SUCCESS(s2n_connection_set_config(client_conn, config));

        /* Set up our I/O callbacks. Use stuffers for the "I/O context" */
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&client_to_server, 0));
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&server_to_client, 0));
        EXPECT_SUCCESS(s2n_connection_set_io_stuffers(&server_to_client, &client_to_server, client_conn));
        EXPECT_SUCCESS(s2n_connection_set_io_stuffers(&client_to_server, &server_to_client, server_conn));

        EXPECT_SUCCESS(s2n_negotiate_test_server_and_client(server_conn, client_conn));

        /* Verify that both connections negotiated Mutual Auth */
        EXPECT_TRUE(s2n_connection_client_cert_used(server_conn));
        EXPECT_TRUE(s2n_connection_client_cert_used(client_conn));
        EXPECT_TRUE(verify_data.callback_invoked);

        EXPECT_SUCCESS(s2n_connection_free(client_conn));
        EXPECT_SUCCESS(s2n_connection_free(server_conn));
        EXPECT_SUCCESS(s2n_stuffer_free(&server_to_client));
        EXPECT_SUCCESS(s2n_stuffer_free(&client_to_server));
    }

    /*
     * Test Mutual Auth using **s2n_config_set_client_auth_type**
     */

    EXPECT_SUCCESS(s2n_config_set_client_auth_type(config, S2N_CERT_AUTH_REQUIRED));

    /* Verify that a handshake succeeds for every cipher in the default list. */
    for (size_t cipher_idx = 0; cipher_idx < default_cipher_preferences->count; cipher_idx++) {
        struct s2n_cipher_preferences server_cipher_preferences;
        struct s2n_security_policy server_security_policy;
        struct s2n_connection *client_conn = NULL;
        struct s2n_connection *server_conn = NULL;
        struct s2n_stuffer client_to_server = { 0 };
        struct s2n_stuffer server_to_client = { 0 };

        /* Craft a cipher preference with a cipher_idx cipher */
        EXPECT_MEMCPY_SUCCESS(&server_cipher_preferences, default_cipher_preferences, sizeof(server_cipher_preferences));
        server_cipher_preferences.count = 1;
        struct s2n_cipher_suite *cur_cipher = default_cipher_preferences->suites[cipher_idx];

        if (!cur_cipher->available) {
            /* Skip Ciphers that aren't supported with the linked libcrypto */
            continue;
        }

        server_cipher_preferences.suites = &cur_cipher;
        EXPECT_MEMCPY_SUCCESS(&server_security_policy, default_security_policy, sizeof(server_security_policy));
        server_security_policy.cipher_preferences = &server_cipher_preferences;
        config->security_policy = &server_security_policy;

        EXPECT_NOT_NULL(server_conn = s2n_connection_new(S2N_SERVER));
        EXPECT_SUCCESS(s2n_connection_set_config(server_conn, config));

        EXPECT_NOT_NULL(client_conn = s2n_connection_new(S2N_CLIENT));
        EXPECT_SUCCESS(s2n_connection_set_config(client_conn, config));

        /* Set up our I/O callbacks. Use stuffers for the "I/O context" */
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&client_to_server, 0));
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&server_to_client, 0));
        EXPECT_SUCCESS(s2n_connection_set_io_stuffers(&server_to_client, &client_to_server, client_conn));
        EXPECT_SUCCESS(s2n_connection_set_io_stuffers(&client_to_server, &server_to_client, server_conn));

        EXPECT_SUCCESS(s2n_negotiate_test_server_and_client(server_conn, client_conn));

        /* Verify that both connections negotiated Mutual Auth */
        EXPECT_TRUE(s2n_connection_client_cert_used(server_conn));
        EXPECT_TRUE(s2n_connection_client_cert_used(client_conn));

        EXPECT_SUCCESS(s2n_connection_free(client_conn));
        EXPECT_SUCCESS(s2n_connection_free(server_conn));
        EXPECT_SUCCESS(s2n_stuffer_free(&server_to_client));
        EXPECT_SUCCESS(s2n_stuffer_free(&client_to_server));
    }

    /*
     * Test Mutual Auth using connection override of **s2n_config_set_client_auth_type**
     */

    EXPECT_SUCCESS(s2n_config_set_client_auth_type(config, S2N_CERT_AUTH_NONE));

    /* Verify that a handshake succeeds for every cipher in the default list. */
    for (size_t cipher_idx = 0; cipher_idx < default_cipher_preferences->count; cipher_idx++) {
        struct s2n_cipher_preferences server_cipher_preferences;
        struct s2n_security_policy server_security_policy;
        struct s2n_connection *client_conn = NULL;
        struct s2n_connection *server_conn = NULL;
        struct s2n_stuffer client_to_server = { 0 };
        struct s2n_stuffer server_to_client = { 0 };

        /* Craft a cipher preference with a cipher_idx cipher */
        EXPECT_MEMCPY_SUCCESS(&server_cipher_preferences, default_cipher_preferences, sizeof(server_cipher_preferences));
        server_cipher_preferences.count = 1;
        struct s2n_cipher_suite *cur_cipher = default_cipher_preferences->suites[cipher_idx];

        if (!cur_cipher->available) {
            /* Skip Ciphers that aren't supported with the linked libcrypto */
            continue;
        }

        server_cipher_preferences.suites = &cur_cipher;

        EXPECT_MEMCPY_SUCCESS(&server_security_policy, default_security_policy, sizeof(server_security_policy));
        server_security_policy.cipher_preferences = &server_cipher_preferences;

        config->security_policy = &server_security_policy;

        EXPECT_NOT_NULL(server_conn = s2n_connection_new(S2N_SERVER));
        EXPECT_SUCCESS(s2n_connection_set_config(server_conn, config));

        EXPECT_NOT_NULL(client_conn = s2n_connection_new(S2N_CLIENT));
        EXPECT_SUCCESS(s2n_connection_set_config(client_conn, config));

        EXPECT_SUCCESS(s2n_connection_set_client_auth_type(server_conn, S2N_CERT_AUTH_REQUIRED));
        EXPECT_SUCCESS(s2n_connection_set_client_auth_type(client_conn, S2N_CERT_AUTH_REQUIRED));

        /* Set up our I/O callbacks. Use stuffers for the "I/O context" */
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&client_to_server, 0));
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&server_to_client, 0));
        EXPECT_SUCCESS(s2n_connection_set_io_stuffers(&server_to_client, &client_to_server, client_conn));
        EXPECT_SUCCESS(s2n_connection_set_io_stuffers(&client_to_server, &server_to_client, server_conn));

        EXPECT_SUCCESS(s2n_negotiate_test_server_and_client(server_conn, client_conn));

        /* Verify that both connections negotiated Mutual Auth */
        EXPECT_TRUE(s2n_connection_client_cert_used(server_conn));
        EXPECT_TRUE(s2n_connection_client_cert_used(client_conn));

        EXPECT_SUCCESS(s2n_connection_free(client_conn));
        EXPECT_SUCCESS(s2n_connection_free(server_conn));
        EXPECT_SUCCESS(s2n_stuffer_free(&server_to_client));
        EXPECT_SUCCESS(s2n_stuffer_free(&client_to_server));
    }

    /*
     * Test Mutual Auth using connection override of **s2n_config_set_client_auth_type** only on one side of the
     * connection and verify that a connection is not established
     */

    EXPECT_SUCCESS(s2n_config_set_client_auth_type(config, S2N_CERT_AUTH_NONE));

    /* Verify that a handshake succeeds for every cipher in the default list. */
    for (size_t cipher_idx = 0; cipher_idx < default_cipher_preferences->count; cipher_idx++) {
        struct s2n_cipher_preferences server_cipher_preferences;
        struct s2n_security_policy server_security_policy;
        struct s2n_connection *client_conn = NULL;
        struct s2n_connection *server_conn = NULL;
        struct s2n_stuffer client_to_server = { 0 };
        struct s2n_stuffer server_to_client = { 0 };

        /* Craft a cipher preference with a cipher_idx cipher */
        EXPECT_MEMCPY_SUCCESS(&server_cipher_preferences, default_cipher_preferences, sizeof(server_cipher_preferences));
        server_cipher_preferences.count = 1;
        struct s2n_cipher_suite *cur_cipher = default_cipher_preferences->suites[cipher_idx];

        if (!cur_cipher->available) {
            /* Skip Ciphers that aren't supported with the linked libcrypto */
            continue;
        }

        server_cipher_preferences.suites = &cur_cipher;

        EXPECT_MEMCPY_SUCCESS(&server_security_policy, default_security_policy, sizeof(server_security_policy));
        server_security_policy.cipher_preferences = &server_cipher_preferences;

        config->security_policy = &server_security_policy;

        EXPECT_NOT_NULL(server_conn = s2n_connection_new(S2N_SERVER));
        EXPECT_SUCCESS(s2n_connection_set_config(server_conn, config));

        EXPECT_NOT_NULL(client_conn = s2n_connection_new(S2N_CLIENT));
        EXPECT_SUCCESS(s2n_connection_set_config(client_conn, config));

        /* Only set S2N_CERT_AUTH_REQUIRED on the server and not the client so that the connection fails */
        EXPECT_SUCCESS(s2n_connection_set_client_auth_type(server_conn, S2N_CERT_AUTH_REQUIRED));

        /* Set up our I/O callbacks. Use stuffers for the "I/O context" */
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&client_to_server, 0));
        EXPECT_SUCCESS(s2n_stuffer_growable_alloc(&server_to_client, 0));
        EXPECT_SUCCESS(s2n_connection_set_io_stuffers(&server_to_client, &client_to_server, client_conn));
        EXPECT_SUCCESS(s2n_connection_set_io_stuffers(&client_to_server, &server_to_client, server_conn));

        EXPECT_FAILURE(s2n_negotiate_test_server_and_client(server_conn, client_conn));

        /* Verify that NEITHER connections negotiated Mutual Auth */
        EXPECT_FALSE(s2n_connection_client_cert_used(server_conn));
        EXPECT_FALSE(s2n_connection_client_cert_used(client_conn));

        EXPECT_SUCCESS(s2n_connection_free(client_conn));
        EXPECT_SUCCESS(s2n_connection_free(server_conn));
        EXPECT_SUCCESS(s2n_stuffer_free(&server_to_client));
        EXPECT_SUCCESS(s2n_stuffer_free(&client_to_server));
    }

    /* Ensure that the client's certificate is validated, regardless of how client auth was enabled */
    {
        typedef enum {
            TEST_ENABLE_WITH_CONFIG,
            TEST_ENABLE_WITH_CONN_BEFORE_CONFIG,
            TEST_ENABLE_WITH_CONN_AFTER_CONFIG,
            TEST_COUNT,
        } test_case;

        for (test_case test = TEST_ENABLE_WITH_CONFIG; test < TEST_COUNT; test++) {
            DEFER_CLEANUP(struct s2n_config *client_config = s2n_config_new(), s2n_config_ptr_free);
            EXPECT_NOT_NULL(client_config);
            EXPECT_SUCCESS(s2n_config_set_cipher_preferences(client_config, "default_tls13"));
            EXPECT_SUCCESS(s2n_config_set_client_auth_type(client_config, S2N_CERT_AUTH_OPTIONAL));

            /* The client trusts the server's cert, and sends the same cert to the server. */
            EXPECT_SUCCESS(s2n_config_set_verification_ca_location(client_config, S2N_DEFAULT_TEST_CERT_CHAIN, NULL));
            EXPECT_SUCCESS(s2n_config_add_cert_chain_and_key_to_store(client_config, chain_and_key));

            DEFER_CLEANUP(struct s2n_config *server_config = s2n_config_new(), s2n_config_ptr_free);
            EXPECT_NOT_NULL(server_config);
            EXPECT_SUCCESS(s2n_config_set_cipher_preferences(server_config, "default_tls13"));
            struct host_verify_data verify_data_allow = { .allow = 1 };
            EXPECT_SUCCESS(s2n_config_set_verify_host_callback(server_config, verify_host_fn, &verify_data_allow));

            /* The server sends its cert, but does NOT trust the client's cert. This should always
             * cause certificate validation to fail on the server.
             */
            EXPECT_SUCCESS(s2n_config_add_cert_chain_and_key_to_store(server_config, chain_and_key));

            DEFER_CLEANUP(struct s2n_connection *server_conn = s2n_connection_new(S2N_SERVER),
                    s2n_connection_ptr_free);
            EXPECT_NOT_NULL(server_conn);
            EXPECT_SUCCESS(s2n_connection_set_blinding(server_conn, S2N_SELF_SERVICE_BLINDING));

            switch (test) {
                case TEST_ENABLE_WITH_CONFIG:
                    EXPECT_SUCCESS(s2n_config_set_client_auth_type(server_config, S2N_CERT_AUTH_REQUIRED));
                    EXPECT_SUCCESS(s2n_connection_set_config(server_conn, server_config));
                    break;
                case TEST_ENABLE_WITH_CONN_BEFORE_CONFIG:
                    EXPECT_SUCCESS(s2n_connection_set_client_auth_type(server_conn, S2N_CERT_AUTH_REQUIRED));
                    EXPECT_SUCCESS(s2n_connection_set_config(server_conn, server_config));
                    break;
                case TEST_ENABLE_WITH_CONN_AFTER_CONFIG:
                    EXPECT_SUCCESS(s2n_connection_set_config(server_conn, server_config));
                    EXPECT_SUCCESS(s2n_connection_set_client_auth_type(server_conn, S2N_CERT_AUTH_REQUIRED));
                    break;
                default:
                    FAIL_MSG("Invalid test case");
            }

            DEFER_CLEANUP(struct s2n_connection *client_conn = s2n_connection_new(S2N_CLIENT),
                    s2n_connection_ptr_free);
            EXPECT_NOT_NULL(client_conn);
            EXPECT_SUCCESS(s2n_connection_set_config(client_conn, client_config));
            EXPECT_SUCCESS(s2n_set_server_name(client_conn, "localhost"));

            DEFER_CLEANUP(struct s2n_test_io_pair io_pair = { 0 }, s2n_io_pair_close);
            EXPECT_SUCCESS(s2n_io_pair_init_non_blocking(&io_pair));
            EXPECT_SUCCESS(s2n_connection_set_io_pair(client_conn, &io_pair));
            EXPECT_SUCCESS(s2n_connection_set_io_pair(server_conn, &io_pair));

            EXPECT_FAILURE_WITH_ALERT(s2n_negotiate_test_server_and_client(server_conn, client_conn),
                    S2N_ERR_CERT_UNTRUSTED, S2N_TLS_ALERT_CERTIFICATE_UNKNOWN);

            /* Ensure that a client certificate was received on the server, indicating that the
             * validation error occurred when processing the client's certificate, rather than the
             * server's.
             */
            uint8_t *client_cert_chain = NULL;
            uint32_t client_cert_chain_len = 0;
            EXPECT_SUCCESS(s2n_connection_get_client_cert_chain(server_conn,
                    &client_cert_chain, &client_cert_chain_len));
            EXPECT_TRUE(client_cert_chain_len > 0);
        }
    }

    EXPECT_SUCCESS(s2n_cert_chain_and_key_free(chain_and_key));
    EXPECT_SUCCESS(s2n_config_free(config));
    free(cert_chain_pem);
    free(private_key_pem);
    free(dhparams_pem);
    END_TEST();
    return 0;
}
