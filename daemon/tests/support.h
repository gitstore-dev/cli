#ifndef GITSTORED_TEST_SUPPORT_H
#define GITSTORED_TEST_SUPPORT_H
/* Fixtures are private mkdtemp roots; callers must check failures. */
char *gstd_test_dir(void);
int gstd_test_remove(char *root);
#endif
