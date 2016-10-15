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
#  include <time.h>
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
#define TLT_INTERRUPT   "tinylthread.interrupt.error"
#define TLT_C_API_V1    "tinylthread.c.api.v1"


/* common header for all objects/userdatas that may be shared over
 * multiple threads, and thus need reference counting */
typedef struct {
  size_t cnt;
  mtx_t mtx;
} tinylheader;


/* a structure that contains all information about where a
 * tinylthread is currently blocked */
typedef struct {
  tinylheader* header;
  cnd_t* condition;
  mtx_t* mutex;
} tinylblock;


/* shared part of thread handle userdata type */
typedef struct {
  tinylheader ref;
  thrd_t thread;
  mtx_t mutex;
  tinylblock block;  /* contains info if thread is blocked */
  lua_State* L;  /* as long as it lives only the child may access L */
  int  exit_status;
  char is_detached;
  char is_interrupted;
  char ignore_interrupt;
} tinylthread_shared;

/* thread handle userdata type */
typedef struct {
  tinylthread_shared* s;
  char is_parent;
} tinylthread;


/* shared part of the mutex handle */
typedef struct {
  tinylheader ref;
  mtx_t mutex;
  cnd_t unlocked;
  size_t count;
} tinylmutex_shared;

/* mutex handle userdata type */
typedef struct {
  tinylmutex_shared* s;
  char is_owner;
} tinylmutex;


/* shared part of port userdata type
 *
 * receiver:
 * - lock the mutex
 * - while L != NULL and wports > 0 wait on waiting_receivers
 * - raise error if wports == 0 or interrupted
 * - set L and signal waiting_senders
 * - wait on data_copied
 * - set L to NULL
 * - signal waiting_receivers
 * - raise error if (wports == 0 and no value) or interrupted
 *
 * sender:
 * - lock the mutex
 * - while L == NULL and rports > 0 wait on waiting_senders
 * - raise error if rports == 0 or (interrupted and L == 0)
 * - copy value to L
 * - signal data_copied
 * - raise error if interrupted
 */
typedef struct {
  tinylheader ref;
  mtx_t mutex;
  cnd_t data_copied;
  cnd_t waiting_senders;
  cnd_t waiting_receivers;
  lua_State* L;  /* L of current receiver */
  size_t rports;
  size_t wports;
} tinylport_shared;

/* port userdata type */
typedef struct {
  tinylport_shared* s;
  char is_reader;
} tinylport;



/* a C API for other extension modules */
typedef struct {
  unsigned version;

} tinylthread_c_api_v1;

/* minor version of the v1 C API */
#define TLT_C_API_V1_MINOR  0


/* function pointer for copying certain userdata values to the Lua
 * states of other threads */
typedef int (*tinylport_copyf)( void* ud, lua_State* L, int midx );


#endif /* TINYLTHREAD_H_ */

