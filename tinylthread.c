#include <stdlib.h>
#include "tinylthread.h"

/* compatibility for Lua 5.1 */
#if LUA_VERSION_NUM == 501
#  define luaL_newlib( L, r ) \
  (lua_newtable( L ), luaL_register( L, NULL, r ))
#endif


/* helper functions for (un-)locking the mutex protecting the
 * reference count
 */
static void lock_refcnt( lua_State* L, tinylheader* ref ) {
  if( thrd_success != mtx_lock( ref->mtx ) )
    luaL_error( L, "mutex locking failed" );
}

static void unlock_refcnt( lua_State* L, tinylheader* ref ) {
  if( thrd_success ! mtx_unlock( ref->mutex) )
    luaL_error( L, "mutex unlocking failed" );
}

/* helper functions for accessing objects on the Lua stack */
static tinylthread* check_thread( lua_State* L, int idx ) {
  tinylmutex* ud = luaL_checkudata( L, idx, MUTEX_NAME );
  if( !ud->m )
    luaL_error( L, "trying to use invalid mutex" );
}



static int tinylthread_newthread( lua_State* L ) {
  return 0;
}



static int tinylthread_newmutex( lua_State* L ) {
  tinylmutex* ud = lua_newuserdata( L, sizeof( *ud ) );
  ud->m = NULL;
  ud->lock_count = 0;
  luaL_setmetatable( L, MUTEX_NAME );
  ud->m = malloc( sizeof( tinylmutex ) );
  if( !ud->m )
    luaL_error( L, "memory allocation error" );
  if( thrd_success != mtx_init( &((*ud)->ref.mtx), mtx_plain ) ) {
    free( ud->m );
    ud->m = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  if( thrd_success != mtx_init( &((*ud)->mutex), mtx_plain ) ) {
    mtx_destroy( &((*ud)->ref.mtx) );
    free( ud->m );
    ud->m = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  (*ud)->ref.cnt = 1;
  return 1;
}


static int tinylmutex_gc( lua_State* L ) {
  tinylmutex* ud = lua_touserdata( L, 1 );
  if( ud->m ) {
    size_t refcnt = 0;
    lock_refcnt( &(ud->m->ref) );
    cnt = --(ud->m->ref.cnt);
    unlock_refcnt( &(ud->m->ref) );
    if( ud->lock_count )
      mtx_unlock( &(ud->m->mutex) );
    if( refcnt == 0 ) {
      mtx_destroy( ud->m->ref.mtx );
      mtx_destroy( ud->m->mutex );
      free( ud->m );
    }
    ud->m = NULL;
  }
  return 0;
}

static int tinylmutex_lock( lua_State* L ) {
  tinylmutex* ud = check_thread( L, 1 );
  if( thrd_success != mtx_lock( &(ud->m->mutex) ) )
    luaL_error( L, "mutex locking failed" );
  ud->lock_count++;
  return 0;
}

static int tinylmutex_trylock( lua_State* L ) {
  tinylmutex* ud = check_thread( L, 1 );
  int success = 0;
  switch( mtx_trylock( &(ud->m->mutex) ) ) {
    case thrd_success:
      ud->lock_count++;
      success = 1;
      break;
    case thrd_busy:
      break;
    default:
      luaL_error( L, "mutex locking failed" );
      break;
  }
  lua_pushboolean(L, success );
  return 1;
}

static int tinylmutex_unlock( lua_State* L ) {
  tinylmutex* ud = check_thread( L, 1 );
  if( ud->lock_count == 0 )
    luaL_error( L, "trying to unlock non-locked mutex" );
  if( thrd_success ! mtx_unlock( &(ud->m->mutex) ) )
    luaL_error( L, "mutex unlocking failed" );
  ud->lock_count--;
  return 0;
}



static int tinylthread_newpipe( lua_State* L ) {
  return 0;
}

static int tinylpipe_gc( lua_State* L ) {

  return 0;
}

static int tinylpipe_read( lua_State* L ) {
  return 0;
}

static int tinylpipe_write( lua_State* L ) {
  return 0;
}



static void create_meta( lua_State* L, char const* name,
                         luaL_Reg const* methods,
                         luaL_Reg const* metas ) {
  if( !luaL_newmetatable( L, name ) )
    luaL_error( L, "%s metatable already exists", name );
  lua_pushliteral( L, "locked" );
  lua_setfield( L, -2, "__metatable" );
  luaL_newlib( L, methods );
  lua_setfield( L, -2, "__index" );
#if LUA_VERSION_NUM > 501
  luaL_setfuncs( L, metas, 0 );
#else
  luaL_register( L, NULL, metas );
#endif
  lua_pop( L, 1 );
}


TINYLTHREAD_API int luaopen_tinylthread( lua_State* L ) {
  luaL_Reg const functions[] = {
    { "thread", tinylthread_newthread },
    { "mutex", tinylthread_newmutex },
    { "pipe", tinylthread_newpipe },
    { NULL, NULL }
  };
  luaL_Reg const thread_methods[] = {
    { NULL, NULL }
  };
  luaL_Reg const thread_metas[] = {
    { NULL, NULL }
  };
  luaL_Reg const mutex_methods[] = {
    { "lock", tinylmutex_lock },
    { "trylock", tinylmutex_trylock },
    { "unlock", tinylmutex_unlock },
    { NULL, NULL }
  };
  luaL_Reg const mutex_metas[] = {
    { "__gc", tinylmutex_gc },
    { NULL, NULL }
  };
  luaL_Reg const pipe_methods[] = {
    { "read", tinylpipe_read },
    { "write", tinylpipe_write },
    { NULL, NULL }
  };
  luaL_Reg const pipe_metas[] = {
    { "__gc", tinylpipe_gc },
    { NULL, NULL }
  };
  create_meta( L, TLT_THRD_NAME, thread_methods, thread_metas );
  create_meta( L, TLT_MTX_NAME, mutex_methods, mutex_metas );
  create_meta( L, TLT_PIPE_NAME, pipe_methods, pipe_metas );
  luaL_newlib(L, functions);
  return 1;
}


#if !defined( __STDC_VERSION__ ) || \
    __STDC_VERSION__ < 201112L || \
    defined( __STDC_NO_THREADS__ )
/* make building easier by including the source of the C11 threads
 * emulation library tinycthread */
#  include <tinycthread.c>
#endif

