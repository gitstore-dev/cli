#ifndef GITSTORED_SERVER_H
#define GITSTORED_SERVER_H
int gstd_serve(const char *runtime_directory, const char *state_directory, const char *entrypoint,
               int readiness_fd);
#endif
