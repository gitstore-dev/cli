#define _GNU_SOURCE 1
#include "bootstrap.h"
#include "bootstrap_material.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "native_exit.h"
#include "support.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d\n", __LINE__);                                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static char *read_file(int root, const char *name) {
    int fd = openat(root, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat info;
    if (fd < 0)
        return NULL;
    char *bytes = NULL;
    if (!fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= 32768 &&
        info.st_uid == geteuid() && info.st_nlink == 1 && (info.st_mode & 07777) == 0600) {
        bytes = malloc((size_t)info.st_size + 1);
        if (bytes && pread(fd, bytes, (size_t)info.st_size, 0) == info.st_size)
            bytes[info.st_size] = 0;
        else {
            free(bytes);
            bytes = NULL;
        }
    }
    close(fd);
    return bytes;
}
static int worker(pid_t parent, int root, int persistent) {
    alarm(15);
    struct rlimit core = {0, 0};
    CHECK(!setrlimit(RLIMIT_CORE, &core));
    CHECK(!mkdirat(root, "scope", 0700));
    int scope = openat(root, "scope", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(scope >= 3);
    json_t *components = json_pack("[s,s,s]", "api", "git", "controller");
#if defined(__aarch64__)
    json_t *manifest =
        gstd_asset_manifest_for_datastore(components, "arm64", persistent ? "scylladb" : "memdb");
#else
    json_t *manifest =
        gstd_asset_manifest_for_datastore(components, "amd64", persistent ? "scylladb" : "memdb");
#endif
    json_decref(components);
    CHECK(manifest);
    gstd_bootstrap_request request = {.manifest = manifest,
                                      .store_id = "0123456789abcdef0123456789abcdef",
                                      .deployment_id = "fixture",
                                      .operation_id = "fixture-start",
                                      .datastore = persistent ? "scylladb" : "memdb",
                                      .generation = 1,
                                      .control_owner = parent,
                                      .scope_fd = scope};
    gstd_credential material = {0}, replacement = {0};
    CHECK(!gstd_bootstrap_material_generate(parent, &material));
    CHECK(!gstd_bootstrap_material_generate(parent, &replacement));
    struct stat before, after;
    char boot[37];
    uint64_t api_start;
    CHECK(!gstd_native_boot_id(boot) && !gstd_native_process_start(parent, &api_start));
    CHECK(gstd_bootstrap_controller_bind(&request, &material, "assigned-uid", parent, api_start,
                                         boot) == -1);
    CHECK(fstatat(scope, "owner.json", &after, AT_SYMLINK_NOFOLLOW) && errno == ENOENT);
    CHECK(!gstd_bootstrap_prepare(&request, &material));
    CHECK(gstd_bootstrap_controller_bind(&request, &material, "bad\nuid", parent, api_start,
                                         boot) == -1);
    CHECK(gstd_bootstrap_controller_bind(&request, &material, "assigned-uid", parent, api_start + 1,
                                         boot) == -1);
    CHECK(!gstd_bootstrap_controller_bind(&request, &material, "assigned-uid", parent, api_start,
                                          boot));
    CHECK(!fstatat(scope, "controller-owned.toml", &before, AT_SYMLINK_NOFOLLOW));
    CHECK(!gstd_bootstrap_controller_bind(&request, &material, "assigned-uid", parent, api_start,
                                          boot));
    CHECK(!fstatat(scope, "controller-owned.toml", &after, AT_SYMLINK_NOFOLLOW) &&
          before.st_ino == after.st_ino);
    CHECK(gstd_bootstrap_controller_bind(&request, &material, "another-uid", parent, api_start,
                                         boot) == -1);
    CHECK(!fstatat(scope, "api-owned.toml", &before, AT_SYMLINK_NOFOLLOW));
    CHECK(!gstd_bootstrap_prepare(&request, &material));
    CHECK(!fstatat(scope, "api-owned.toml", &after, AT_SYMLINK_NOFOLLOW) &&
          before.st_ino == after.st_ino && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
          before.st_mtim.tv_nsec == after.st_mtim.tv_nsec);
    char *api = read_file(scope, "api-owned.toml"), *base = read_file(scope, "api-base.toml"),
         *git = read_file(scope, "git.toml"),
         *controller = read_file(scope, "controller-base.toml"),
         *users = read_file(scope, "users.yaml"),
         *signing = read_file(scope, "controller-signing.json"),
         *owner = read_file(scope, "owner.json");
    CHECK(api && base && git && controller && users && signing && owner);
    if (persistent) {
        CHECK(strstr(base, "backend = \"scylla\"") && strstr(base, "auto_migrate = false") &&
              strstr(base, "127.0.0.1:9042") && !strstr(base, "memdb"));
        CHECK(strstr(owner, json_string_value(json_object_get(manifest, "manifestDigest"))));
    } else
        CHECK(strstr(base, "backend = \"memdb\"") && !strstr(base, "auto_migrate"));
    json_t *secrets = gstd_json_parse(material.bytes, material.length);
    const char *password = json_string_value(json_object_get(secrets, "password")),
               *jwt = json_string_value(json_object_get(secrets, "jwtSecret")),
               *hmac = json_string_value(json_object_get(secrets, "hmacSecret")),
               *key = json_string_value(json_object_get(secrets, "controllerPEM"));
    CHECK(strstr(api, "BEGIN PRIVATE KEY") && strstr(api, jwt) && strstr(api, hmac));
    CHECK(strstr(git, hmac) && !strstr(git, "PRIVATE KEY") && !strstr(git, jwt));
    CHECK(!strstr(base, "PRIVATE KEY") && !strstr(base, hmac) && !strstr(base, jwt));
    CHECK(!strstr(controller, "PRIVATE KEY") && !strstr(controller, hmac) &&
          !strstr(controller, jwt));
    CHECK(!strstr(users, password) &&
          strstr(users, json_string_value(json_object_get(secrets, "passwordHash"))));
    CHECK(!strstr(owner, password) && !strstr(owner, "PRIVATE KEY") && !strstr(owner, "password"));
    json_t *record = gstd_json_parse((unsigned char *)signing, strlen(signing));
    CHECK(record && !strcmp(json_string_value(json_object_get(record, "format")),
                            "serviceaccount-signing-key/v1"));
    char encoded[2048];
    CHECK(EVP_EncodeBlock((unsigned char *)encoded, (unsigned char *)key, (int)strlen(key)) > 0);
    CHECK(
        !strcmp(json_string_value(json_object_get(json_object_get(record, "values"), "privateKey")),
                encoded));
    CHECK(gstd_bootstrap_prepare(&request, &replacement) == -1);
    gstd_bootstrap_request wrong = request;
    wrong.generation++;
    CHECK(gstd_bootstrap_prepare(&wrong, &material) == -1);
    wrong = request;
    wrong.datastore = persistent ? "memdb" : "scylladb";
    CHECK(gstd_bootstrap_prepare(&wrong, &material) == -1);
    wrong = request;
    wrong.control_owner = getpid();
    CHECK(gstd_bootstrap_prepare(&wrong, &material) == -1);
    CHECK(!linkat(scope, "users.yaml", root, "linked-users", 0));
    CHECK(gstd_bootstrap_prepare(&request, &material) == -1);
    CHECK(!unlinkat(root, "linked-users", 0) && !gstd_bootstrap_prepare(&request, &material));
    int fd = openat(scope, "git.toml", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(fd >= 3);
    CHECK(pwrite(fd, "tamper", 6, 0) == 6 && !close(fd));
    CHECK(gstd_bootstrap_prepare(&request, &material) == -1);
    char *changed = read_file(scope, "git.toml");
    CHECK(changed && !memcmp(changed, "tamper", 6));
    fd = openat(scope, "git.toml", O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
    CHECK(fd >= 3);
    CHECK(write(fd, git, strlen(git)) == (ssize_t)strlen(git) && !close(fd));
    CHECK(!gstd_bootstrap_prepare(&request, &material));
    CHECK(!unlinkat(scope, "users.yaml", 0));
    CHECK(gstd_bootstrap_prepare(&request, &material) == -1);
    CHECK(fstatat(scope, "users.yaml", &after, AT_SYMLINK_NOFOLLOW) && errno == ENOENT);
    CHECK(!mkdirat(root, "foreign", 0700));
    int foreign = openat(root, "foreign", O_DIRECTORY | O_CLOEXEC);
    CHECK(foreign >= 3);
    fd = openat(foreign, "api-base.toml", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    CHECK(fd >= 3 && !close(fd));
    wrong = request;
    wrong.scope_fd = foreign;
    CHECK(gstd_bootstrap_prepare(&wrong, &material) == -1);
    CHECK(fstatat(foreign, "prepare.lock", &after, AT_SYMLINK_NOFOLLOW) && errno == ENOENT);
    close(foreign);
    close(scope);
    json_decref(manifest);
    json_decref(secrets);
    json_decref(record);
    char *buffers[] = {api, base, git, controller, users, signing, owner, changed};
    for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++) {
        OPENSSL_cleanse(buffers[i], strlen(buffers[i]));
        free(buffers[i]);
    }
    OPENSSL_cleanse(encoded, sizeof(encoded));
    gstd_credential_clear(&material);
    gstd_credential_clear(&replacement);
    return 0;
}
static int scenario(int persistent) {
    gstd_json_initialize();
    char *root = gstd_test_dir();
    if (!root || !geteuid())
        return 1;
    int fd = open(root, O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    pid_t parent = getpid(), child = fork();
    if (!child)
        _exit(worker(parent, fd, persistent));
    int status = 0;
    int valid = child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                !WEXITSTATUS(status);
    if (fd >= 0)
        close(fd);
    if (gstd_test_remove(root))
        valid = 0;
    return valid ? 0 : 1;
}

int main(void) { return scenario(0) || scenario(1); }
