#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "native_exit.h"
#include "native_population.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "population line %d\n", __LINE__);                                     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(void) {
    char *root = gstd_test_dir();
    CHECK(root);
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(fd >= 3);
    CHECK(!mkdirat(fd, "group", 0700));
    gstd_native_population group = GSTD_NATIVE_POPULATION_INITIAL;
    CHECK(gstd_native_population_empty(&group) == -1);
    group.directory_fd = openat(fd, "group", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(group.directory_fd >= 3);
    int writer =
        openat(group.directory_fd, "events", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(writer >= 3 && write(writer, "populated 0\n", 12) == 12 && !close(writer));
    group.events_fd = openat(group.directory_fd, "events", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat directory, event;
    CHECK(group.events_fd >= 3 && !fstat(group.directory_fd, &directory) &&
          !fstat(group.events_fd, &event));
    group.device = (uint64_t)directory.st_dev;
    group.inode = (uint64_t)directory.st_ino;
    group.events_inode = (uint64_t)event.st_ino;
    CHECK(gstd_native_population_empty(&group) == -1);
    CHECK(!gstd_native_population_identity(&group, "unowned",
                                           "00000000-0000-0000-0000-000000000001"));
    /* Unlinked ordinary inodes are not cgroup removal proof, even when a forged
     * events file says empty. Held descriptors must retain actual cgroup2 type. */
    CHECK(!unlinkat(group.directory_fd, "events", 0) && !unlinkat(fd, "group", AT_REMOVEDIR));
    CHECK(gstd_native_population_empty(&group) == -1);
    gstd_native_population_close(&group);
    CHECK(group.directory_fd == -1 && group.events_fd == -1);
    char boot[37];
    uint64_t start;
#ifdef __linux__
    CHECK(!gstd_native_boot_id(boot) && !gstd_native_process_start(getpid(), &start));
#else
    boot[0] = 0;
    start = 1;
#endif
    CHECK(gstd_native_population_open("unowned", getpid(), start + 1, boot, &group) == -1);
    CHECK(gstd_native_population_open("unowned", getpid(), start, boot, &group) == -1);
    CHECK(group.directory_fd == -1 && group.events_fd == -1);
    CHECK(gstd_native_population_reopen(NULL, "unowned", boot, &group) == -1);
    json_t *receipt = json_pack("{s:i,s:s,s:s,s:s,s:s,s:s}", "schemaVersion", 1, "bootId", boot,
                                "scopePath", "/sys/fs/cgroup/crun-unowned.scope/container",
                                "cgroupDevice", "1", "cgroupInode", "2", "eventsInode", "3");
    CHECK(receipt);
    CHECK(gstd_native_population_reopen(receipt, "unowned", boot, &group) == -1);
    const char *invalid[] = {
        "/tmp/crun-unowned.scope/container", "/sys/fs/cgroup/crun-other.scope/container",
        "/sys/fs/cgroup/crun-unowned.scope/../container",
        "/sys/fs/cgroup/crun-unowned.scope/foo/..", "/sys/fs/cgroup/crun-unowned.scope//container"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        CHECK(!json_object_set_new(receipt, "scopePath", json_string(invalid[i])));
        CHECK(gstd_native_population_reopen(receipt, "unowned", boot, &group) == -1);
    }
    CHECK(!json_object_set_new(receipt, "scopePath",
                               json_string("/sys/fs/cgroup/crun-unowned.scope/container")));
    CHECK(!json_object_set_new(receipt, "cgroupInode", json_string("18446744073709551616")));
    CHECK(gstd_native_population_reopen(receipt, "unowned", boot, &group) == -1);
    CHECK(group.directory_fd == -1 && group.events_fd == -1);
    json_decref(receipt);
    close(fd);
    CHECK(!gstd_test_remove(root));
    return 0;
}
