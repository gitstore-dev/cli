#define _GNU_SOURCE 1
#include "oidc_execution.h"
#include "gitstored/execution.h"
#include "namespace_owner.h"
#include "oidc_config.h"
#include "oidc_profile.h"
#include <string.h>
#ifdef __linux__
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
static int descriptor(int fd, int directory, char path[4096], struct stat *info) {
    int flags = fcntl(fd, F_GETFL);
    if (fd < 3 || flags < 0 || fstat(fd, info) || info->st_uid != geteuid() ||
        (directory
             ? (!S_ISDIR(info->st_mode) || (info->st_mode & 0022))
             : (!S_ISREG(info->st_mode) || (info->st_mode & 07777) != 0600 || info->st_nlink != 1 ||
                info->st_size <= 0 || info->st_size > 65536 || (flags & O_ACCMODE) != O_RDONLY)))
        return -1;
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t length = readlink(link, path, 4095);
    if (length <= 0 || length >= 4095)
        return -1;
    path[length] = 0;
    return path[0] == '/' && !strstr(path, " (deleted)") ? 0 : -1;
}
static int mount_file(json_t *mounts, const char *destination, const char *source, int writable) {
    return json_array_append_new(mounts, json_pack("{s:s,s:s,s:s,s:[s,s,s,s,s]}", "destination",
                                                   destination, "type", "bind", "source", source,
                                                   "options", writable ? "rw" : "ro", "nosuid",
                                                   "nodev", "noexec", writable ? "rbind" : "bind"));
}
#endif
json_t *gstd_oidc_execution_build(const gstd_execution_inputs *input) {
    if (!input || !gstd_oidc_component(input->component) ||
        (gstd_oidc_manifest_check(input->manifest) &&
         gstd_oidc_core_manifest_check(input->manifest)) ||
        !input->network || gstd_network_namespace_check(input->network) ||
        input->network->namespace_uid != 0 || gstd_oidc_ports_check(input->oidc_ports))
        return NULL;
    int selected = 0;
    const json_t *images = json_object_get(input->manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        const json_t *component = json_object_get(json_array_get(images, i), "component");
        selected |=
            json_is_string(component) &&
            json_string_length(component) == strlen(input->component) &&
            !memcmp(json_string_value(component), input->component, strlen(input->component));
    }
    if (!selected)
        return NULL;
#ifdef __linux__
#if defined(__aarch64__)
    const char *architecture = "arm64";
#elif defined(__x86_64__)
    const char *architecture = "amd64";
#else
    return NULL;
#endif
    if (strcmp(architecture, json_string_value(json_object_get(
                                 json_object_get(input->manifest, "platform"), "architecture"))))
        return NULL;
    int hydra = !strcmp(input->component, "oidc-hydra"),
        kratos = !strcmp(input->component, "oidc-kratos"),
        ui = !strcmp(input->component, "oidc-ui"),
        bridge = !strcmp(input->component, "oidc-bridge"),
        smtp = !strcmp(input->component, "oidc-smtp");
    int migration = input->purpose == GSTD_EXEC_OIDC_MIGRATE;
    if ((input->purpose != GSTD_EXEC_SERVICE && !migration) || (migration && !hydra && !kratos))
        return NULL;
    unsigned roles = (1u << GSTD_MOUNT_RESOLVER);
    if (!migration && (hydra || kratos || ui))
        roles |= 1u << GSTD_MOUNT_OVERLAY;
    if (hydra || kratos)
        roles |= 1u << GSTD_MOUNT_DATASTORE;
    if (kratos && !migration)
        roles |= 1u << GSTD_MOUNT_POLICY;
    char root[4096], sources[GSTD_MOUNT_COUNT][4096];
    struct stat root_info, files[GSTD_MOUNT_COUNT];
    if (descriptor(input->rootfs_fd, 1, root, &root_info))
        return NULL;
    for (unsigned i = 0; i < GSTD_MOUNT_COUNT; i++) {
        if (!(roles & (1u << i))) {
            if (input->role_fds[i] != -1)
                return NULL;
            continue;
        }
        if (descriptor(input->role_fds[i], i == GSTD_MOUNT_DATASTORE, sources[i], &files[i]))
            return NULL;
        if (i == GSTD_MOUNT_DATASTORE &&
            ((files[i].st_mode & 07777) != 0700 ||
             (files[i].st_dev == root_info.st_dev && files[i].st_ino == root_info.st_ino)))
            return NULL;
        for (unsigned j = 0; j < i; j++)
            if ((roles & (1u << j)) && files[i].st_dev == files[j].st_dev &&
                files[i].st_ino == files[j].st_ino)
                return NULL;
    }
    static const char resolver[] = "nameserver 10.0.2.3\noptions timeout:2 attempts:2\n";
    char resolver_bytes[sizeof(resolver)];
    if (files[GSTD_MOUNT_RESOLVER].st_size != sizeof(resolver) - 1 ||
        pread(input->role_fds[GSTD_MOUNT_RESOLVER], resolver_bytes, sizeof(resolver_bytes), 0) !=
            sizeof(resolver) - 1 ||
        memcmp(resolver_bytes, resolver, sizeof(resolver) - 1))
        return NULL;
    json_t *spec = json_object(), *process = json_object(), *linux = json_object(),
           *mounts = json_array(), *caps = json_object(), *namespaces = json_array(),
           *policy = gstd_execution_policy(),
           *env = json_pack("[s,s,s,s,s]", "PATH=/nonexistent", "HOME=/tmp", "GOMAXPROCS=1",
                            "GOMEMLIMIT=128MiB", "SQA_OPT_OUT=true"),
           *args = NULL;
    if (!spec || !process || !linux || !mounts || !caps || !namespaces || !policy || !env)
        goto fail;
    const char *command = hydra ? "/usr/bin/hydra" : "/usr/bin/kratos";
    if (migration)
        args = json_pack("[s,s,s,s,s,s]", command, "migrate", "sql", "up", "--yes", "-e");
    else if (hydra)
        args = json_pack("[s,s,s,s,s,s]", command, "serve", "all", "--config",
                         "/run/gitstore-oidc/provider.json", "--dev");
    else if (kratos)
        args = json_pack("[s,s,s,s,s,s,s]", command, "serve", "--config",
                         "/run/gitstore-oidc/provider.json", "--dev", "--watch-courier",
                         "--sqa-opt-out");
    else if (ui)
        args =
            json_pack("[s,s,s]", "/bin/sh", "-c",
                      "child=; stopping=0; "
                      "trap 'stopping=1; case \"$child\" in \"\") ;; *) kill -TERM \"$child\" "
                      "2>/dev/null || : ;; esac' TERM INT; "
                      "set -a; . /run/gitstore-oidc/ui.env || exit 1; "
                      "case \"$stopping\" in 1) exit 0 ;; esac; "
                      "/usr/local/bin/node /usr/src/app/lib/index.js & child=$!; "
                      "case \"$stopping\" in 1) kill -TERM \"$child\" 2>/dev/null || : ;; esac; "
                      "wait \"$child\"; result=$?; "
                      "case \"$stopping\" in 1) wait \"$child\" 2>/dev/null || :; exit 0 ;; esac; "
                      "exit \"$result\"");
    else if (bridge)
        args = json_pack("[s]", "/app/bridge");
    else if (smtp)
        args = json_pack("[s]", "/mailpit");
    if (!args)
        goto fail;
    if (migration &&
        json_array_append_new(env, json_string("DSN=sqlite:///work/provider.sqlite?_fk=true")))
        goto fail;
    char values[8][192];
    size_t count = 0;
    if (bridge) {
        snprintf(values[count++], sizeof(values[0]), "GITSTORE_OIDC_BRIDGE__LISTEN_PORT=%u",
                 input->oidc_ports->bridge);
        snprintf(values[count++], sizeof(values[0]),
                 "GITSTORE_OIDC_BRIDGE__HYDRA__ADMIN_URI=http://127.0.0.1:4445");
        snprintf(values[count++], sizeof(values[0]),
                 "GITSTORE_OIDC_BRIDGE__KRATOS__PUBLIC_URI=http://127.0.0.1:%u",
                 input->oidc_ports->identity);
        snprintf(values[count++], sizeof(values[0]),
                 "GITSTORE_OIDC_BRIDGE__KRATOS__PUBLIC_BROWSER_URI=http://127.0.0.1:%u",
                 input->oidc_ports->identity);
        snprintf(values[count++], sizeof(values[0]),
                 "GITSTORE_OIDC_BRIDGE__KRATOS__ADMIN_URI=http://127.0.0.1:4434");
        snprintf(values[count++], sizeof(values[0]),
                 "GITSTORE_OIDC_BRIDGE__HYDRA__DEFAULT_AUDIENCE=gitstore");
    }
    if (smtp) {
        snprintf(values[count++], sizeof(values[0]), "MP_SMTP_BIND_ADDR=127.0.0.1:1025");
        snprintf(values[count++], sizeof(values[0]), "MP_UI_BIND_ADDR=0.0.0.0:%u",
                 input->oidc_ports->mail);
        snprintf(values[count++], sizeof(values[0]), "MP_MAX_MESSAGES=25");
        snprintf(values[count++], sizeof(values[0]), "MP_MAX_MESSAGE_SIZE=1");
        snprintf(values[count++], sizeof(values[0]), "MP_DISABLE_VERSION_CHECK=true");
        snprintf(values[count++], sizeof(values[0]), "MP_SMTP_DISABLE_RDNS=true");
        snprintf(values[count++], sizeof(values[0]), "MP_QUIET=true");
        snprintf(values[count++], sizeof(values[0]), "MP_ALLOWED_HOSTS=127.0.0.1:%u",
                 input->oidc_ports->mail);
    }
    for (size_t i = 0; i < count; i++)
        if (json_array_append_new(env, json_string(values[i])))
            goto fail;
    const char *sets[] = {"bounding", "effective", "inheritable", "permitted", "ambient"};
    for (size_t i = 0; i < 5; i++)
        if (json_object_set_new(caps, sets[i], json_array()))
            goto fail;
    const char *types[] = {"user", "pid", "mount", "uts", "ipc", "network"};
    for (size_t i = 0; i < 6; i++) {
        json_t *entry = json_pack("{s:s}", "type", types[i]);
        if (entry && (i == 0 || i == 5)) {
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
    const char *temps[] = {"/dev", "/run", "/tmp"},
               *options[] = {"size=4194304,mode=0755", "size=4194304,mode=0700",
                             "size=67108864,mode=1777"};
    for (size_t i = 0; i < 3; i++)
        if (json_array_append_new(mounts, json_pack("{s:s,s:s,s:s,s:[s,s,s]}", "destination",
                                                    temps[i], "type", "tmpfs", "source", "tmpfs",
                                                    "options", "nosuid", "noexec", options[i])))
            goto fail;
    if (json_array_append_new(mounts, json_pack("{s:s,s:s,s:s,s:[s,s,s,s]}", "destination", "/proc",
                                                "type", "proc", "source", "proc", "options",
                                                "nosuid", "nodev", "noexec", "ro")) ||
        mount_file(mounts, "/etc/resolv.conf", sources[GSTD_MOUNT_RESOLVER], 0))
        goto fail;
    if ((roles & (1u << GSTD_MOUNT_OVERLAY)) &&
        mount_file(mounts, ui ? "/run/gitstore-oidc/ui.env" : "/run/gitstore-oidc/provider.json",
                   sources[GSTD_MOUNT_OVERLAY], 0))
        goto fail;
    if ((roles & (1u << GSTD_MOUNT_POLICY)) &&
        mount_file(mounts, "/run/gitstore-oidc/identity.schema.json", sources[GSTD_MOUNT_POLICY],
                   0))
        goto fail;
    if ((hydra || kratos) && mount_file(mounts, "/work", sources[GSTD_MOUNT_DATASTORE], 1))
        goto fail;
    if (json_object_set_new(process, "terminal", json_false()) ||
        json_object_set_new(process, "user", json_pack("{s:i,s:i}", "uid", 0, "gid", 0)) ||
        json_object_set(process, "args", args) || json_object_set(process, "env", env) ||
        json_object_set(process, "capabilities", caps) ||
        json_object_set_new(process, "cwd", json_string(ui ? "/usr/src/app" : "/")) ||
        json_object_set_new(process, "noNewPrivileges", json_true()) ||
        json_object_set_new(process, "rlimits",
                            json_pack("[{s:s,s:I,s:I},{s:s,s:i,s:i}]", "type", "RLIMIT_AS", "soft",
                                      (json_int_t)(UINT64_C(1) << 40), "hard",
                                      (json_int_t)(UINT64_C(1) << 40), "type", "RLIMIT_CORE",
                                      "soft", 0, "hard", 0)) ||
        json_object_set(linux, "namespaces", namespaces) ||
        json_object_set(linux, "seccomp", policy) ||
        json_object_set_new(linux, "resources",
                            json_pack("{s:{s:i},s:{s:i},s:{s:i,s:i}}", "memory", "limit", 536870912,
                                      "pids", "limit", 256, "cpu", "quota", 100000, "period",
                                      100000)) ||
        json_object_set_new(spec, "ociVersion", json_string("1.0.2")) ||
        json_object_set_new(spec, "root", json_pack("{s:s,s:b}", "path", root, "readonly", 1)) ||
        json_object_set(spec, "process", process) || json_object_set(spec, "linux", linux) ||
        json_object_set(spec, "mounts", mounts))
        goto fail;
    json_decref(process);
    json_decref(linux);
    json_decref(mounts);
    json_decref(caps);
    json_decref(namespaces);
    json_decref(policy);
    json_decref(env);
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
    json_decref(env);
    json_decref(args);
#endif
    return NULL;
}
