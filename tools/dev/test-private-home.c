/* SPDX-License-Identifier: GPL-2.0-or-later
 * Process-local test preferences and POSIX semaphore names on macOS. */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>

extern const char *g_get_home_dir (void);
static const char *private_home (void)
{
 const char *path = getenv ("ARDOUR_PROBE_HOME");
 if (!path) abort ();
 return path;
}
static void semaphore_name (char name[32], const char *original)
{
 uint64_t hash = 14695981039346656037ULL;
 for (const unsigned char *p = (const unsigned char *) original; *p; ++p) {
  hash ^= *p;
  hash *= 1099511628211ULL;
 }
 if (snprintf (name, 32, "/ap%08x%016llx", (unsigned) getpid (), (unsigned long long) hash) >= 32) abort ();
}
static sem_t *private_sem_open (const char *original, int flags, ...)
{
 char name[32];
 semaphore_name (name, original);
 if (flags & O_CREAT) {
  va_list args;
  va_start (args, flags);
  int mode = va_arg (args, int);
  unsigned value = va_arg (args, unsigned);
  va_end (args);
  return sem_open (name, flags, mode, value);
 }
 return sem_open (name, flags);
}
static int private_sem_unlink (const char *original)
{
 char name[32];
 semaphore_name (name, original);
 return sem_unlink (name);
}
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement; const void *original; } hooks[] = {
 {(void *) private_home, (void *) g_get_home_dir},
 {(void *) private_sem_open, (void *) sem_open},
 {(void *) private_sem_unlink, (void *) sem_unlink}
};
