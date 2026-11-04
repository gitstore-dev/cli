#define _GNU_SOURCE 1
#include "execution_plan.h"
#include "gitstored/assets.h"
#include "gitstored/execution.h"
#include "namespace_owner.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
static int descriptor_path(int fd, int directory, char output[4096]) {
    struct stat info;
    int flags = fcntl(fd, F_GETFL);
    if (fd < 3 || flags < 0 || fstat(fd, &info) || info.st_uid != geteuid() ||
        (directory
             ? (!S_ISDIR(info.st_mode) || (info.st_mode & 0022))
             : (!S_ISREG(info.st_mode) || (info.st_mode & 07777) != 0600 || info.st_nlink != 1 ||
                info.st_size <= 0 || info.st_size > 1048576 || (flags & O_ACCMODE) != O_RDONLY)))
        return -1;
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t length = readlink(link, output, 4095);
    if (length <= 0 || length >= 4095)
        return -1;
    output[length] = 0;
    return output[0] == '/' && !strstr(output, " (deleted)") ? 0 : -1;
}
static int mount_add(json_t *mounts, const char *destination, const char *type, const char *source,
                     const char *access, int recursive) {
    return json_array_append_new(mounts,
                                 json_pack("{s:s,s:s,s:s,s:[s,s,s,s]}", "destination", destination,
                                           "type", type, "source", source, "options", access,
                                           "nosuid", "nodev", recursive ? "rbind" : "bind"));
}
#endif
json_t *gstd_execution_build(const gstd_execution_inputs *input) {
    if (!input || !input->component || gstd_asset_manifest_check(input->manifest))
        return NULL;
#ifdef __linux__
#if defined(__aarch64__)
    const char *architecture = "arm64";
#elif defined(__x86_64__)
    const char *architecture = "amd64";
#else
    return NULL;
#endif
    const char *registered_architecture = json_string_value(
        json_object_get(json_object_get(input->manifest, "platform"), "architecture"));
    if (!registered_architecture || strcmp(architecture, registered_architecture))
        return NULL;
    const char *component = input->component, *command = NULL;
    unsigned roles = 0;
    if (!strcmp(component, "api")) {
        command = "/app/api";
        roles = (1u << GSTD_MOUNT_BASE) | (1u << GSTD_MOUNT_OVERLAY) | (1u << GSTD_MOUNT_POLICY) |
                (1u << GSTD_MOUNT_USERS);
    } else if (!strcmp(component, "git")) {
        command = "/app/git-service";
        roles = (1u << GSTD_MOUNT_BASE) | (1u << GSTD_MOUNT_REPOSITORIES);
    } else if (!strcmp(component, "controller")) {
        command = "/app/controller-manager";
        roles = (1u << GSTD_MOUNT_BASE) | (1u << GSTD_MOUNT_OVERLAY) |
                (1u << GSTD_MOUNT_SIGNING_RECORD);
    } else
        return NULL;
    if (input->network) {
        if (gstd_network_namespace_check(input->network))
            return NULL;
        roles |= 1u << GSTD_MOUNT_RESOLVER;
    }
    int registered = 0;
    json_t *images = json_object_get(input->manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        const char *name =
            json_string_value(json_object_get(json_array_get(images, i), "component"));
        if (name && !strcmp(name, component))
            registered = 1;
    }
    char root[4096], sources[GSTD_MOUNT_COUNT][4096];
    struct stat identities[GSTD_MOUNT_COUNT];
    if (!registered || descriptor_path(input->rootfs_fd, 1, root))
        return NULL;
    for (unsigned i = 0; i < GSTD_MOUNT_COUNT; i++) {
        if (!(roles & (1u << i))) {
            if (input->role_fds[i] != -1)
                return NULL;
        } else if (descriptor_path(input->role_fds[i], i == GSTD_MOUNT_REPOSITORIES, sources[i]))
            return NULL;
        else {
            if (fstat(input->role_fds[i], &identities[i]) ||
                (i == GSTD_MOUNT_REPOSITORIES && (identities[i].st_mode & 07777) != 0700))
                return NULL;
            for (unsigned j = 0; j < i; j++)
                if ((roles & (1u << j)) && identities[j].st_dev == identities[i].st_dev &&
                    identities[j].st_ino == identities[i].st_ino)
                    return NULL;
        }
    }
    if (input->network) {
        static const char resolver[] = "nameserver 10.0.2.3\noptions timeout:2 attempts:2\n";
        char bytes[sizeof(resolver)];
        if (identities[GSTD_MOUNT_RESOLVER].st_size != sizeof(resolver) - 1 ||
            pread(input->role_fds[GSTD_MOUNT_RESOLVER], bytes, sizeof(bytes), 0) !=
                sizeof(resolver) - 1 ||
            memcmp(bytes, resolver, sizeof(resolver) - 1))
            return NULL;
    }
    json_t *spec = json_object(), *process = json_object(), *linux = json_object(),
           *mounts = json_array(), *caps = json_object(), *namespaces = json_array(),
           *policy = gstd_execution_policy(),
           *args = json_pack("[s,s,s]", command, "--config-file", "/etc/gitstore/gitstore.toml");
    if (!spec || !process || !linux || !mounts || !caps || !namespaces || !policy || !args)
        goto fail;
    if ((roles & (1u << GSTD_MOUNT_OVERLAY)) &&
        (json_array_append_new(args, json_string("--config-file")) ||
         json_array_append_new(args, json_string("/etc/gitstore/owned.toml"))))
        goto fail;
    const char *sets[] = {"bounding", "effective", "inheritable", "permitted", "ambient"};
    for (size_t i = 0; i < 5; i++)
        if (json_object_set_new(caps, sets[i], json_array()))
            goto fail;
    const char *types[] = {"user", "pid", "mount", "uts", "ipc", "network"};
    for (size_t i = 0; i < 6; i++) {
        json_t *entry = json_pack("{s:s}", "type", types[i]);
        if (entry && input->network && (i == 0 || i == 5)) {
            char path[64];
            snprintf(path, sizeof(path), "/proc/self/fd/%d",
                     i == 0 ? input->network->user_fd : input->network->network_fd);
            if (json_object_set_new(entry, "path", json_string(path))) {
                json_decref(entry);
                goto fail;
            }
        }
        if (!entry || json_array_append_new(namespaces, entry))
            goto fail;
    }
    const char *destinations[] = {"/etc/gitstore/gitstore.toml",
                                  "/etc/gitstore/owned.toml",
                                  "/etc/gitstore/policy.yaml",
                                  "/etc/gitstore/users.yaml",
                                  "/run/gitstore/controller-signing.json",
                                  "/var/lib/gitstore/repositories",
                                  "/etc/resolv.conf"};
    for (unsigned i = 0; i < GSTD_MOUNT_COUNT; i++)
        if ((roles & (1u << i)) &&
            mount_add(mounts, destinations[i], "bind", sources[i],
                      i == GSTD_MOUNT_REPOSITORIES ? "rw" : "ro", i == GSTD_MOUNT_REPOSITORIES))
            goto fail;
    if (json_array_append_new(mounts, json_pack("{s:s,s:s,s:s,s:[s,s,s]}", "destination", "/proc",
                                                "type", "proc", "source", "proc", "options",
                                                "nosuid", "nodev", "noexec")))
        goto fail;
    const char *temporaries[] = {"/tmp", "/run", "/dev"};
    const char *options[] = {"size=67108864,mode=1777", "size=4194304,mode=0700",
                             "size=4194304,mode=0755"};
    for (size_t i = 0; i < 3; i++)
        if (json_array_append_new(mounts,
                                  json_pack("{s:s,s:s,s:s,s:[s,s,s]}", "destination",
                                            temporaries[i], "type", "tmpfs", "source", "tmpfs",
                                            "options", "nosuid", "noexec", options[i])))
            goto fail;
    /* OCI applies mount entries in order: mount private tmpfs parents before
     * their nested read-only role files. Move the fixed parent mounts first. */
    for (size_t i = 0; i < 3; i++) {
        size_t last = json_array_size(mounts) - 1;
        json_t *entry = json_incref(json_array_get(mounts, last));
        int failed = json_array_remove(mounts, last) || json_array_insert(mounts, 0, entry);
        json_decref(entry);
        if (failed)
            goto fail;
    }
    if (json_object_set_new(process, "terminal", json_false()) ||
        json_object_set_new(process, "user", json_pack("{s:i,s:i}", "uid", 0, "gid", 0)) ||
        json_object_set(process, "args", args) ||
        json_object_set_new(process, "cwd", json_string("/app")) ||
        json_object_set_new(
            process, "env",
            json_pack("[s,s]", "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                      "HOME=/tmp")) ||
        json_object_set_new(process, "noNewPrivileges", json_true()) ||
        json_object_set(process, "capabilities", caps) ||
        json_object_set_new(process, "rlimits",
                            json_pack("[{s:s,s:I,s:I},{s:s,s:i,s:i},{s:s,s:i,s:i}]", "type",
                                      "RLIMIT_AS", "soft", (json_int_t)(UINT64_C(1) << 40), "hard",
                                      (json_int_t)(UINT64_C(1) << 40), "type", "RLIMIT_CORE",
                                      "soft", 0, "hard", 0, "type", "RLIMIT_NOFILE", "soft", 1024,
                                      "hard", 1024)) ||
        json_object_set(linux, "namespaces", namespaces) ||
        json_object_set(linux, "seccomp", policy) ||
        json_object_set_new(linux, "uidMappings",
                            json_pack("[{s:i,s:I,s:i}]", "containerID", 0, "hostID",
                                      (json_int_t)geteuid(), "size", 1)) ||
        json_object_set_new(linux, "gidMappings",
                            json_pack("[{s:i,s:I,s:i}]", "containerID", 0, "hostID",
                                      (json_int_t)getegid(), "size", 1)) ||
        json_object_set_new(linux, "resources",
                            json_pack("{s:{s:I},s:{s:i},s:{s:i,s:i}}", "memory", "limit",
                                      (json_int_t)536870912, "pids", "limit", 256, "cpu", "quota",
                                      100000, "period", 100000)) ||
        json_object_set_new(linux, "maskedPaths",
                            json_pack("[s,s,s,s,s]", "/proc/kcore", "/proc/keys",
                                      "/proc/timer_list", "/proc/sched_debug", "/sys/firmware")) ||
        json_object_set_new(
            linux, "readonlyPaths",
            json_pack("[s,s,s,s]", "/proc/sys", "/proc/sysrq-trigger", "/proc/irq", "/proc/bus")) ||
        json_object_set_new(spec, "ociVersion", json_string("1.0.2")) ||
        json_object_set_new(spec, "hostname", json_string(component)) ||
        json_object_set_new(spec, "root", json_pack("{s:s,s:b}", "path", root, "readonly", 1)) ||
        json_object_set(spec, "process", process) || json_object_set(spec, "linux", linux) ||
        json_object_set(spec, "mounts", mounts))
        goto fail;
    if (input->network &&
        (json_object_del(linux, "uidMappings") || json_object_del(linux, "gidMappings")))
        goto fail;
#ifdef GITSTORED_INSTRUMENTED
    /* ASan reserves many TiB before the namespace payload exec. The ordinary
     * build proves finite worker/workload address bounds; this diagnostic lane
     * retains physical cgroup, file-size, descriptor and core limits. */
    if (json_array_remove(json_object_get(process, "rlimits"), 0))
        goto fail;
#endif
    json_decref(process);
    json_decref(linux);
    json_decref(mounts);
    json_decref(caps);
    json_decref(namespaces);
    json_decref(policy);
    json_decref(args);
    return spec;
fail:
    json_decref(spec);
    json_decref(process);
    json_decref(linux);
    json_decref(mounts);
    json_decref(caps);
    json_decref(namespaces);
    json_decref(policy);
    json_decref(args);
#endif
    return NULL;
}
