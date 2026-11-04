#include "gitstored/execution.h"
#include "gitstored/protocol.h"
#include "network_sandbox.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d\n", __LINE__);                                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(void) {
    gstd_json_initialize();
    json_t *policy = gstd_execution_policy();
    CHECK(policy &&
          !strcmp(json_string_value(json_object_get(policy, "defaultAction")), "SCMP_ACT_ERRNO"));
    CHECK(json_integer_value(json_object_get(policy, "defaultErrnoRet")) == 1);
    CHECK(json_array_size(json_object_get(policy, "architectures")) == 1);
    CHECK(strlen(gstd_execution_policy_digest()) == 71);
    json_t *rules = json_object_get(policy, "syscalls");
    CHECK(json_is_array(rules) && json_array_size(rules) > 5);
    int clone3_denied = 0;
    for (size_t i = 0; i < json_array_size(rules); i++) {
        json_t *rule = json_array_get(rules, i), *names = json_object_get(rule, "names");
        CHECK(!json_object_get(rule, "includes") && !json_object_get(rule, "excludes"));
        const char *action = json_string_value(json_object_get(rule, "action"));
        CHECK(action);
        for (size_t j = 0; j < json_array_size(names); j++) {
            const char *name = json_string_value(json_array_get(names, j));
            CHECK(name);
            if (!strcmp(action, "SCMP_ACT_ALLOW"))
                CHECK(strcmp(name, "mount") && strcmp(name, "unshare") && strcmp(name, "setns") &&
                      strcmp(name, "bpf") && strcmp(name, "keyctl"));
            if (!strcmp(name, "clone3"))
                clone3_denied = !strcmp(action, "SCMP_ACT_ERRNO") &&
                                json_integer_value(json_object_get(rule, "errnoRet")) == 38;
        }
    }
    CHECK(clone3_denied);
    json_decref(policy);
    policy = gstd_network_worker_policy();
    CHECK(policy);
    rules = json_object_get(policy, "syscalls");
    unsigned sockets = 0;
    for (size_t i = 0; i < json_array_size(rules); i++) {
        json_t *rule = json_array_get(rules, i), *names = json_object_get(rule, "names");
        for (size_t j = 0; j < json_array_size(names); j++) {
            const char *name = json_string_value(json_array_get(names, j));
            CHECK(strcmp(name, "execve") && strcmp(name, "execveat") && strcmp(name, "clone") &&
                  strcmp(name, "clone3") && strcmp(name, "fork") && strcmp(name, "vfork") &&
                  strcmp(name, "socketpair") && strcmp(name, "ptrace") && strcmp(name, "setns") &&
                  strcmp(name, "chroot") && strcmp(name, "unshare") && strcmp(name, "pidfd_getfd"));
            if (!strcmp(name, "socket")) {
                json_t *args = json_object_get(rule, "args");
                CHECK(json_array_size(args) == 1);
                json_t *arg = json_array_get(args, 0);
                json_int_t family = json_integer_value(json_object_get(arg, "value"));
                CHECK((family == 2 || family == 10) &&
                      !strcmp(json_string_value(json_object_get(arg, "op")), "SCMP_CMP_EQ"));
                sockets++;
            }
        }
    }
    CHECK(sockets == 2);
    json_decref(policy);
    unsigned delay = 999;
    CHECK(gstd_execution_backoff(0, 0, &delay) == -1 && !delay);
    uint64_t elapsed = 0;
    unsigned attempts = 1;
    while (gstd_execution_backoff(attempts, elapsed, &delay) == 1) {
        CHECK(delay && delay <= 2000);
        elapsed += delay;
        CHECK(elapsed <= 10000 && attempts <= 4);
        attempts++;
    }
    CHECK(attempts == 5 && !delay);
    CHECK(!gstd_execution_backoff(1, 9999, &delay) && !delay);
#ifndef __linux__
    CHECK(!gstd_execution_kernel_supported());
#endif
    return 0;
}
