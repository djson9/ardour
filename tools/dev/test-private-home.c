#include <stdlib.h>
extern const char *g_get_home_dir(void);
static const char *private_home(void){ const char *p=getenv("ARDOUR_PROBE_HOME"); if(!p) abort(); return p; }
__attribute__((used,section("__DATA,__interpose"))) static const struct { const void *replacement; const void *original; } hook={(void*)private_home,(void*)g_get_home_dir};
