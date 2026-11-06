#define _GNU_SOURCE 1
#include "bootstrap_http.h"
#include "gitstored/protocol.h"
#if defined(__linux__) && defined(GITSTORED_BOOTSTRAP)
#include "bootstrap_queries.inc"
#include "oidc_discovery.h"
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <linux/capability.h>
#include <openssl/crypto.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
typedef struct {
    unsigned char *bytes;
    size_t length, capacity;
    int overflow;
    long code;
    char type[128];
} response;
static void clear_response(response *value) {
    if (value->bytes)
        OPENSSL_cleanse(value->bytes, value->capacity);
    free(value->bytes);
    memset(value, 0, sizeof(*value));
}
static size_t receive(char *bytes, size_t size, size_t count, void *argument) {
    response *value = argument;
    if (size && count > SIZE_MAX / size) {
        value->overflow = 1;
        return 0;
    }
    size_t length = size * count;
    if (!length)
        return 0;
    if (length > GSTD_FRAME_MAX - value->length) {
        value->overflow = 1;
        return 0;
    }
    size_t needed = value->length + length + 1;
    if (needed > value->capacity) {
        size_t capacity = value->capacity ? value->capacity : 4096;
        while (capacity < needed)
            capacity = capacity > GSTD_FRAME_MAX / 2 ? GSTD_FRAME_MAX + 1 : capacity * 2;
        unsigned char *copy = realloc(value->bytes, capacity);
        if (!copy)
            return 0;
        value->bytes = copy;
        value->capacity = capacity;
    }
    memcpy(value->bytes + value->length, bytes, length);
    value->length += length;
    value->bytes[value->length] = 0;
    return length;
}
static int bounded(int resource, rlim_t maximum) {
    struct rlimit limit;
    if (getrlimit(resource, &limit))
        return -1;
    if (limit.rlim_cur > maximum)
        limit.rlim_cur = maximum;
    if (limit.rlim_max > maximum)
        limit.rlim_max = maximum;
    return setrlimit(resource, &limit);
}
static int worker(const gstd_bootstrap_http_context *context) {
    if (!context || context->control_owner <= 0 || context->control_owner == getpid() || !geteuid())
        return -1;
    struct rlimit core;
    if (getrlimit(RLIMIT_CORE, &core) || core.rlim_cur)
        return -1;
    DIR *tasks = opendir("/proc/self/task");
    if (!tasks)
        return -1;
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(tasks)))
        if (entry->d_name[0] != '.')
            count++;
    closedir(tasks);
    if (count != 1)
        return -1;
    if (bounded(RLIMIT_FSIZE, 67108864) || bounded(RLIMIT_NOFILE, 128))
        return -1;
#ifndef GITSTORED_INSTRUMENTED
    if (bounded(RLIMIT_AS, 1073741824))
        return -1;
#endif
    if (context->test_fixture) {
#ifdef GITSTORED_TESTING
        return context->network ? -1 : 0;
#else
        return -1;
#endif
    }
    if (!context->network || gstd_network_namespace_check(context->network) ||
        setns(context->network->user_fd, CLONE_NEWUSER) ||
        setns(context->network->network_fd, CLONE_NEWNET))
        return -1;
    for (unsigned cap = 0;; cap++) {
        int present = prctl(PR_CAPBSET_READ, cap, 0, 0, 0);
        if (present < 0) {
            if (errno != EINVAL)
                return -1;
            break;
        }
        if (present && prctl(PR_CAPBSET_DROP, cap, 0, 0, 0))
            return -1;
    }
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[2] = {{0}};
    return syscall(SYS_capset, &header, caps) ||
                   prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) ||
                   prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)
               ? -1
               : 0;
}
static int text(const json_t *value, const char *name, const char *expected) {
    const json_t *item = json_object_get(value, name);
    return json_is_string(item) && json_string_length(item) == strlen(expected) &&
           !memcmp(json_string_value(item), expected, strlen(expected));
}
static int media(const response *value, const char *expected) {
    size_t length = strlen(expected);
    return !strncasecmp(value->type, expected, length) &&
           (!value->type[length] || value->type[length] == ';');
}
static int transport(const char *url, const char *body, const char *token, const char *username,
                     const char *password, response *output) {
    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;
    struct curl_slist *headers = NULL;
    char *authorization = NULL;
    int options = 0;
#define OPTION(name, value)                                                                        \
    do {                                                                                           \
        if (curl_easy_setopt(curl, name, value) != CURLE_OK)                                       \
            options = 1;                                                                           \
    } while (0)
    OPTION(CURLOPT_URL, url);
    OPTION(CURLOPT_PROTOCOLS_STR, "http");
    OPTION(CURLOPT_REDIR_PROTOCOLS_STR, "http");
    OPTION(CURLOPT_FOLLOWLOCATION, 0L);
    OPTION(CURLOPT_PROXY, "");
    OPTION(CURLOPT_NOPROXY, "*");
    OPTION(CURLOPT_CONNECTTIMEOUT_MS, 500L);
    OPTION(CURLOPT_TIMEOUT_MS, 2000L);
    OPTION(CURLOPT_NOSIGNAL, 1L);
    OPTION(CURLOPT_VERBOSE, 0L);
    OPTION(CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    OPTION(CURLOPT_WRITEFUNCTION, receive);
    OPTION(CURLOPT_WRITEDATA, output);
    OPTION(CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)GSTD_FRAME_MAX);
    if (body) {
        headers = curl_slist_append(NULL, "Content-Type: application/json");
        if (!headers)
            options = 1;
        OPTION(CURLOPT_POSTFIELDS, body);
        OPTION(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body));
    }
    if (token) {
        if (asprintf(&authorization, "Authorization: Bearer %s", token) < 0)
            authorization = NULL;
        struct curl_slist *next = authorization ? curl_slist_append(headers, authorization) : NULL;
        if (!next)
            options = 1;
        else
            headers = next;
    }
    if (username && password) {
        OPTION(CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        OPTION(CURLOPT_USERNAME, username);
        OPTION(CURLOPT_PASSWORD, password);
    }
    if (headers)
        OPTION(CURLOPT_HTTPHEADER, headers);
    CURLcode status = options ? CURLE_FAILED_INIT : curl_easy_perform(curl);
    char *type = NULL;
    if (status == CURLE_OK &&
        (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &output->code) != CURLE_OK ||
         curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type) != CURLE_OK))
        status = CURLE_FAILED_INIT;
    if (type && strlen(type) < sizeof(output->type))
        memcpy(output->type, type, strlen(type) + 1);
    for (struct curl_slist *entry = headers; entry; entry = entry->next)
        if (entry->data)
            OPENSSL_cleanse(entry->data, strlen(entry->data));
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    if (authorization)
        OPENSSL_cleanse(authorization, strlen(authorization));
    free(authorization);
    return status == CURLE_OK ? 0 : status == CURLE_FILESIZE_EXCEEDED || output->overflow ? -2 : -1;
#undef OPTION
}
static size_t discard(char *bytes, size_t size, size_t count, void *argument) {
    (void)bytes;
    size_t *received = argument;
    if (size && count > SIZE_MAX / size)
        return 0;
    size_t length = size * count;
    if (length > GSTD_FRAME_MAX - *received)
        return 0;
    *received += length;
    return length;
}
/* Health consumers intentionally retain no response bytes. Their upstream
 * bodies are not a contract, and this worker must not relay them to a parent. */
static int transport_status(const char *url, long *code) {
    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;
    if (!code) {
        curl_easy_cleanup(curl);
        return -1;
    }
    size_t received = 0;
    int options = 0;
#define OPTION(name, value)                                                                        \
    do {                                                                                           \
        if (curl_easy_setopt(curl, name, value) != CURLE_OK)                                       \
            options = 1;                                                                           \
    } while (0)
    OPTION(CURLOPT_URL, url);
    OPTION(CURLOPT_PROTOCOLS_STR, "http");
    OPTION(CURLOPT_REDIR_PROTOCOLS_STR, "http");
    OPTION(CURLOPT_FOLLOWLOCATION, 0L);
    OPTION(CURLOPT_PROXY, "");
    OPTION(CURLOPT_NOPROXY, "*");
    OPTION(CURLOPT_CONNECTTIMEOUT_MS, 500L);
    OPTION(CURLOPT_TIMEOUT_MS, 2000L);
    OPTION(CURLOPT_NOSIGNAL, 1L);
    OPTION(CURLOPT_VERBOSE, 0L);
    OPTION(CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    OPTION(CURLOPT_WRITEFUNCTION, discard);
    OPTION(CURLOPT_WRITEDATA, &received);
    CURLcode result = options ? CURLE_FAILED_INIT : curl_easy_perform(curl);
    if (result == CURLE_OK && curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, code) != CURLE_OK)
        result = CURLE_FAILED_INIT;
    curl_easy_cleanup(curl);
    return result == CURLE_OK ? 0 : -1;
#undef OPTION
}
static json_t *json_response(const response *value) {
    return media(value, "application/json") ? gstd_json_parse(value->bytes, value->length) : NULL;
}
static int errors(const json_t *value) {
    const json_t *field = json_object_get(value, "errors");
    return field && (!json_is_array(field) || json_array_size(field));
}
static int valid_token(const json_t *value) {
    const char *bytes = json_string_value(value);
    size_t length = json_string_length(value);
    if (!bytes || !length || length > 16384 || strlen(bytes) != length)
        return 0;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)bytes[i] < 33 || (unsigned char)bytes[i] > 126)
            return 0;
    return 1;
}
static int uid_read(const json_t *value, char output[129]) {
    const char *bytes = json_string_value(value);
    size_t length = json_string_length(value);
    if (!bytes || !length || length > 128 || strlen(bytes) != length)
        return -1;
    for (size_t i = 0; i < length; i++)
        if (!((bytes[i] >= 'a' && bytes[i] <= 'z') || (bytes[i] >= 'A' && bytes[i] <= 'Z') ||
              (bytes[i] >= '0' && bytes[i] <= '9') || bytes[i] == '-'))
            return -1;
    memcpy(output, bytes, length + 1);
    return 0;
}
static gstd_bootstrap_http_status enroll(const gstd_credential *material, char uid[129]) {
    json_t *value = gstd_json_parse(material->bytes, material->length), *variables = NULL,
           *request = NULL, *login = NULL, *created = NULL;
    response reply = {0};
    char *body = NULL, *public_key = NULL;
    gstd_bootstrap_http_status result = GSTD_BOOT_HTTP_REJECTED;
    variables = value ? json_pack("{s:O,s:O}", "username", json_object_get(value, "username"),
                                  "password", json_object_get(value, "password"))
                      : NULL;
    request = variables
                  ? json_pack("{s:s,s:O}", "query", bootstrap_login_query, "variables", variables)
                  : NULL;
    json_decref(variables);
    variables = NULL;
    body = request ? gstd_json_dump(request) : NULL;
    if (!body)
        goto done;
    result = GSTD_BOOT_HTTP_UNCERTAIN;
    if (transport("http://127.0.0.1:4000/graphql", body, NULL, NULL, NULL, &reply))
        goto done;
    if (reply.code != 200) {
        result = reply.code == 401 || reply.code == 403 || reply.code == 429
                     ? GSTD_BOOT_HTTP_REJECTED
                     : GSTD_BOOT_HTTP_UNCERTAIN;
        goto done;
    }
    login = json_response(&reply);
    if (!login || errors(login))
        goto done;
    const json_t *token = json_object_get(
        json_object_get(json_object_get(json_object_get(login, "data"), "login"), "token"),
        "accessToken");
    if (!valid_token(token))
        goto done;
    clear_response(&reply);
    OPENSSL_cleanse(body, strlen(body));
    free(body);
    body = NULL;
    json_decref(request);
    request = NULL;
    public_key = gstd_bootstrap_controller_public(material);
    variables = public_key ? json_pack("{s:s,s:s,s:s}", "name", "gitstore-controller-manager",
                                       "keyID", "controller-key", "publicKey", public_key)
                           : NULL;
    request = variables
                  ? json_pack("{s:s,s:O}", "query", bootstrap_enroll_query, "variables", variables)
                  : NULL;
    json_decref(variables);
    variables = NULL;
    body = request ? gstd_json_dump(request) : NULL;
    if (!body) {
        result = GSTD_BOOT_HTTP_REJECTED;
        goto done;
    }
    if (transport("http://127.0.0.1:4000/graphql", body, json_string_value(token), NULL, NULL,
                  &reply))
        goto done;
    created = json_response(&reply);
    if (!created)
        goto done;
    const json_t *identity = json_object_get(
        json_object_get(json_object_get(json_object_get(json_object_get(created, "data"),
                                                        "createServiceAccount"),
                                        "serviceAccount"),
                        "metadata"),
        "uid");
    int known = !uid_read(identity, uid);
    if (reply.code != 200) {
        result = reply.code == 401 || reply.code == 403 || reply.code == 429
                     ? GSTD_BOOT_HTTP_REJECTED
                     : GSTD_BOOT_HTTP_UNCERTAIN;
    } else if (!errors(created) && known)
        result = GSTD_BOOT_HTTP_READY;
done:
    clear_response(&reply);
    if (body)
        OPENSSL_cleanse(body, strlen(body));
    free(body);
    free(public_key);
    json_decref(request);
    json_decref(variables);
    json_decref(value);
    json_decref(login);
    json_decref(created);
    return result;
}
gstd_bootstrap_http_status gstd_bootstrap_http(const gstd_bootstrap_http_context *context,
                                               gstd_bootstrap_http_action action,
                                               const gstd_credential *material, char uid[129]) {
    if (action < GSTD_BOOT_API_READY || action > GSTD_BOOT_CONTROLLER_READY || (uid && uid[0]) ||
        (action == GSTD_BOOT_CONTROLLER_ENROLL && !uid) ||
        ((action == GSTD_BOOT_CONTROLLER_ENROLL || action == GSTD_BOOT_GIT_READY) &&
         gstd_bootstrap_material_check(material)) ||
        worker(context))
        return GSTD_BOOT_HTTP_REJECTED;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return GSTD_BOOT_HTTP_REJECTED;
    if (action == GSTD_BOOT_CONTROLLER_ENROLL) {
        gstd_bootstrap_http_status result = enroll(material, uid);
        curl_global_cleanup();
        return result;
    }
    const char *url = action == GSTD_BOOT_API_READY ? "http://127.0.0.1:4000/ready"
                      : action == GSTD_BOOT_CONTROLLER_READY
                          ? "http://127.0.0.1:5001/health"
                          : "http://127.0.0.1:9000/gitstore-system/gitstore-system/info/"
                            "refs?service=git-upload-pack";
    json_t *credentials =
        action == GSTD_BOOT_GIT_READY ? gstd_json_parse(material->bytes, material->length) : NULL;
    response reply = {0};
    gstd_bootstrap_http_status result = GSTD_BOOT_HTTP_PENDING;
    int received = transport(
        url, NULL, NULL,
        credentials ? json_string_value(json_object_get(credentials, "username")) : NULL,
        credentials ? json_string_value(json_object_get(credentials, "password")) : NULL, &reply);
    json_t *value = NULL;
    if (received == -2) {
        result = GSTD_BOOT_HTTP_REJECTED;
        goto done;
    }
    if (received || reply.code == 503)
        goto done;
    result = GSTD_BOOT_HTTP_REJECTED;
    if (reply.code != 200 || reply.overflow)
        goto done;
    if (action == GSTD_BOOT_GIT_READY) {
        static const char prefix[] = "001e# service=git-upload-pack\n";
        if (media(&reply, "application/x-git-upload-pack-advertisement") &&
            reply.length >= sizeof(prefix) - 1 && !memcmp(reply.bytes, prefix, sizeof(prefix) - 1))
            result = GSTD_BOOT_HTTP_READY;
        goto done;
    }
    value = json_response(&reply);
    if (!value || !text(value, "version", "0.1.0-alpha.8"))
        goto done;
    if (action == GSTD_BOOT_API_READY) {
        if (text(value, "status", "degraded")) {
            result = GSTD_BOOT_HTTP_PENDING;
            goto done;
        }
        const json_t *checks = json_object_get(value, "checks");
        if (text(value, "status", "healthy") &&
            text(json_object_get(checks, "datastore"), "status", "healthy") &&
            text(json_object_get(checks, "resource_watch"), "status", "healthy"))
            result = GSTD_BOOT_HTTP_READY;
    } else {
        if (text(value, "status", "degraded") || json_is_false(json_object_get(value, "ready")) ||
            json_is_false(json_object_get(value, "credentialReady"))) {
            result = GSTD_BOOT_HTTP_PENDING;
            goto done;
        }
        if (!text(value, "status", "ok") || !json_is_true(json_object_get(value, "ready")) ||
            !json_is_true(json_object_get(value, "credentialReady")))
            goto done;
        const char *kinds[] = {"Namespace", "Repository", "CategoryTaxonomy", "Product"};
        const json_t *all = json_object_get(value, "kinds");
        for (size_t i = 0; i < 4; i++) {
            const json_t *kind = json_object_get(all, kinds[i]),
                         *poison = json_object_get(kind, "poisonItems");
            if (!json_is_true(json_object_get(kind, "registered")) ||
                !json_is_false(json_object_get(kind, "recovering")) ||
                !json_is_false(json_object_get(kind, "stalled")) || !json_is_integer(poison) ||
                json_integer_value(poison)) {
                result = GSTD_BOOT_HTTP_PENDING;
                goto done;
            }
        }
        result = GSTD_BOOT_HTTP_READY;
    }
done:
    json_decref(value);
    json_decref(credentials);
    clear_response(&reply);
    curl_global_cleanup();
    return result;
}
gstd_oidc_http_status gstd_oidc_http(const gstd_bootstrap_http_context *context,
                                     gstd_oidc_http_action action, const gstd_oidc_client *client,
                                     const gstd_credential *secret) {
    if (action < GSTD_OIDC_HTTP_READY || action > GSTD_OIDC_HTTP_INSPECT ||
        (action == GSTD_OIDC_HTTP_READY && (client || secret)) ||
        (action != GSTD_OIDC_HTTP_READY && gstd_oidc_client_check(client)) ||
        (action == GSTD_OIDC_HTTP_INSPECT && secret))
        return GSTD_OIDC_REJECTED;
    json_t *request =
        action == GSTD_OIDC_HTTP_CREATE ? gstd_oidc_client_request(client, secret) : NULL;
    char *body = request ? gstd_json_dump(request) : NULL;
    json_decref(request);
    gstd_oidc_http_status result = GSTD_OIDC_REJECTED;
    response reply = {0};
    json_t *value = NULL;
    int initialized = 0;
    if ((action == GSTD_OIDC_HTTP_CREATE && !body) || worker(context))
        goto finish;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        goto finish;
    initialized = 1;
    char url[160];
    int length = action == GSTD_OIDC_HTTP_INSPECT
                     ? snprintf(url, sizeof(url), "http://127.0.0.1:4445/admin/clients/%s",
                                client->client_id)
                     : snprintf(url, sizeof(url), "http://127.0.0.1:4445/%s",
                                action == GSTD_OIDC_HTTP_CREATE ? "admin/clients" : "health/ready");
    if (length < 1 || (size_t)length >= sizeof(url))
        goto finish;
    result = action == GSTD_OIDC_HTTP_CREATE ? GSTD_OIDC_UNCERTAIN : GSTD_OIDC_PENDING;
    int received = transport(url, body, NULL, NULL, NULL, &reply);
    if (action == GSTD_OIDC_HTTP_CREATE) {
        if (!received && reply.code == 201 && (value = json_response(&reply)) &&
            gstd_oidc_client_matches(client, value, secret))
            result = GSTD_OIDC_CLIENT_CREATED;
        goto finish;
    }
    if (received || reply.code == 503) {
        if (received == -2)
            result = GSTD_OIDC_REJECTED;
        goto finish;
    }
    result = GSTD_OIDC_REJECTED;
    if (reply.code != 200 || !(value = json_response(&reply)))
        goto finish;
    if (action == GSTD_OIDC_HTTP_READY ? text(value, "status", "ok")
                                       : gstd_oidc_client_matches(client, value, NULL))
        result = action == GSTD_OIDC_HTTP_READY ? GSTD_OIDC_PROVIDER_READY : GSTD_OIDC_PUBLIC_MATCH;
finish:
    json_decref(value);
    if (body)
        OPENSSL_cleanse(body, strlen(body));
    free(body);
    clear_response(&reply);
    if (initialized)
        curl_global_cleanup();
    return result;
}
static gstd_oidc_health_status issuer_health(const gstd_bootstrap_http_context *context,
                                             const gstd_oidc_ports *ports) {
    if (worker(context) || curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return GSTD_OIDC_HEALTH_REJECTED;
    gstd_oidc_health_status result = GSTD_OIDC_HEALTH_PENDING;
    const char *paths[] = {"openid-configuration", "jwks.json"};
    for (unsigned i = 0; i < 2; i++) {
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%u/.well-known/%s", ports->issuer, paths[i]);
        response reply = {0};
        int received = transport(url, NULL, NULL, NULL, NULL, &reply);
        json_t *value = !received && reply.code == 200 ? json_response(&reply) : NULL;
        int valid = value &&
                    (i ? gstd_oidc_jwks_matches(value) : gstd_oidc_discovery_matches(value, ports));
        int malformed = received == -2 || (!received && reply.code == 200 && !valid);
        json_decref(value);
        clear_response(&reply);
        if (!valid) {
            result = malformed ? GSTD_OIDC_HEALTH_REJECTED : GSTD_OIDC_HEALTH_PENDING;
            goto done;
        }
    }
    result = GSTD_OIDC_HEALTH_READY;
done:
    curl_global_cleanup();
    return result;
}
gstd_oidc_health_status gstd_oidc_health(const gstd_bootstrap_http_context *context,
                                         gstd_oidc_health_role role, const gstd_oidc_ports *ports) {
    if (role < GSTD_OIDC_HEALTH_HYDRA || role > GSTD_OIDC_HEALTH_ISSUER ||
        gstd_oidc_ports_check(ports))
        return GSTD_OIDC_HEALTH_REJECTED;
    if (role == GSTD_OIDC_HEALTH_ISSUER)
        return issuer_health(context, ports);
    if (role == GSTD_OIDC_HEALTH_HYDRA) {
        gstd_oidc_http_status status = gstd_oidc_http(context, GSTD_OIDC_HTTP_READY, NULL, NULL);
        return status == GSTD_OIDC_PROVIDER_READY ? GSTD_OIDC_HEALTH_READY
               : status == GSTD_OIDC_REJECTED     ? GSTD_OIDC_HEALTH_REJECTED
                                                  : GSTD_OIDC_HEALTH_PENDING;
    }
    if (worker(context))
        return GSTD_OIDC_HEALTH_REJECTED;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return GSTD_OIDC_HEALTH_REJECTED;
    char url[128];
    int length = role == GSTD_OIDC_HEALTH_KRATOS
                     ? snprintf(url, sizeof(url), "http://127.0.0.1:4434/admin/health/ready")
                 : role == GSTD_OIDC_HEALTH_BRIDGE
                     ? snprintf(url, sizeof(url), "http://127.0.0.1:%u/health", ports->bridge)
                 : role == GSTD_OIDC_HEALTH_UI
                     ? snprintf(url, sizeof(url), "http://127.0.0.1:%u/health/ready", ports->ui)
                     : snprintf(url, sizeof(url), "http://127.0.0.1:%u/readyz", ports->mail);
    long code = 0;
    int received = length < 1 || (size_t)length >= sizeof(url) || transport_status(url, &code);
    curl_global_cleanup();
    return !received && code == 200 ? GSTD_OIDC_HEALTH_READY : GSTD_OIDC_HEALTH_PENDING;
}
#else
gstd_bootstrap_http_status gstd_bootstrap_http(const gstd_bootstrap_http_context *context,
                                               gstd_bootstrap_http_action action,
                                               const gstd_credential *material, char uid[129]) {
    (void)context;
    (void)action;
    (void)material;
    (void)uid;
    return GSTD_BOOT_HTTP_REJECTED;
}
gstd_oidc_http_status gstd_oidc_http(const gstd_bootstrap_http_context *context,
                                     gstd_oidc_http_action action, const gstd_oidc_client *client,
                                     const gstd_credential *secret) {
    (void)context;
    (void)action;
    (void)client;
    (void)secret;
    return GSTD_OIDC_REJECTED;
}
gstd_oidc_health_status gstd_oidc_health(const gstd_bootstrap_http_context *context,
                                         gstd_oidc_health_role role, const gstd_oidc_ports *ports) {
    (void)context;
    (void)role;
    (void)ports;
    return GSTD_OIDC_HEALTH_REJECTED;
}
#endif
