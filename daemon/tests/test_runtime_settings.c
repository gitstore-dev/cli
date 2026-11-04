#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "gitstored/protocol.h"
#include "runtime_settings.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "settings line %d\n", __LINE__);                                       \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int write_settings(int root, const char *text) {
    int fd =
        openat(root, "settings.json", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 3)
        return -1;
    int valid = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    close(fd);
    return valid ? 0 : -1;
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    gstd_json_initialize();
    char *path = gstd_test_dir();
    CHECK(path);
    int root = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(root >= 3);
    gstd_runtime_settings settings = GSTD_RUNTIME_SETTINGS_INITIAL;
    CHECK(!gstd_runtime_settings_load(root, &settings));
    CHECK(!strcmp(json_string_value(json_object_get(settings.credential_source, "kind")),
                  "os-store"));
    gstd_runtime_settings_clear(&settings);
    CHECK(faccessat(root, "settings.json", F_OK, 0) != 0);
    const char *bad[] = {
        "{}",
        "{\"schemaVersion\":2}",
        "{\"schemaVersion\":1,\"shell\":\"anything\"}",
        "{\"schemaVersion\":1,\"schemaVersion\":1}",
        "{\"schemaVersion\":1,\"offlineLayouts\":null}",
        "{\"schemaVersion\":1,\"bootstrapCredentialSource\":null}",
        "{\"schemaVersion\":1,\"bootstrapCredentialSource\":{\"kind\":\"os-store\\u0000file\"}}",
        "{\"schemaVersion\":1,\"offlineLayouts\":{\"api\\u0000bad\":\"/tmp\"}}",
        "{\"schemaVersion\":1,\"offlineLayouts\":{\"storefront\":\"/tmp\"}}",
        "{\"schemaVersion\":1,\"offlineLayouts\":{\"api\":\"relative\"}}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        CHECK(!write_settings(root, bad[i]) && gstd_runtime_settings_load(root, &settings) == -1);
        CHECK(!settings.credential_source && settings.offline_fds[0] == -1);
    }
    CHECK(!mkdirat(root, "credentials", 0700) && !mkdirat(root, "offline", 0700));
    char *text = NULL;
    CHECK(
        asprintf(
            &text,
            "{\"schemaVersion\":1,\"bootstrapCredentialSource\":{\"kind\":\"protected-directory\","
            "\"path\":\"%s/credentials\"},\"offlineLayouts\":{\"api\":\"%s/offline\"}}",
            path, path) > 0);
    CHECK(!write_settings(root, text));
    free(text);
    CHECK(!gstd_runtime_settings_load(root, &settings));
    CHECK(settings.credential_parent_fd >= 3 && settings.offline_fds[0] >= 3 &&
          settings.offline_fds[1] == -1);
    CHECK(faccessat(settings.credential_parent_fd, "new-material", F_OK, 0) != 0);
    json_t *one =
               gstd_runtime_settings_source(&settings, "0123456789abcdef0123456789abcdef", "one"),
           *two =
               gstd_runtime_settings_source(&settings, "0123456789abcdef0123456789abcdef", "two"),
           *other =
               gstd_runtime_settings_source(&settings, "fedcba9876543210fedcba9876543210", "one"),
           *same =
               gstd_runtime_settings_source(&settings, "0123456789abcdef0123456789abcdef", "one");
    CHECK(one && two && other && same && json_equal(one, same) && !json_equal(one, two) &&
          !json_equal(one, other));
    CHECK(!strcmp(json_string_value(json_object_get(one, "kind")), "protected-file"));
    CHECK(access(json_string_value(json_object_get(one, "path")), F_OK) != 0);
    CHECK(!gstd_runtime_settings_source(&settings, "bad-store", "one"));
    CHECK(!renameat(root, "credentials", root, "moved-credentials") &&
          !mkdirat(root, "credentials", 0700));
    CHECK(!gstd_runtime_settings_source(&settings, "0123456789abcdef0123456789abcdef", "one"));
    CHECK(!unlinkat(root, "credentials", AT_REMOVEDIR) &&
          !renameat(root, "moved-credentials", root, "credentials"));
    json_decref(one);
    json_decref(two);
    json_decref(other);
    json_decref(same);
    /* The parsed snapshot and held offline inode survive owner settings edits. */
    CHECK(!write_settings(root, "{\"schemaVersion\":1}"));
    CHECK(!strcmp(json_string_value(json_object_get(settings.credential_source, "kind")),
                  "protected-directory"));
    gstd_runtime_settings_clear(&settings);
    CHECK(!fchmodat(root, "settings.json", 0644, 0));
    CHECK(gstd_runtime_settings_load(root, &settings) == -1);
    CHECK(!fchmodat(root, "settings.json", 0600, 0));
    CHECK(!linkat(root, "settings.json", root, "hardlink", 0));
    CHECK(gstd_runtime_settings_load(root, &settings) == -1 && !unlinkat(root, "hardlink", 0));
    CHECK(!renameat(root, "settings.json", root, "original.json") &&
          !symlinkat("original.json", root, "settings.json"));
    CHECK(gstd_runtime_settings_load(root, &settings) == -1 && !unlinkat(root, "settings.json", 0));
    CHECK(!symlinkat("offline", root, "linked"));
    CHECK(asprintf(&text, "{\"schemaVersion\":1,\"offlineLayouts\":{\"api\":\"%s/linked\"}}",
                   path) > 0);
    CHECK(!write_settings(root, text));
    free(text);
    CHECK(gstd_runtime_settings_load(root, &settings) == -1);
    CHECK(asprintf(&text, "{\"schemaVersion\":1,\"offlineLayouts\":{\"api\":\"%s/missing\"}}",
                   path) > 0);
    CHECK(!write_settings(root, text));
    free(text);
    CHECK(gstd_runtime_settings_load(root, &settings) == -1 &&
          faccessat(root, "missing", F_OK, 0) != 0);
    CHECK(!fchmodat(root, "credentials", 0755, 0));
    CHECK(asprintf(&text,
                   "{\"schemaVersion\":1,\"bootstrapCredentialSource\":{\"kind\":\"protected-"
                   "directory\",\"path\":\"%s/credentials\"}}",
                   path) > 0);
    CHECK(!write_settings(root, text));
    free(text);
    CHECK(gstd_runtime_settings_load(root, &settings) == -1);
    CHECK(!mkdirat(root, "runtime", 0700));
    char runtime[4096];
    CHECK(snprintf(runtime, sizeof(runtime), "%s/runtime", path) > 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        alarm(3);
        execl(argv[1], argv[1], "--runtime-dir", runtime, "--state-dir", path, (char *)NULL);
        _exit(127);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 2);
    CHECK(faccessat(root, "state.db", F_OK, 0) != 0);
    CHECK(faccessat(root, "credentials/new-material", F_OK, 0) != 0);
    close(root);
    CHECK(!gstd_test_remove(path));
    return 0;
}
