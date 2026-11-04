#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/assets.h"
#include "gitstored/dependencies.h"
#include "gitstored/protocol.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
int main(int argc, char **argv) {
    gstd_json_initialize();
    int result = 0, input = -1, writer = -1, reader = -1, pipefds[2] = {-1, -1};
    char *root = gstd_test_dir(), path[1024], alias[1024];
    json_t *components = NULL, *manifest = NULL, *proof = NULL, *changed = NULL, *other = NULL;
    CHECK(argc == 2 && root);
    CHECK(snprintf(path, sizeof(path), "%s/manifest", root) > 0);
    CHECK(snprintf(alias, sizeof(alias), "%s/alias", root) > 0);
    input = open(argv[1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(input >= 0);
    unsigned char bytes[4096], tail;
    ssize_t size = read(input, bytes, sizeof(bytes));
    CHECK(size > 0 && size < (ssize_t)sizeof(bytes) && read(input, &tail, 1) == 0);
    writer = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    CHECK(writer >= 0 && write(writer, bytes, (size_t)size) == size);
    reader = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(reader >= 0 && lseek(reader, 17, SEEK_SET) == 17);
    components = json_pack("[s]", "api");
    CHECK(components);
    manifest = gstd_asset_manifest(components, "arm64");
    CHECK(manifest);
    proof = gstd_dependency_asset_fd(manifest, "api", reader, "cache");
    CHECK(proof && !gstd_dependency_asset_fd_check(proof, manifest, "api", reader, "cache"));
    CHECK(lseek(reader, 0, SEEK_CUR) == 17 && fcntl(reader, F_GETFD) >= 0);
    CHECK(!gstd_dependency_asset_fd(manifest, "api", writer, "cache"));
    changed = json_deep_copy(proof);
    CHECK(changed);
    CHECK(!json_object_set_new(json_object_get(changed, "pathIdentity"), "trusted", json_true()));
    CHECK(gstd_dependency_asset_fd_check(changed, manifest, "api", reader, "cache") < 0);
    CHECK(!fchmod(writer, 0644));
    CHECK(!gstd_dependency_asset_fd(manifest, "api", reader, "cache"));
    other = gstd_dependency_asset_fd(manifest, "api", reader, "host");
    CHECK(other && !gstd_dependency_asset_fd_check(other, manifest, "api", reader, "host"));
    CHECK(!fchmod(writer, 0664));
    CHECK(!gstd_dependency_asset_fd(manifest, "api", reader, "host"));
    CHECK(!fchmod(writer, 0600));
    CHECK(!link(path, alias));
    CHECK(!gstd_dependency_asset_fd(manifest, "api", reader, "host"));
    CHECK(!unlink(alias));
    unsigned char bad = bytes[0] ^ 1;
    CHECK(pwrite(writer, &bad, 1, 0) == 1);
    CHECK(!gstd_dependency_asset_fd(manifest, "api", reader, "cache"));
    CHECK(gstd_dependency_asset_fd_check(proof, manifest, "api", reader, "cache") < 0);
    CHECK(!ftruncate(writer, 1024 * 1024 + 1));
    CHECK(!gstd_dependency_asset_fd(manifest, "api", reader, "host"));
    CHECK(!pipe(pipefds));
    CHECK(!gstd_dependency_asset_fd(manifest, "api", pipefds[0], "host"));
done:
    if (input >= 0)
        close(input);
    if (writer >= 0)
        close(writer);
    if (reader >= 0)
        close(reader);
    if (pipefds[0] >= 0)
        close(pipefds[0]);
    if (pipefds[1] >= 0)
        close(pipefds[1]);
    json_decref(components);
    json_decref(manifest);
    json_decref(proof);
    json_decref(changed);
    json_decref(other);
    if (root && gstd_test_remove(root))
        result = 1;
    return result;
}
