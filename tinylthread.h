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


/* metatable names */
#define TLT_THRD_NAME   "tinylthread.thread"
#define TLT_MTX_NAME    "tinylthread.mutex"
#define TLT_RPORT_NAME  "tinylthread.port.in"
#define TLT_WPORT_NAME  "tinylthread.port.out"
#define TLT_ITR_NAME    "tinylthread.interrupt"

/* other important keys in the registry */
#define TLT_THISTHREAD  "tinylthread.this"
#define TLT_THUNK       "tinylthread.thunk"
#define TLT_INTERRUPTED "tinylthread.interrupt.err"


/* common header for all objects/userdatas that may be shared over
 * multiple threads, and thus need reference counting */
typedef struct {
  size_t cnt;
  mtx_t mtx;
} tinylheader;


/* thread handle userdata type */
typedef struct {
  tinylheader ref;
  thrd_t thread;
  mtx_t thread_mutex;
  lua_State* L;
  int exit_status;
  char is_interrupted;
  char is_detached;
  char is_dead;
} tinylthread_shared;

typedef struct {
  tinylthread_shared* t;
  char is_parent;
} tinylthread;


/* shared part of the mutex handle */
typedef struct {
  tinylheader ref;
  mtx_t mutex;
} tinylmutex_shared;

/* mutex handle userdata type */
typedef struct {
  tinylmutex_shared* m;
  size_t lock_count;
} tinylmutex;


/* port userdata type */
typedef struct {
  tinylheader ref;
  mtx_t port_mutex;
  cnd_t data_available;
  cnd_t write_finished;
  lua_State* L;
  size_t readers;
  size_t writers;
} tinylport;


/* function pointer for copying certain userdata values to the Lua
 * states of other threads */
typedef int (*tinylpipe_copyf)( void* ud, lua_State* L, int midx );


#endif /* TINYLTHREAD_H_ */

