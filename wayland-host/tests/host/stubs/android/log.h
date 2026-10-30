#ifndef TRIERARCH_TEST_ANDROID_LOG_H
#define TRIERARCH_TEST_ANDROID_LOG_H
#include <stdio.h>
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
static inline int __android_log_print(int priority, const char *tag, const char *format, ...) {
    (void)priority;
    fprintf(stderr, "[%s] ", tag);
    va_list args;
    va_start(args, format);
    int result = vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    return result;
}
#endif
