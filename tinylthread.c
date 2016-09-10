#include "tinylthread.h"



TINYLTHREAD_API int luaopen_tinylthread( lua_State* L ) {

  return 0;
}


#if !defined( __STDC_VERSION__ ) || \
    __STDC_VERSION__ < 201112L || \
    defined( __STDC_NO_THREADS__ )
/* make building easier by including the source of the C11 threads
 * emulation library tinycthread */
#  include <tinycthread.c>
#endif

