#ifndef TINYLTHREAD_H_
#define TINYLTHREAD_H_


#include <stddef.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#if defined( __STDC_VERSION__ ) && \
    __STDC_VERSION__ >= 201112L && \
    !defined( __STDC_NO_THREADS__ ) /* use C11 threads */
#  include <threads.h>
#else /* use C11 threads emulation tinycthreads */
#  include <tinycthread.h>
#endif


#ifndef TINYLTHREAD_API
#  ifdef _WIN32
#    define TINYLTHREAD_API __declspec(dllexport)
#  else
#    define TINYLTHREAD_API extern
#  endif
#endif


/* common header for all objects/userdatas that may be shared over
 * multiple threads, and thus need reference counting
 */
typedef struct {
  size_t refcnt;
  mtx_t refcnt_mutex;
} tinylheader;


/* thread handle userdata type */
typedef struct {
  mtx_t thread_mutex;
  thrd_t thread;
  lua_State* L;
  char is_interrupted;
  char is_joinable;
  char is_dead;
} tinylthread;


/* mutex handle userdata type */
typedef struct {
  tinylheader ref;
  mtx_t mutex;
} tinylmutex;


/* channel userdata type */
typedef struct {
  tinylheader ref;
  mtx_t channel_mutex;
  cnd_t can_read;
  cnd_t can_write;
  lua_State* L;
  size_t capacity;
  size_t readpos;
  size_t writepos;
  int lreferences[ 1 ]; /* variable length array! */
} tinylchannel;


#endif /* TINYLTHREAD_H_ */

