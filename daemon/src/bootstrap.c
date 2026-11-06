#define _GNU_SOURCE 1
#include "bootstrap.h"
#include "bootstrap_material.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "oidc_profile.h"
#include <string.h>
int gstd_bootstrap_graph_begin(const json_t *manifest, const char *datastore, uint64_t generation,
                               gstd_bootstrap_graph *graph) {
    int persistent = datastore && !strcmp(datastore, "scylladb");
    const char *manifest_datastore =
        manifest ? json_string_value(json_object_get(manifest, "datastore")) : NULL;
    if (!graph || !datastore || (!persistent && strcmp(datastore, "memdb")) ||
        (persistent ? (!manifest_datastore || strcmp(manifest_datastore, "scylladb"))
                    : manifest_datastore != NULL) ||
        !generation || generation > UINT64_C(9007199254740991) ||
        gstd_asset_manifest_check(manifest))
        return -1;
    unsigned api = 0, git = 0, controller = 0, datastore_image = 0;
    json_t *images = json_object_get(manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        const char *component =
            json_string_value(json_object_get(json_array_get(images, i), "component"));
        if (!strcmp(component, "api"))
            api = 1;
        else if (!strcmp(component, "git"))
            git = 1;
        else if (!strcmp(component, "controller"))
            controller = 1;
        else if (!strcmp(component, "scylladb") && persistent)
            datastore_image = 1;
        else if (!gstd_oidc_component(component))
            return -1;
    }
    if (!api || !git || datastore_image != (unsigned)persistent)
        return -1;
    *graph = (gstd_bootstrap_graph){.generation = generation,
                                    .stage = GSTD_BOOT_PREPARE,
                                    .controller = controller,
                                    .persistent = (unsigned)persistent};
    return 0;
}
int gstd_bootstrap_graph_persistent_begin(const json_t *manifest, uint64_t generation, int fresh,
                                          gstd_bootstrap_graph *graph) {
    if ((fresh != 0 && fresh != 1) ||
        gstd_bootstrap_graph_begin(manifest, "scylladb", generation, graph))
        return -1;
    graph->fresh_datastore = (unsigned)fresh;
    return 0;
}
int gstd_bootstrap_graph_observe(gstd_bootstrap_graph *graph, uint64_t generation,
                                 gstd_bootstrap_stage stage, int result, const char *uid) {
    if (!graph || generation != graph->generation || !generation || graph->outcome ||
        graph->stage != stage || stage < GSTD_BOOT_PREPARE || stage >= GSTD_BOOT_STAGE_COUNT ||
        stage == GSTD_BOOT_COMPLETE ||
        (stage >= GSTD_BOOT_LAUNCH_DATASTORE && !graph->persistent) || result < -2 || result > 1 ||
        (result == 1 && stage != GSTD_BOOT_WAIT_API && stage != GSTD_BOOT_WAIT_GIT &&
         stage != GSTD_BOOT_WAIT_CONTROLLER && stage != GSTD_BOOT_WAIT_DATASTORE) ||
        (uid && *uid && (stage != GSTD_BOOT_ENROLL || strnlen(uid, 129) > 128)) ||
        (stage == GSTD_BOOT_ENROLL && graph->controller_uid[0] && uid &&
         strcmp(graph->controller_uid, uid)) ||
        (stage == GSTD_BOOT_ENROLL && result == 0 && (!uid || !*uid)))
        return -1;
    if (uid)
        for (const unsigned char *p = (const unsigned char *)uid; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '-'))
                return -1;
    if (stage == GSTD_BOOT_ENROLL && uid && *uid)
        memmove(graph->controller_uid, uid, strlen(uid) + 1);
    if (result < 0) {
        graph->outcome = result;
        return 0;
    }
    if (result == 1)
        return 0;
    if (stage == GSTD_BOOT_PREPARE && graph->persistent)
        graph->stage = GSTD_BOOT_LAUNCH_DATASTORE;
    else if (stage == GSTD_BOOT_LAUNCH_DATASTORE)
        graph->stage = GSTD_BOOT_WAIT_DATASTORE;
    else if (stage == GSTD_BOOT_WAIT_DATASTORE)
        graph->stage = graph->fresh_datastore ? GSTD_BOOT_PREPARE_KEYSPACE : GSTD_BOOT_LAUNCH_GIT;
    else if (stage == GSTD_BOOT_PREPARE_KEYSPACE)
        graph->stage = GSTD_BOOT_MIGRATE_SCHEMA;
    else if (stage == GSTD_BOOT_MIGRATE_SCHEMA)
        graph->stage = GSTD_BOOT_LAUNCH_GIT;
    else
        graph->stage =
            stage == GSTD_BOOT_WAIT_GIT && !graph->controller ? GSTD_BOOT_COMPLETE : stage + 1;
    return 0;
}
#include "native_exit.h"
#if defined(__linux__) && defined(GITSTORED_BOOTSTRAP)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
static const char api_ports[] = "[api]\nport = 4000\ngit_port = 9000\ngrpc_port = 6000\n";
static const char api_common[] =
    "[api.git_service]\nuri = \"dns:///127.0.0.1:50051\"\n"
    "[api.auth.static_users]\nusers_file = \"/etc/gitstore/users.yaml\"\n"
    "[api.auth.authz]\nprovider = \"rbac-local\"\n"
    "[api.auth.userdir]\nprovider = \"static-users\"\n"
    "[api.auth.rbac_local]\npolicy_file = \"/etc/gitstore/policy.yaml\"\n"
    "[observability.otlp]\nuri = \"\"\n[log]\nlevel = \"info\"\nformat = "
    "\"json\"\n";
static const char api_chain[] = "[api.auth.authn]\nchain = [\"static-users\", "
                                "\"serviceaccount-assertion\", "
                                "\"serviceaccount-jwt\", \"anonymous\"]\n";
static const char api_oidc_chain[] = "[api.auth.authn]\nchain = [\"static-users\", "
                                     "\"serviceaccount-assertion\", "
                                     "\"serviceaccount-jwt\", \"oidc-jwt\", \"anonymous\"]\n";
static const char controller_base[] =
    "[controller]\nport = 5001\napi_uri = \"http://127.0.0.1:4000/graphql\"\n"
    "[controller.serviceaccount]\nname = \"gitstore-controller-manager\"\n"
    "key_ref = {kind = \"SecretRef\", name = \"controller-signing\"}\n"
    "assertion_audience = "
    "\"gitstore-api/serviceaccount-token\"\naccess_token_audience = "
    "\"gitstore-api\"\n"
    "[controller.secret_providers.bootstrap]\ntype = \"file\"\nbase_path = "
    "\"/run/gitstore\"\n"
    "[controller.checkpoint]\ndir = "
    "\"/tmp/checkpoints\"\nflush_interval_events = 100\n"
    "[observability.otlp]\nuri = \"\"\n[log]\nlevel = \"info\"\nformat = "
    "\"json\"\n";
/* Versioned public RBAC vocabulary from the pinned upstream policy. Only the
 * bootstrap admin, controller and API-internal roles required by core are
 * bound. */
static const char policy[] =
    "apiVersion: rbac.authorization.gitstore.dev/v1beta1\nkind: "
    "RbacLocalConfiguration\ndefaultDeny: true\n"
    "clusterRoles:\n  cluster-admin:\n    rules:\n      - resources: ['*']\n   "
    "     verbs: ['*']\n"
    "  controller:\n    rules:\n      - resources: [namespace, repository, "
    "categoryTaxonomy, "
    "product, file]\n"
    "        verbs: [read, list, watch]\n"
    "      - resources: [namespace/status, repository/status, "
    "categoryTaxonomy/status, "
    "product/status, file/status]\n"
    "        verbs: [write]\n      - resources: [namespace, repository, "
    "categoryTaxonomy, "
    "product]\n        verbs: [purge]\n"
    "      - resources: [namespace/system-repository]\n        verbs: "
    "[provision]\n"
    "      - resources: [repository]\n        verbs: [create]\n"
    "      - resources: [product/management]\n        verbs: [read, list]\n"
    "  controller-token:\n    rules:\n      - resources: "
    "[serviceAccount/token]\n        verbs: "
    "[issue]\n"
    "        resourceNames: [gitstore-controller-manager]\n"
    "  system:api-internal:\n    rules:\n      - resources: [repository]\n     "
    "   verbs: [create, "
    "delete]\n"
    "      - resources: [repository/contents]\n        verbs: [read, "
    "write]\nroles: "
    "{}\nclusterRoleBindings:\n"
    "  - roleRef: {kind: ClusterRole, name: cluster-admin}\n    subjects: "
    "[{kind: User, name: "
    "admin}]\n"
    "  - roleRef: {kind: ClusterRole, name: controller}\n    subjects: [{kind: "
    "ServiceAccount, "
    "name: gitstore-controller-manager}]\n"
    "  - roleRef: {kind: ClusterRole, name: controller-token}\n    subjects: "
    "[{kind: "
    "ServiceAccount, name: gitstore-controller-manager}]\n"
    "  - roleRef: {kind: ClusterRole, name: 'system:api-internal'}\n    "
    "subjects: [{kind: User, "
    "name: 'system:api'}]\n"
    "roleBindings: []\n";
static int scope(int fd, struct stat *info) {
    return fd >= 3 && !fstat(fd, info) && S_ISDIR(info->st_mode) && info->st_uid == geteuid() &&
           (info->st_mode & 07777) == 0700;
}
static int inventory(int fd, int empty) {
    static const char *const allowed[] = {"prepare.lock",
                                          "owner.json",
                                          "prepared.json",
                                          "git.toml",
                                          "api-base.toml",
                                          "api-owned.toml",
                                          "api-validator.toml",
                                          "users.yaml",
                                          "policy.yaml",
                                          "controller-base.toml",
                                          "controller-signing.json",
                                          "controller-owned.toml",
                                          "controller-identity.json"};
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    DIR *directory = copy >= 0 ? fdopendir(copy) : NULL;
    if (!directory) {
        if (copy >= 0)
            close(copy);
        return -1;
    }
    rewinddir(directory);
    struct dirent *entry;
    int valid = 1;
    errno = 0;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (empty) {
            valid = 0;
            break;
        }
        int known = 0;
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
            if (!strcmp(entry->d_name, allowed[i]))
                known = 1;
        if (!known) {
            valid = 0;
            break;
        }
    }
    if (errno)
        valid = 0;
    closedir(directory);
    return valid ? 0 : -1;
}
static int exact_file(int root, const char *name, const char *bytes, int create) {
    size_t length = strlen(bytes);
    if (!length || length > 32768)
        return -1;
    int fd = openat(root, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd >= 0) {
        struct stat before, after;
        unsigned char buffer[32768];
        int valid = !fstat(fd, &before) && S_ISREG(before.st_mode) && before.st_uid == geteuid() &&
                    before.st_nlink == 1 && (before.st_mode & 07777) == 0600 &&
                    before.st_size == (off_t)length;
        ssize_t count = valid ? pread(fd, buffer, length, 0) : -1;
        valid = valid && count == (ssize_t)length && !CRYPTO_memcmp(buffer, bytes, length) &&
                !fstat(fd, &after) && before.st_dev == after.st_dev &&
                before.st_ino == after.st_ino && before.st_size == after.st_size &&
                before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
                before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
                before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
                before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
        OPENSSL_cleanse(buffer, sizeof(buffer));
        close(fd);
        return valid ? 0 : -1;
    }
    if (errno != ENOENT || !create)
        return -1;
    fd =
        openat(root, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0)
        return -1;
    struct stat info;
    int valid = !fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
                info.st_nlink == 1 && (info.st_mode & 07777) == 0600;
    size_t sent = 0;
    while (valid && sent < length) {
        ssize_t count = write(fd, bytes + sent, length - sent);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            valid = 0;
        else
            sent += (size_t)count;
    }
    valid = valid && !fsync(fd);
    close(fd);
    return valid && !fsync(root) ? 0 : -1;
}
static char *quote(const char *text) {
    json_t *value = json_string(text);
    size_t length = value ? json_dumpb(value, NULL, 0, JSON_COMPACT | JSON_ENCODE_ANY) : 0;
    char *bytes = length && length <= 4096 ? malloc(length + 1) : NULL;
    if (bytes && json_dumpb(value, bytes, length, JSON_COMPACT | JSON_ENCODE_ANY) == length)
        bytes[length] = 0;
    else {
        free(bytes);
        bytes = NULL;
    }
    json_decref(value);
    return bytes;
}
static char *base64(const char *text) {
    size_t length = strlen(text);
    if (length > 1024)
        return NULL;
    char *bytes = malloc(4 * ((length + 2) / 3) + 1);
    if (bytes &&
        EVP_EncodeBlock((unsigned char *)bytes, (const unsigned char *)text, (int)length) < 0) {
        free(bytes);
        return NULL;
    }
    return bytes;
}
static int identifier(const char *text) { return text && gstd_identifier(text, strnlen(text, 65)); }
static int store_id(const char *text) {
    if (!text || strnlen(text, 33) != 32)
        return 0;
    for (size_t i = 0; i < 32; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return 0;
    return 1;
}
static int datastore_matches(const gstd_bootstrap_request *request) {
    if (!request || !request->datastore)
        return 0;
    const char *registered = json_string_value(json_object_get(request->manifest, "datastore"));
    return !strcmp(request->datastore, "memdb") ? registered == NULL
                                                : !strcmp(request->datastore, "scylladb") &&
                                                      registered && !strcmp(registered, "scylladb");
}
static int host_platform(const json_t *manifest) {
#if defined(__aarch64__)
    const char *expected = "arm64";
#elif defined(__x86_64__)
    const char *expected = "amd64";
#else
    (void)manifest;
    return 0;
#endif
#if defined(__aarch64__) || defined(__x86_64__)
    const char *actual =
        json_string_value(json_object_get(json_object_get(manifest, "platform"), "architecture"));
    return actual && !strcmp(actual, expected);
#endif
}
int gstd_bootstrap_prepare(const gstd_bootstrap_request *request, const gstd_credential *material) {
    struct stat root, lock_info, marker;
    struct rlimit limit;
    if (!datastore_matches(request) || !store_id(request->store_id) ||
        !identifier(request->deployment_id) || !identifier(request->operation_id) ||
        !request->generation || request->generation > UINT64_C(9007199254740991) ||
        request->control_owner <= 0 || request->control_owner == getpid() || !geteuid() ||
        getrlimit(RLIMIT_CORE, &limit) || limit.rlim_cur ||
        gstd_asset_manifest_check(request->manifest) || !host_platform(request->manifest) ||
        !scope(request->scope_fd, &root) || inventory(request->scope_fd, 0) ||
        gstd_bootstrap_material_check(material))
        return -1;
    int has_api = 0, has_git = 0, has_controller = 0;
    json_t *images = json_object_get(request->manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        const char *component =
            json_string_value(json_object_get(json_array_get(images, i), "component"));
        if (!strcmp(component, "api"))
            has_api = 1;
        else if (!strcmp(component, "git"))
            has_git = 1;
        else if (!strcmp(component, "controller"))
            has_controller = 1;
    }
    if (!has_api || !has_git)
        return -1;
    int oidc = json_object_get(request->manifest, "oidcProfileDigest") != NULL;
    if (oidc ? (gstd_oidc_core_manifest_check(request->manifest) ||
                request->oidc_issuer_port < 1024 || request->oidc_issuer_port > 65535 ||
                gstd_oidc_client_check(request->oidc_client))
             : (request->oidc_issuer_port || request->oidc_client))
        return -1;
    int exists = !fstatat(request->scope_fd, "owner.json", &marker, AT_SYMLINK_NOFOLLOW);
    if (!exists && errno != ENOENT)
        return -1;
    int completed = !fstatat(request->scope_fd, "prepared.json", &marker, AT_SYMLINK_NOFOLLOW);
    if ((!completed && errno != ENOENT) || exists != completed ||
        (!exists && inventory(request->scope_fd, 1)))
        return -1;
    int lock = openat(
        request->scope_fd, "prepare.lock",
        O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (exists ? 0 : O_CREAT | O_EXCL), 0600);
    if (lock < 0)
        return -1;
    if (fstat(lock, &lock_info) || !S_ISREG(lock_info.st_mode) || lock_info.st_uid != geteuid() ||
        lock_info.st_nlink != 1 || lock_info.st_size || (lock_info.st_mode & 07777) != 0600 ||
        flock(lock, LOCK_EX | LOCK_NB)) {
        close(lock);
        return -1;
    }
    struct stat named_lock;
    if (fstatat(request->scope_fd, "prepare.lock", &named_lock, AT_SYMLINK_NOFOLLOW) ||
        named_lock.st_dev != lock_info.st_dev || named_lock.st_ino != lock_info.st_ino) {
        close(lock);
        return -1;
    }
    int result = -1;
    char *owner_bytes = NULL, *api = NULL, *git = NULL, *users = NULL, *signing = NULL,
         *validator = NULL, *api_base = NULL, *oidc_config = NULL;
    char *jwt = NULL, *hmac = NULL, *issuer = NULL, *hash = NULL, *key = NULL, *key_id = NULL;
    json_t *value = gstd_json_parse(material->bytes, material->length), *owner = NULL,
           *record = NULL;
    unsigned char digest[32];
    unsigned digest_length = 0;
    char seed[72] = "sha256:";
    if (EVP_Digest(material->bytes, material->length, digest, &digest_length, EVP_sha256(), NULL) !=
            1 ||
        digest_length != 32)
        goto done;
    for (size_t i = 0; i < 32; i++)
        snprintf(seed + 7 + 2 * i, 3, "%02x", digest[i]);
    char root_device[32], root_inode[32], lock_device[32], lock_inode[32];
    snprintf(root_device, sizeof(root_device), "%llu", (unsigned long long)root.st_dev);
    snprintf(root_inode, sizeof(root_inode), "%llu", (unsigned long long)root.st_ino);
    snprintf(lock_device, sizeof(lock_device), "%llu", (unsigned long long)lock_info.st_dev);
    snprintf(lock_inode, sizeof(lock_inode), "%llu", (unsigned long long)lock_info.st_ino);
    owner = json_pack("{s:i,s:s,s:s,s:s,s:I,s:s,s:s,s:s,s:s,s:s,s:s}", "schemaVersion", 1,
                      "storeId", request->store_id, "deploymentId", request->deployment_id,
                      "operationId", request->operation_id, "generation",
                      (json_int_t)request->generation, "profileDigest",
                      json_string_value(json_object_get(request->manifest, "profileDigest")),
                      "materialDigest", seed, "rootDevice", root_device, "rootInode", root_inode,
                      "lockDevice", lock_device, "lockInode", lock_inode);
    int persistent = !strcmp(request->datastore, "scylladb");
    if (persistent && owner &&
        json_object_set(owner, "manifestDigest",
                        json_object_get(request->manifest, "manifestDigest")))
        goto done;
    char oidc_issuer[64];
    if (oidc) {
        snprintf(oidc_issuer, sizeof(oidc_issuer), "http://127.0.0.1:%u/",
                 request->oidc_issuer_port);
        json_t *mapping =
            json_pack("{s:s,s:s,s:s}", "issuerURI", oidc_issuer, "clientId",
                      request->oidc_client->client_id, "audience", request->oidc_client->audience);
        int bad = !owner || !mapping || json_object_set(owner, "oidc", mapping);
        json_decref(mapping);
        if (bad || asprintf(&oidc_config,
                            "[api.auth.oidc_jwt]\nissuer_uri = \"%s\"\nclient_id = "
                            "\"%s\"\naudience = \"%s\"\n",
                            oidc_issuer, request->oidc_client->client_id,
                            request->oidc_client->audience) < 0)
            goto done;
    }
    const char *datastore_config = persistent ? "[api.datastore]\nbackend = "
                                                "\"scylla\"\n[api.datastore.scylla]\nauto_migrate "
                                                "= false\n"
                                                "hosts = [\"127.0.0.1:9042\"]\nkeyspace = "
                                                "\"gitstore\"\ndisable_shard_aware_port = true\n"
                                              : "[api.datastore]\nbackend = \"memdb\"\n";
    if (asprintf(&api_base, "%s%s%s%s%s", api_ports, datastore_config,
                 oidc ? api_oidc_chain : api_chain, api_common, oidc ? oidc_config : "") < 0)
        goto done;
    owner_bytes = owner ? gstd_json_dump(owner) : NULL;
    jwt = quote(json_string_value(json_object_get(value, "jwtSecret")));
    hmac = quote(json_string_value(json_object_get(value, "hmacSecret")));
    issuer = quote(json_string_value(json_object_get(value, "apiIssuerPEM")));
    hash = quote(json_string_value(json_object_get(value, "passwordHash")));
    if (!owner_bytes || !jwt || !hmac || !issuer || !hash ||
        exact_file(request->scope_fd, "owner.json", owner_bytes, !exists))
        goto done;
    if (asprintf(&api,
                 "[api.auth.jwt]\nsecret = %s\nttl = \"24h\"\nissuer = "
                 "\"gitstore\"\nrefresh_grace "
                 "= \"60s\"\n"
                 "[api.auth.serviceaccount]\nissuer = \"gitstore\"\naudience = "
                 "\"gitstore-api\"\n"
                 "assertion_audience = "
                 "\"gitstore-api/serviceaccount-token\"\nsigning_key = "
                 "%s\n[grpc_auth]\nhmac_secret = %s\n",
                 jwt, issuer, hmac) < 0 ||
        asprintf(&git,
                 "[git_service]\ngrpc_port = 50051\ndata_dir = "
                 "\"/var/lib/gitstore/repositories\"\n"
                 "[git_service.catalog]\nuri = "
                 "\"dns:///127.0.0.1:6000\"\n[git_service.validation]\ntimeout = "
                 "\"10s\"\n"
                 "[git_service.admission]\nbranch_pattern = "
                 "\"^refs/heads/main$\"\n[grpc_auth]\nhmac_secret = %s\n"
                 "[log]\nlevel = \"info\"\nformat = \"json\"\n",
                 hmac) < 0 ||
        asprintf(&users,
                 "version: v1\nusers:\n  - username: admin\n    password_hash: %s\n"
                 "    display_name: Local administrator\n    email: admin@localhost\n",
                 hash) < 0 ||
        asprintf(&validator, "%s%s", api_base, api) < 0)
        goto done;
    if (exact_file(request->scope_fd, "api-base.toml", api_base, !exists) ||
        exact_file(request->scope_fd, "api-owned.toml", api, !exists) ||
        exact_file(request->scope_fd, "git.toml", git, !exists) ||
        exact_file(request->scope_fd, "api-validator.toml", validator, !exists) ||
        exact_file(request->scope_fd, "users.yaml", users, !exists) ||
        exact_file(request->scope_fd, "policy.yaml", policy, !exists))
        goto done;
    if (has_controller) {
        key = base64(json_string_value(json_object_get(value, "controllerPEM")));
        key_id = base64("controller-key");
        record = key && key_id
                     ? json_pack("{s:s,s:{s:s,s:s}}", "format", "serviceaccount-signing-key/v1",
                                 "values", "privateKey", key, "keyID", key_id)
                     : NULL;
        signing = record ? gstd_json_dump(record) : NULL;
        if (!signing ||
            exact_file(request->scope_fd, "controller-base.toml", controller_base, !exists) ||
            exact_file(request->scope_fd, "controller-signing.json", signing, !exists))
            goto done;
    }
    if (exact_file(request->scope_fd, "prepared.json", owner_bytes, !exists))
        goto done;
    result = 0;
done: {
    char *sensitive[] = {api,    git,  users, signing, validator, jwt,        hmac,
                         issuer, hash, key,   key_id,  api_base,  oidc_config};
    for (size_t i = 0; i < sizeof(sensitive) / sizeof(sensitive[0]); i++)
        if (sensitive[i]) {
            OPENSSL_cleanse(sensitive[i], strlen(sensitive[i]));
            free(sensitive[i]);
        }
}
    free(owner_bytes);
    json_decref(value);
    json_decref(owner);
    json_decref(record);
    close(lock);
    return result;
}
int gstd_bootstrap_controller_bind(const gstd_bootstrap_request *request,
                                   const gstd_credential *material, const char *uid, pid_t api_pid,
                                   uint64_t api_start, const char *api_boot) {
    struct stat prepared;
    if (!request || !uid || !*uid || strnlen(uid, 129) > 128 || api_pid <= 0 || !api_start ||
        !api_boot || fstatat(request->scope_fd, "owner.json", &prepared, AT_SYMLINK_NOFOLLOW) ||
        fstatat(request->scope_fd, "prepared.json", &prepared, AT_SYMLINK_NOFOLLOW))
        return -1;
    for (const unsigned char *p = (const unsigned char *)uid; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-'))
            return -1;
    char boot[37];
    uint64_t start;
    if (gstd_native_boot_id(boot) || strcmp(api_boot, boot) ||
        gstd_native_process_start(api_pid, &start) || start != api_start ||
        gstd_bootstrap_prepare(request, material))
        return -1;
    int selected = 0;
    json_t *images = json_object_get(request->manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++)
        if (!strcmp(json_string_value(json_object_get(json_array_get(images, i), "component")),
                    "controller"))
            selected = 1;
    if (!selected)
        return -1;
    int lock = openat(request->scope_fd, "prepare.lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat info, named, overlay, identity;
    int valid = lock >= 3 && !fstat(lock, &info) && S_ISREG(info.st_mode) &&
                info.st_uid == geteuid() && info.st_nlink == 1 && info.st_size == 0 &&
                (info.st_mode & 07777) == 0600 && !flock(lock, LOCK_EX | LOCK_NB) &&
                !fstatat(request->scope_fd, "prepare.lock", &named, AT_SYMLINK_NOFOLLOW) &&
                info.st_dev == named.st_dev && info.st_ino == named.st_ino;
    if (!valid) {
        if (lock >= 0)
            close(lock);
        return -1;
    }
    int has_overlay =
        !fstatat(request->scope_fd, "controller-owned.toml", &overlay, AT_SYMLINK_NOFOLLOW);
    valid = has_overlay || errno == ENOENT;
    int has_identity =
        !fstatat(request->scope_fd, "controller-identity.json", &identity, AT_SYMLINK_NOFOLLOW);
    valid = valid && (has_identity || errno == ENOENT) && has_overlay == has_identity;
    char *text = NULL, *record_bytes = NULL;
    json_t *record = json_pack(
        "{s:i,s:s,s:s,s:s,s:I,s:s,s:s,s:I,s:I,s:s}", "schemaVersion", 1, "storeId",
        request->store_id, "deploymentId", request->deployment_id, "operationId",
        request->operation_id, "generation", (json_int_t)request->generation, "profileDigest",
        json_string_value(json_object_get(request->manifest, "profileDigest")), "uid", uid,
        "apiPid", (json_int_t)api_pid, "apiStart", (json_int_t)api_start, "apiBoot", api_boot);
    record_bytes = record ? gstd_json_dump(record) : NULL;
    if (valid && record_bytes &&
        asprintf(&text, "[controller.serviceaccount]\nuid = \"%s\"\n", uid) >= 0 &&
        !gstd_native_process_start(api_pid, &start) && start == api_start &&
        !exact_file(request->scope_fd, "controller-owned.toml", text, !has_overlay) &&
        !exact_file(request->scope_fd, "controller-identity.json", record_bytes, !has_identity))
        valid = 1;
    else
        valid = 0;
    free(text);
    free(record_bytes);
    json_decref(record);
    close(lock);
    return valid ? 0 : -1;
}
int gstd_bootstrap_provision(const gstd_bootstrap_request *request, int broker,
                             const json_t *source, int initial) {
    struct stat root;
    struct rlimit limit;
    if (!datastore_matches(request) || broker < 3 || (initial != 0 && initial != 1) ||
        request->control_owner <= 0 || request->control_owner == getpid() || !geteuid() ||
        getrlimit(RLIMIT_CORE, &limit) || limit.rlim_cur || !store_id(request->store_id) ||
        !identifier(request->deployment_id) || !identifier(request->operation_id) ||
        !scope(request->scope_fd, &root) || gstd_asset_manifest_check(request->manifest) ||
        !host_platform(request->manifest))
        return -1;
    if (!request->generation || request->generation > UINT64_C(9007199254740991) ||
        inventory(request->scope_fd, 0))
        return -1;
    int has_api = 0, has_git = 0;
    json_t *images = json_object_get(request->manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        const char *component =
            json_string_value(json_object_get(json_array_get(images, i), "component"));
        if (!strcmp(component, "api"))
            has_api = 1;
        if (!strcmp(component, "git"))
            has_git = 1;
    }
    if (!has_api || !has_git)
        return -1;
    struct stat marker;
    int owner_exists = !fstatat(request->scope_fd, "owner.json", &marker, AT_SYMLINK_NOFOLLOW);
    if (!owner_exists && errno != ENOENT)
        return -1;
    int complete = !fstatat(request->scope_fd, "prepared.json", &marker, AT_SYMLINK_NOFOLLOW);
    if ((!complete && errno != ENOENT) || owner_exists != complete ||
        (!owner_exists && inventory(request->scope_fd, 1)))
        return -1;
    json_t *location =
        source ? json_incref((json_t *)source) : json_pack("{s:s}", "kind", "os-store");
    json_t *message = location ? json_pack("{s:{s:s,s:s,s:s,s:s},s:O}", "identity", "installation",
                                           request->store_id, "issuer", "gitstore-local/core/1",
                                           "account", request->deployment_id, "purpose",
                                           "bootstrap-material", "source", location)
                               : NULL;
    json_decref(location);
    char *encoded = message ? gstd_json_dump(message) : NULL;
    gstd_credential material = {0}, candidate = {0};
    int result = -1;
    if (!encoded)
        goto done;
    int status =
        gstd_credential_run(broker, (unsigned char *)encoded, strlen(encoded), 5000, &material);
    free(encoded);
    encoded = NULL;
    if (status == 1) {
        // Missing once-created material is not permission to generate replacement
        // credentials. A lost reply after file preparation retains this marker.
        struct stat marker;
        if (!initial || !fstatat(request->scope_fd, "owner.json", &marker, AT_SYMLINK_NOFOLLOW) ||
            errno != ENOENT || gstd_bootstrap_material_generate(request->control_owner, &candidate))
            goto done;
        if (json_object_set_new(message, "createIfAbsent",
                                json_stringn((char *)candidate.bytes, candidate.length)))
            goto done;
        encoded = gstd_json_dump(message);
        if (!encoded)
            goto done;
        status =
            gstd_credential_run(broker, (unsigned char *)encoded, strlen(encoded), 5000, &material);
    }
    if (status || gstd_bootstrap_material_check(&material))
        goto done;
    result = gstd_bootstrap_prepare(request, &material);
done:
    if (encoded)
        OPENSSL_cleanse(encoded, strlen(encoded));
    free(encoded);
    gstd_credential_clear(&material);
    gstd_credential_clear(&candidate);
    json_decref(message);
    return result;
}

#else
int gstd_bootstrap_controller_bind(const gstd_bootstrap_request *request,
                                   const gstd_credential *material, const char *uid, pid_t api_pid,
                                   uint64_t api_start, const char *api_boot) {
    (void)request;
    (void)material;
    (void)uid;
    (void)api_pid;
    (void)api_start;
    (void)api_boot;
    return -1;
}
int gstd_bootstrap_prepare(const gstd_bootstrap_request *request, const gstd_credential *material) {
    (void)request;
    (void)material;
    return -1;
}
int gstd_bootstrap_provision(const gstd_bootstrap_request *request, int broker,
                             const json_t *source, int initial) {
    (void)request;
    (void)broker;
    (void)source;
    (void)initial;
    return -1;
}

#endif
