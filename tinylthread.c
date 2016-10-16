#include <assert.h>
#include <stdlib.h>
#include <math.h>
#include "tinylthread.h"



/* compatibility for older Lua versions */
#if LUA_VERSION_NUM <= 501
#  define luaL_setmetatable( L, tn ) \
  (luaL_getmetatable( L, tn ), lua_setmetatable( L, -2 ))
#endif

#if LUA_VERSION_NUM <= 502
static int lua_isinteger( lua_State* L, int idx ) {
  if( lua_type( L, idx ) == LUA_TNUMBER ) {
    lua_Number n = lua_tonumber( L, idx );
    lua_Integer i = lua_tointeger( L, idx );
    if( i == n )
      return 1;
  }
  return 0;
}
#endif


#if LUA_VERSION_NUM == 501
typedef struct {
  void* key;
  lua_CFunction f;
} lua_cpcallr_data;

static int lua_cpcallr_helper( lua_State* L ) {
  lua_cpcallr_data* data = lua_touserdata( L, 1 );
  lua_pushlightuserdata( L, data->key );
  lua_pushcfunction( L, data->f );
  lua_rawset( L, LUA_REGISTRYINDEX );
  return 0;
}

/* similar to lua_cpcall but allows return values */
static int lua_cpcallr( lua_State* L, lua_CFunction f, void* u, int ret ) {
  char xyz[ 1 ] = { 0 }; /* only used for its pointer value */
  lua_cpcallr_data data = { xyz, f };
  int status = 0;
  if( ret == 0 )
    return lua_cpcall( L, f, u );
  status = lua_cpcall( L, lua_cpcallr_helper, &data );
  if( status )
    return status;
  lua_pushlightuserdata( L, xyz );
  lua_rawget( L, LUA_REGISTRYINDEX );
  lua_pushlightuserdata( L, xyz );
  lua_pushnil( L );
  lua_rawset( L, LUA_REGISTRYINDEX );
  lua_pushlightuserdata( L, u );
  return lua_pcall( L, 1, ret, 0 );
}

static int has_uservalue( lua_State* L, int idx ) {
  (void)L;
  (void)idx;
  /* we can't say for sure in Lua 5.1! */
  return 0;
}
#else
#  define lua_cpcallr( L, f, u, r ) \
  (lua_pushcfunction( L, f ), \
   lua_pushlightuserdata( L, u ), \
   lua_pcall( L, 1, r, 0 ))

static int has_uservalue( lua_State* L, int idx ) {
  int ret = 0;
  lua_getuservalue( L, idx );
  ret = lua_type( L, -1 ) != LUA_TNIL;
  lua_pop( L, 1 );
  return ret;
}
#endif


static void* get_udata_from_registry( lua_State* L, char const* name ) {
  lua_getfield( L, LUA_REGISTRYINDEX, name );
  return lua_touserdata( L, -1 );
}


/* in many circumstances failure to use a synchronization primitive
 * should be fatal (i.e. raise a Lua error or even abort). */
static void mtx_lock_or_throw( lua_State* L, mtx_t* m ) {
  if( thrd_success != mtx_lock( m ) )
    luaL_error( L, "locking mutex failed" );
}

#define no_fail( _call ) \
  do { \
    int ok = thrd_success == _call; \
    assert( ok && #_call ); \
    (void)ok; \
  } while( 0 )


/* helper functions for managing the reference count of shared
 * objects */
static void increment_ref_count( lua_State* L, tinylheader* ref ) {
  no_fail( mtx_lock( &(ref->mtx) ) );
  ref->cnt++;
  no_fail( mtx_unlock( &(ref->mtx) ) );
}

static size_t decrement_ref_count( lua_State* L, tinylheader* ref ) {
  size_t newcnt = 0;
  no_fail( mtx_lock( &(ref->mtx) ) );
  newcnt = --(ref->cnt);
  no_fail( mtx_unlock( &(ref->mtx) ) );
  return newcnt;
}

/* helper functions for accessing objects on the Lua stack */
static tinylthread* check_thread( lua_State* L, int idx ) {
  tinylthread* thread = luaL_checkudata( L, idx, TLT_THRD_NAME );
  if( !thread->s )
    luaL_error( L, "attempt to use invalid thread" );
  return thread;
}

static tinylmutex* check_mutex( lua_State* L, int idx ) {
  tinylmutex* mutex = luaL_checkudata( L, idx, TLT_MTX_NAME );
  if( !mutex->s )
    luaL_error( L, "attempt to use invalid mutex" );
  return mutex;
}

static tinylport* check_rport( lua_State* L, int idx ) {
  tinylport* port = luaL_checkudata( L, idx, TLT_RPORT_NAME );
  if( !port->s )
    luaL_error( L, "attempt to use invalid port" );
  return port;
}

static tinylport* check_wport( lua_State* L, int idx ) {
  tinylport* port = luaL_checkudata( L, idx, TLT_WPORT_NAME );
  if( !port->s )
    luaL_error( L, "attempt to use invalid port" );
  return port;
}



static int is_interrupted( tinylthread* thread, int* disabled ) {
  int v = 0;
  int dummy = 0;
  if( !disabled )
    disabled = &dummy;
  if( thread != NULL ) {
    no_fail( mtx_lock( &(thread->s->mutex) ) );
    *disabled |= thread->s->ignore_interrupt;
    if( thread->s->is_interrupted ) {
      if( !*disabled )
        v = 1;
    }
    thread->s->ignore_interrupt = 0;
    no_fail( mtx_unlock( &(thread->s->mutex) ) );
  }
  return v;
}

static void throw_interrupt( lua_State* L ) {
  get_udata_from_registry( L, TLT_INTERRUPT );
  lua_error( L );
}


static void set_block( tinylthread* thread, tinylheader* h,
                       cnd_t* c, mtx_t* m ) {
  if( thread != NULL ) {
    no_fail( mtx_lock( &(thread->s->mutex) ) );
    thread->s->block.header = h;
    thread->s->block.condition = c;
    thread->s->block.mutex = m;
    no_fail( mtx_unlock( &(thread->s->mutex) ) );
  }
}


static int cleanup_thread( lua_State* L ) {
  int* memory_problem = lua_touserdata( L, 1 );
  tinylthread* thread = get_udata_from_registry( L, TLT_THISTHREAD );
  int is_detached = 1;
  lua_settop( L, 0 );
  if( thread != NULL &&
      thrd_success == mtx_lock( &(thread->s->mutex) ) ) {
    is_detached = thread->s->is_detached;
    no_fail( mtx_unlock( &(thread->s->mutex) ) );
  }
  lua_settop( L, 0 );
  if( is_detached ) {
    /* the Lua states of detached threads are never closed, because
     * the last detached thread would unload its own thread main
     * function while running, so we clean up as best as we can ... */
    lua_gc( L, LUA_GCCOLLECT, 0 );
    lua_gc( L, LUA_GCCOLLECT, 0 );
  }
  if( *memory_problem ) {
    lua_pushliteral( L, "memory allocation error" );
    return 1;
  }
  return 0;
}


static int tinylthread_thunk( void* arg ) {
  lua_State* L = arg;
  int memprob = 0;
  int status = lua_pcall( L, lua_gettop( L )-1, LUA_MULTRET, 0 );
  if( !lua_checkstack( L, 5+LUA_MINSTACK ) ) {
    memprob = 1;
    lua_settop( L, 0 ); /* make room */
  }
  if( 0 != lua_cpcallr( L, cleanup_thread, &memprob, !!memprob ) &&
      !memprob )
    lua_pop( L, 1 ); /* pop error message if necessary */
  return memprob ? LUA_ERRMEM : status;
}


static int copy_primitive( lua_State* toL, lua_State* fromL, int i ) {
  switch( lua_type( fromL, i ) ) {
    case LUA_TNIL:
      lua_pushnil( toL );
      return 1;
    case LUA_TBOOLEAN:
      lua_pushboolean( toL, lua_toboolean( fromL, i ) );
      return 1;
    case LUA_TSTRING: {
        size_t len = 0;
        char const* s = lua_tolstring( fromL, i, &len );
        lua_pushlstring( toL, s, len );
      }
      return 1;
    case LUA_TNUMBER:
      if( lua_isinteger( fromL, i ) )
        lua_pushinteger( toL, lua_tointeger( fromL, i ) );
      else
        lua_pushnumber( toL, lua_tonumber( fromL, i ) );
      return 1;
  }
  return 0;
}

static int copy_udata( lua_State* toL, lua_State* fromL, int i ) {
  /* check that the userdata has `__name` and `__copy@tinylthread`
   * metafields, and that there exists a corresponding metatable
   * in the registry of the target Lua state */
  int result = 0;
  if( lua_type( fromL, i ) == LUA_TUSERDATA &&
      !has_uservalue( fromL, i ) ) {
    lua_getmetatable( fromL, i );
    if( lua_istable( fromL, -1 ) ) {
      tinylport_copyf copyf = 0;
      lua_pushliteral( fromL, "__name" );
      lua_rawget( fromL, -2 );
      lua_pushliteral( fromL, "__copy@tinylthread" );
      lua_rawget( fromL, -3 );
      copyf = (tinylport_copyf)lua_tocfunction( fromL, -1 );
      lua_pop( fromL, 1 ); /* copyf */
      if( copyf != 0 && lua_type( fromL, -1 ) == LUA_TSTRING ) {
        int equal = 0;
        /* make sure __name really refers to this metatable */
        lua_pushvalue( fromL, -1 );
        lua_rawget( fromL, LUA_REGISTRYINDEX );
        equal = lua_rawequal( fromL, -1, -3 );
        lua_pop( fromL, 1 ); /* pop 2nd metatable */
        if( equal ) {
          size_t len = 0;
          char const* name = lua_tolstring( fromL, -1, &len );
          lua_pushlstring( toL, name, len );
          lua_rawget( toL, LUA_REGISTRYINDEX );
          if( lua_istable( toL, -1 ) ) {
            int top = lua_gettop( toL );
            if( copyf( lua_touserdata( fromL, i ), toL, top ) ) {
              lua_insert( toL, top );
              lua_settop( toL, top );
              result = 1;
            }
          }
        }
      }
      lua_pop( fromL, 1 ); /* pop name */
    }
    lua_pop( fromL, 1 ); /* pop metatable */
  }
  return result;
}

static int copy_table( lua_State* toL, lua_State* fromL, int i ) {
  int top = lua_gettop( fromL );
  if( lua_type( fromL, i ) == LUA_TTABLE ) {
    if( !lua_getmetatable( fromL, i ) ) {
      lua_newtable( toL );
      lua_pushnil( fromL );
      while( lua_next( fromL, i ) != 0 ) {
        if( !copy_primitive( toL, fromL, top+1 ) ) {
          lua_pop( fromL, 2 );
          lua_pop( toL, 1 );
          return 0;
        }
        if( !copy_primitive( toL, fromL, top+2 ) &&
            !copy_udata( toL, fromL, top+2 ) ) {
          lua_pop( fromL, 2 );
          lua_pop( toL, 2 );
          return 0;
        }
        lua_rawset( toL, -3 );
        lua_pop( fromL, 1 );
      }
      return 1;
    } else
      lua_pop( fromL, 1 );
  }
  return 0;
}


static void copy_value_to_thread( lua_State* toL, lua_State* fromL, int i ) {
  if( !copy_primitive( toL, fromL, i ) &&
      !copy_udata( toL, fromL, i ) &&
      !copy_table( toL, fromL, i ) ) {
    luaL_error( toL, "bad value #%d (unsupported type: '%s')",
                i, luaL_typename( fromL, i ) );
  }
}


/* all API calls on childL are protected, but all API calls on
 * parentL are *unprotected* and could cause resource leaks if
 * an unhandled error is thrown! */
static int prepare_thread_state( lua_State* childL ) {
  lua_State* parentL = lua_touserdata( childL, 1 );
  size_t len = 0;
  char const* s = NULL;
  int i = 1;
  int top = 0;
  lua_pop( childL, 1 );
  luaL_openlibs( childL );
  /* take package (c)path from parent thread */
  lua_getglobal( childL, "package" );
  if( lua_istable( childL, -1 ) ) {
    if( lua_type( parentL, -2 ) == LUA_TSTRING ) {
      s = lua_tolstring( parentL, -2, &len );
      lua_pushlstring( childL, s, len );
      lua_setfield( childL, -2, "path" );
    }
    if( lua_type( parentL, -1 ) == LUA_TSTRING ) {
      s = lua_tolstring( parentL, -1, &len );
      lua_pushlstring( childL, s, len );
      lua_setfield( childL, -2, "cpath" );
    }
  }
  lua_pop( parentL, 2 ); /* remove package.(c)path */
  lua_pop( childL, 1 ); /* remove package table */
  /* require this library (go through the Lua `require` function so
   * that the library handle is added to this Lua state!) */
  lua_getglobal( childL, "require" );
  if( !lua_isfunction( childL, -1 ) )
    luaL_error( childL, "tinylthread initialization failed" );
  lua_pushliteral( childL, "tinylthread" );
  lua_call( childL, 1, 0 );
  /* copy arguments from the lua_State of the parent */
  top = lua_gettop( parentL );
  luaL_checkstack( childL, top+LUA_MINSTACK, "prepare_thread_state" );
  for( i = 1; i <= top; ++i )
    copy_value_to_thread( childL, parentL, i );
  /* store away the child thread handle */
  lua_setfield( childL, LUA_REGISTRYINDEX, TLT_THISTHREAD );
  /* load lua code for the thread main function */
  s = lua_tolstring( childL, 1, &len );
  if( 0 != luaL_loadbuffer( childL, s, len, "=threadmain" ) )
    lua_error( childL );
  lua_replace( childL, 1 );
  return lua_gettop( childL );
}


/* all API calls on L are protected, but all API calls on fromL are
 * *unprotected* and could cause resource leaks if an unhandled
 * error is thrown! */
static int copy_stack_top( lua_State* L ) {
  lua_State* fromL = lua_touserdata( L, 1 );
  int top = lua_gettop( fromL );
  if( top < 1 )
    lua_pushnil( L );
  else
    copy_value_to_thread( L, fromL, top );
  return 1;
}


static int tinylthread_new_thread( lua_State* L ) {
  tinylthread* thread = NULL;
  thrd_start_t thunk = 0;
  lua_State* tempL = NULL;
  luaL_checkstring( L, 1 ); /* the Lua code */
  thread = lua_newuserdata( L, sizeof( *thread ) );
  thread->s = NULL;
  thread->is_parent = 1;
  luaL_setmetatable( L, TLT_THRD_NAME );
  /* push package.(c)path to the stack for the child thread to copy */
  lua_getglobal( L, "package" );
  if( lua_istable( L, -1 ) ) {
    lua_getfield( L, -1, "cpath" );
    lua_getfield( L, -2, "path" );
  } else {
    lua_pushnil( L );
    lua_pushnil( L );
  }
  lua_replace( L, -3 ); /* remove package table */
  thread->s = malloc( sizeof( *thread->s ) );
  if( !thread->s )
    luaL_error( L, "memory allocation error" );
  thread->s->block.header = NULL;
  thread->s->block.condition = NULL;
  thread->s->block.mutex = NULL;
  thread->s->exit_status = 0;
  thread->s->is_detached = 0;
  thread->s->is_interrupted = 0;
  thread->s->ignore_interrupt = 0;
  thread->s->ref.cnt = 1;
  if( thrd_success != mtx_init( &(thread->s->ref.mtx), mtx_plain ) ) {
    free( thread->s );
    thread->s = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  if( thrd_success != mtx_init( &(thread->s->mutex), mtx_plain ) ) {
    mtx_destroy( &(thread->s->ref.mtx ) );
    free( thread->s );
    thread->s = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  thread->s->L = tempL = luaL_newstate();
  if( !tempL )
    luaL_error( L, "memory allocation error" );
  /* prepare the Lua state for the child thread */
  if( 0 != lua_cpcallr( thread->s->L, prepare_thread_state, L,
                        LUA_MULTRET ) ) {
    int r = lua_cpcallr( L, copy_stack_top, thread->s->L, 1 );
    thread->s->L = NULL;
    lua_close( tempL );
    if( r != 0 )
      lua_pushliteral( L, "thread initialization error" );
    lua_error( L ); /* rethrow the error on this thread */
  }
  /* get thread main function from the child's registry (must not
   * raise an error!) */
  lua_getfield( thread->s->L, LUA_REGISTRYINDEX, TLT_THUNK );
  thunk = (thrd_start_t)lua_tocfunction( thread->s->L, -1 );
  lua_pop( thread->s->L, 1 );
  /* lock this structure and create a C thread */
  if( thrd_success != mtx_lock( &(thread->s->mutex) ) ) {
    thread->s->L = NULL;
    lua_close( tempL );
    luaL_error( L, "locking mutex failed" );
  }
  if( thrd_success !=
      thrd_create( &(thread->s->thread), thunk, thread->s->L ) ) {
    no_fail( mtx_unlock( &(thread->s->mutex) ) );
    thread->s->L = NULL;
    lua_close( tempL );
    luaL_error( L, "thread spawning failed" );
  }
  no_fail( mtx_unlock( &(thread->s->mutex) ) );
  return 1;
}


static int tinylthread_copy( void* p, lua_State* L, int midx ) {
  tinylthread* thread = p;
  tinylthread* copy = lua_newuserdata( L, sizeof( *copy ) );
  copy->s = NULL;
  copy->is_parent = 0;
  lua_pushvalue( L, midx );
  lua_setmetatable( L, -2 );
  if( thread->s ) {
    increment_ref_count( L, &(thread->s->ref) );
    copy->s = thread->s;
  }
  return 1;
}


static int tinylthread_gc( lua_State* L ) {
  tinylthread* thread = lua_touserdata( L, 1 );
  if( thread->s ) {
    int raise_error = 0;
    if( thread->is_parent &&
        thrd_success == mtx_lock( &(thread->s->mutex) ) ) {
      raise_error = !thread->s->is_detached && thread->s->L != NULL;
      no_fail( mtx_unlock( &(thread->s->mutex) ) );
    }
    if( 0 == decrement_ref_count( L, &(thread->s->ref) ) ) {
      mtx_destroy( &(thread->s->ref.mtx) );
      mtx_destroy( &(thread->s->mutex) );
      free( thread->s );
      thread->s = NULL;
    }
    if( raise_error )
      luaL_error( L, "collecting non-joined thread" );
  }
  return 0;
}


static int tinylthread_detach( lua_State* L ) {
  tinylthread* thread = check_thread( L, 1 );
  int is_detached = 0;
  int is_joined = 0;
  int status = 0;
  if( !thread->is_parent )
    luaL_error( L, "detach attempt from non-parent thread" );
  mtx_lock_or_throw( L, &(thread->s->mutex) );
  is_detached = thread->s->is_detached;
  is_joined = thread->s->L == NULL;
  if( !is_detached && !is_joined ) {
    status = thrd_detach( thread->s->thread );
    if( status == thrd_success )
      thread->s->is_detached = 1;
  }
  no_fail( mtx_unlock( &(thread->s->mutex) ) );
  if( is_detached )
    luaL_error( L, "attempt to detach an already detached thread" );
  if( is_joined )
    luaL_error( L, "attempt to detach an already joined thread" );
  if( status == thrd_success ) {
    lua_pushboolean( L, 1 );
    return 1;
  } else {
    lua_pushnil( L );
    lua_pushliteral( L, "detaching thread failed" );
    return 2;
  }
}


typedef struct {
  int status;
  lua_State* L;
} join_data;

static int copy_return_values( lua_State* L ) {
  join_data* data = lua_touserdata( L, 1 );
  int i = 1;
  int top = lua_gettop( data->L );
  lua_pop( L, 1 );
  lua_pushboolean( L, data->status == 0 );
  luaL_checkstack( L, top+LUA_MINSTACK, "copy_return_values" );
  for( i = 1; i <= top; ++i )
    copy_value_to_thread( L, data->L, i );
  return lua_gettop( L );
}

static int tinylthread_join( lua_State* L ) {
  tinylthread* thread = check_thread( L, 1 );
  int is_detached = 0;
  int is_joined = 0;
  join_data data = { 0, NULL };
  if( !thread->is_parent )
    luaL_error( L, "join attempt from non-parent thread" );
  mtx_lock_or_throw( L, &(thread->s->mutex) );
  is_detached = thread->s->is_detached;
  is_joined = thread->s->L == NULL;
  no_fail( mtx_unlock( &(thread->s->mutex) ) );
  if( is_detached )
    luaL_error( L, "attempt to join an already detached thread" );
  if( is_joined )
    luaL_error( L, "attempt to join an already joined thread" );
  if( thrd_success != thrd_join( thread->s->thread, &(data.status) ) )
    luaL_error( L, "joining thread failed" );
  no_fail( mtx_lock( &(thread->s->mutex) ) );
  data.L = thread->s->L;
  thread->s->L = NULL;
  no_fail( mtx_unlock( &(thread->s->mutex) ) );
  lua_settop( L, 0 );
  if( 0 != lua_cpcallr( L, copy_return_values, &data, LUA_MULTRET ) ) {
    lua_close( data.L );
    lua_error( L );
  }
  lua_close( data.L );
  return lua_gettop( L );
}


static int tinylthread_interrupt( lua_State* L ) {
  tinylthread* thread = check_thread( L, 1 );
  tinylblock block;
  no_fail( mtx_lock( &(thread->s->mutex) ) );
  thread->s->is_interrupted = 1;
  block = thread->s->block;
  if( block.header ) {
    /* prevent structure containing block from disappearing */
    no_fail( mtx_lock( &(block.header->mtx) ) );
    /* we must release the thread mutex anyway to avoid a deadlock
     * when acquiring the block mutex */
    no_fail( mtx_unlock( &(thread->s->mutex) ) );
    /* acquire the lock for the condition variable to make sure that
     * the thread is actually waiting on it! */
    no_fail( mtx_lock( block.mutex ) );
    /* wake up the thread */
    no_fail( cnd_broadcast( block.condition ) );
    no_fail( mtx_unlock( block.mutex ) );
    no_fail( mtx_unlock( &(block.header->mtx) ) );
  } else
    no_fail( mtx_unlock( &(thread->s->mutex) ) );
  return 0;
}



static int tinylthread_new_mutex( lua_State* L ) {
  tinylmutex* mutex = lua_newuserdata( L, sizeof( *mutex ) );
  mutex->s = NULL;
  mutex->is_owner = 0;
  luaL_setmetatable( L, TLT_MTX_NAME );
  mutex->s = malloc( sizeof( *mutex->s ) );
  if( !mutex->s )
    luaL_error( L, "memory allocation error" );
  mutex->s->ref.cnt = 1;
  mutex->s->count = 0;
  if( thrd_success != mtx_init( &(mutex->s->ref.mtx), mtx_plain ) ) {
    free( mutex->s );
    mutex->s = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  if( thrd_success != mtx_init( &(mutex->s->mutex), mtx_plain ) ) {
    mtx_destroy( &(mutex->s->ref.mtx) );
    free( mutex->s );
    mutex->s = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  if( thrd_success != cnd_init( &(mutex->s->unlocked) ) ) {
    mtx_destroy( &(mutex->s->ref.mtx) );
    mtx_destroy( &(mutex->s->mutex) );
    free( mutex->s );
    mutex->s = NULL;
    luaL_error( L, "condition variable initialization failed" );
  }
  return 1;
}


static int tinylmutex_copy( void* p, lua_State* L, int midx ) {
  tinylmutex* mutex = p;
  tinylmutex* copy = lua_newuserdata( L, sizeof( *copy ) );
  copy->s = NULL;
  copy->is_owner = 0;
  lua_pushvalue( L, midx );
  lua_setmetatable( L, -2 );
  if( mutex->s ) {
    increment_ref_count( L, &(mutex->s->ref) );
    copy->s = mutex->s;
  }
  return 1;
}


static int tinylmutex_gc( lua_State* L ) {
  tinylmutex* mutex = lua_touserdata( L, 1 );
  if( mutex->s ) {
    if( mutex->is_owner ) {
      no_fail( mtx_lock( &(mutex->s->mutex) ) );
      mutex->s->count = 0;
      no_fail( cnd_signal( &(mutex->s->unlocked) ) );
      no_fail( mtx_unlock( &(mutex->s->mutex) ) );
    }
    if( 0 == decrement_ref_count( L, &(mutex->s->ref) ) ) {
      mtx_destroy( &(mutex->s->ref.mtx) );
      mtx_destroy( &(mutex->s->mutex) );
      cnd_destroy( &(mutex->s->unlocked) );
      free( mutex->s );
    }
    mutex->s = NULL;
  }
  return 0;
}


static int tinylmutex_lock( lua_State* L ) {
  tinylmutex* mutex = check_mutex( L, 1 );
  tinylthread* thread = get_udata_from_registry( L, TLT_THISTHREAD );
  int disabled = 0;
  int itr = 0;
  mtx_lock_or_throw( L, &(mutex->s->mutex) );
  while( !(itr=is_interrupted( thread, &disabled )) &&
         mutex->s->count > 0 && !mutex->is_owner ) {
    set_block( thread, &(mutex->s->ref), &(mutex->s->unlocked),
               &(mutex->s->mutex) );
    if( thrd_success !=
        cnd_wait( &(mutex->s->unlocked), &(mutex->s->mutex) ) ) {
      set_block( thread, NULL, NULL, NULL );
      no_fail( mtx_unlock( &(mutex->s->mutex) ) );
      luaL_error( L, "waiting for mutex failed" );
    }
    set_block( thread, NULL, NULL, NULL );
  }
  if( itr ) {
    no_fail( mtx_unlock( &(mutex->s->mutex) ) );
    throw_interrupt( L );
  }
  mutex->is_owner = 1;
  mutex->s->count++;
  no_fail( mtx_unlock( &(mutex->s->mutex) ) );
  lua_pushboolean( L, 1 );
  return 1;
}


static int tinylmutex_trylock( lua_State* L ) {
  tinylmutex* mutex = check_mutex( L, 1 );
  tinylthread* thread = get_udata_from_registry( L, TLT_THISTHREAD );
  if( is_interrupted( thread, NULL ) )
    throw_interrupt( L );
  mtx_lock_or_throw( L, &(mutex->s->mutex) );
  if( mutex->s->count > 0 && !mutex->is_owner ) {
    no_fail( mtx_unlock( &(mutex->s->mutex) ) );
    lua_pushboolean( L, 0 );
  } else {
    mutex->s->count++;
    mutex->is_owner = 1;
    no_fail( mtx_unlock( &(mutex->s->mutex) ) );
    lua_pushboolean( L, 1 );
  }
  return 1;
}


static int tinylmutex_unlock( lua_State* L ) {
  tinylmutex* mutex = check_mutex( L, 1 );
  tinylthread* thread = get_udata_from_registry( L, TLT_THISTHREAD );
  int locked = 0;
  int owner = mutex->is_owner;
  no_fail( mtx_lock( &(mutex->s->mutex) ) );
  locked = mutex->s->count > 0;
  if( locked && owner && --(mutex->s->count) == 0 ) {
    mutex->is_owner = 0;
    no_fail( cnd_signal( &(mutex->s->unlocked) ) );
  }
  no_fail( mtx_unlock( &(mutex->s->mutex) ) );
  if( is_interrupted( thread, NULL ) )
    throw_interrupt( L );
  if( !locked ) {
    lua_pushnil( L );
    lua_pushliteral( L, "mutex is already unlocked" );
    return 2;
  }
  if( !owner ) {
    lua_pushnil( L );
    lua_pushliteral( L, "mutex is locked by another thread" );
    return 2;
  }
  lua_pushboolean( L, 1 );
  return 1;
}



static int tinylthread_new_pipe( lua_State* L ) {
  tinylport* port1 = lua_newuserdata( L, sizeof( *port1 ) );
  tinylport* port2 = lua_newuserdata( L, sizeof( *port2 ) );
  port1->s = port2->s = NULL;
  luaL_setmetatable( L, TLT_WPORT_NAME );
  lua_pushvalue( L, -2 );
  luaL_setmetatable( L, TLT_RPORT_NAME );
  lua_pop( L, 1 );
  port1->is_reader = 1;
  port2->is_reader = 0;
  port1->s = malloc( sizeof( *port1->s ) );
  if( !port1->s )
    luaL_error( L, "memory allocation error" );
  port1->s->ref.cnt = 2;
  port1->s->L = NULL;
  port1->s->rports = 1;
  port1->s->wports = 1;
  if( thrd_success != mtx_init( &(port1->s->ref.mtx), mtx_plain ) ) {
    free( port1->s );
    port1->s = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  if( thrd_success != mtx_init( &(port1->s->mutex), mtx_plain ) ) {
    mtx_destroy( &(port1->s->ref.mtx) );
    free( port1->s );
    port1->s = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  if( thrd_success != cnd_init( &(port1->s->data_copied) ) ) {
    mtx_destroy( &(port1->s->ref.mtx) );
    mtx_destroy( &(port1->s->mutex) );
    free( port1->s );
    port1->s = NULL;
    luaL_error( L, "condition variable initialization failed" );
  }
  if( thrd_success != cnd_init( &(port1->s->waiting_senders) ) ) {
    mtx_destroy( &(port1->s->ref.mtx) );
    mtx_destroy( &(port1->s->mutex) );
    cnd_destroy( &(port1->s->data_copied) );
    free( port1->s );
    port1->s = NULL;
    luaL_error( L, "condition variable initialization failed" );
  }
  if( thrd_success != cnd_init( &(port1->s->waiting_receivers) ) ) {
    mtx_destroy( &(port1->s->ref.mtx) );
    mtx_destroy( &(port1->s->mutex) );
    cnd_destroy( &(port1->s->data_copied) );
    cnd_destroy( &(port1->s->waiting_senders) );
    free( port1->s );
    port1->s = NULL;
    luaL_error( L, "condition variable initialization failed" );
  }
  port2->s = port1->s;
  return 2;
}


static int tinylport_copy( void* p, lua_State* L, int midx ) {
  tinylport* port = p;
  tinylport* copy = lua_newuserdata( L, sizeof( *copy ) );
  copy->s = NULL;
  copy->is_reader = port->is_reader;
  lua_pushvalue( L, midx );
  lua_setmetatable( L, -2 );
  if( port->s ) {
    increment_ref_count( L, &(port->s->ref) );
    copy->s = port->s;
  }
  return 1;
}


static int tinylport_gc( lua_State* L ) {
  tinylport* port = lua_touserdata( L, 1 );
  if( port->s ) {
    no_fail( mtx_lock( &(port->s->mutex) ) );
    if( port->is_reader ) {
      if( 0 == --(port->s->rports) )
        no_fail( cnd_broadcast( &(port->s->waiting_senders) ) );
    } else {
      if( 0 == --(port->s->wports) ) {
        no_fail( cnd_broadcast( &(port->s->data_copied) ) );
        no_fail( cnd_broadcast( &(port->s->waiting_receivers) ) );
      }
    }
    no_fail( mtx_unlock( &(port->s->mutex) ) );
    if( 0 == decrement_ref_count( L, &(port->s->ref) ) ) {
      mtx_destroy( &(port->s->ref.mtx) );
      mtx_destroy( &(port->s->mutex) );
      cnd_destroy( &(port->s->data_copied) );
      cnd_destroy( &(port->s->waiting_senders) );
      cnd_destroy( &(port->s->waiting_receivers) );
      free( port->s );
    }
    port->s = NULL;
  }
  return 0;
}


static int tinylport_read( lua_State* L ) {
  tinylport* port = check_rport( L, 1 );
  tinylthread* thread = NULL;
  int itr = 0;
  int disabled = 0;
  lua_settop( L, 1 );
  thread = get_udata_from_registry( L, TLT_THISTHREAD );
  mtx_lock_or_throw( L, &(port->s->mutex) );
  while( !(itr=is_interrupted( thread, &disabled )) &&
         port->s->L != NULL &&
         port->s->wports > 0 ) {
    set_block( thread, &(port->s->ref), &(port->s->waiting_receivers),
               &(port->s->mutex) );
    if( thrd_success !=
        cnd_wait( &(port->s->waiting_receivers), &(port->s->mutex) ) ) {
      set_block( thread, NULL, NULL, NULL );
      no_fail( mtx_unlock( &(port->s->mutex) ) );
      luaL_error( L, "waiting for port access failed" );
    }
    set_block( thread, NULL, NULL, NULL );
  }
  if( itr ) { /* handle interrupt request */
    no_fail( mtx_unlock( &(port->s->mutex) ) );
    throw_interrupt( L );
  }
  if( port->s->wports == 0 ) { /* no more senders alive */
    no_fail( mtx_unlock( &(port->s->mutex) ) );
    luaL_error( L, "broken pipe" );
  }
  port->s->L = L;
  if( thrd_success !=
      cnd_signal( &(port->s->waiting_senders) ) ) {
    port->s->L = NULL;
    no_fail( cnd_signal( &(port->s->waiting_receivers) ) );
    no_fail( mtx_unlock( &(port->s->mutex) ) );
    luaL_error( L, "waking up sender thread failed" );
  }
  while( !(itr=is_interrupted( thread, &disabled )) &&
         port->s->L == L &&
         port->s->wports > 0 ) {
    set_block( thread, &(port->s->ref), &(port->s->data_copied),
               &(port->s->mutex) );
    if( thrd_success !=
        cnd_wait( &(port->s->data_copied), &(port->s->mutex) ) ) {
      set_block( thread, NULL, NULL, NULL );
      port->s->L = NULL;
      no_fail( cnd_signal( &(port->s->waiting_receivers) ) );
      no_fail( mtx_unlock( &(port->s->mutex) ) );
      luaL_error( L, "waiting for data transfer failed" );
    }
    set_block( thread, NULL, NULL, NULL );
  }
  if( port->s->L == L ) { /* no data received */
    if( itr ) { /* handle interrupt request */
      no_fail( mtx_unlock( &(port->s->mutex) ) );
      throw_interrupt( L );
    }
    if( port->s->wports == 0 ) {
      no_fail( mtx_unlock( &(port->s->mutex) ) );
      luaL_error( L, "broken pipe" );
    }
  }
  no_fail( mtx_unlock( &(port->s->mutex) ) );
  return 1;
}


static int tinylport_write( lua_State* L ) {
  tinylport* port = check_wport( L, 1 );
  tinylthread* thread = NULL;
  int itr = 0;
  int disabled = 0;
  luaL_checkany( L, 2 );
  lua_settop( L, 2 );
  thread = get_udata_from_registry( L, TLT_THISTHREAD );
  lua_pushvalue( L, 2 );
  mtx_lock_or_throw( L, &(port->s->mutex) );
  while( !(itr=is_interrupted( thread, &disabled )) &&
         port->s->L == NULL &&
         port->s->rports > 0 ) {
    set_block( thread, &(port->s->ref), &(port->s->waiting_senders),
               &(port->s->mutex) );
    if( thrd_success !=
        cnd_wait( &(port->s->waiting_senders), &(port->s->mutex) ) ) {
      set_block( thread, NULL, NULL, NULL );
      no_fail( mtx_unlock( &(port->s->mutex) ) );
      luaL_error( L, "waiting for a receiver thread failed" );
    }
    set_block( thread, NULL, NULL, NULL );
  }
  if( itr ) { /* handle interrupt request */
    no_fail( mtx_unlock( &(port->s->mutex) ) );
    throw_interrupt( L );
  }
  if( port->s->rports == 0 ) { /* no more receivers alive */
    no_fail( mtx_unlock( &(port->s->mutex) ) );
    luaL_error( L, "broken pipe" );
  }
  if( 0 != lua_cpcallr( port->s->L, copy_stack_top, L, 1 ) ) {
    int res = lua_cpcallr( L, copy_stack_top, port->s->L, 1 );
    lua_pop( port->s->L, 1 ); /* remove error object */
    if( res != 0 )
      lua_pushliteral( L, "unknown error" );
    lua_error( L );
  }
  /* signal waiting receiver */
  if( thrd_success !=
      cnd_signal( &(port->s->data_copied) ) ) {
    lua_pop( port->s->L, 1 );
    no_fail( mtx_unlock( &(port->s->mutex) ) );
    luaL_error( L, "waking up receiver thread failed" );
  }
  port->s->L = NULL;
  no_fail( cnd_signal( &(port->s->waiting_receivers) ) );
  no_fail( mtx_unlock( &(port->s->mutex) ) );
  lua_pushboolean( L, 1 );
  return 1;
}



static int tinylitr_tostring( lua_State* L ) {
  lua_pushliteral( L, "thread interrupted" );
  return 1;
}


static int tinylitr_copy( void* p, lua_State* L, int midx ) {
  (void)p;
  (void)midx;
  lua_getfield( L, LUA_REGISTRYINDEX, TLT_INTERRUPT );
  if( lua_type( L, -1 ) != LUA_TUSERDATA ) {
    lua_pop( L, 1 );
    return 0;
  }
  return 1;
}



static int tinylthread_sleep( lua_State* L ) {
  lua_Number seconds = luaL_checknumber( L, 1 );
  tinylthread* thread = get_udata_from_registry( L, TLT_THISTHREAD );
  int disabled = 0;
  int itr = 0;
  int ret = 0;
  struct timespec a, b;
  struct timespec* duration = &a;
  struct timespec* remaining = &b;
  luaL_argcheck( L, seconds >= 0, 1, "positive number expected" );
  duration->tv_sec = (time_t)seconds;
  duration->tv_nsec = (long)((seconds-floor(seconds))*1000000000L);
  if( duration->tv_sec > 0 || duration->tv_nsec > 0 ) {
    while( !(itr=is_interrupted( thread, &disabled )) &&
           -1 == (ret=thrd_sleep( duration, remaining )) ) {
      struct timespec* temp = duration;
      duration = remaining;
      remaining = temp;
    }
  }
  if( itr || is_interrupted( thread, &disabled ) )
    throw_interrupt( L );
  if( ret < 0 )
    luaL_error( L, "sleep failed" );
  return 0;
}


static int tinylthread_nointerrupt( lua_State* L ) {
  tinylthread* thread = get_udata_from_registry( L, TLT_THISTHREAD );
  if( thread != NULL ) {
    no_fail( mtx_lock( &(thread->s->mutex) ) );
    thread->s->ignore_interrupt = 1;
    no_fail( mtx_unlock( &(thread->s->mutex) ) );
  }
  return 0;
}


typedef struct {
  char const* metatable;
  char const* type;
} type_map;

static int tinylthread_type( lua_State* L ) {
  type_map const types[] = {
    { TLT_THRD_NAME, "thread" },
    { TLT_MTX_NAME, "mutex" },
    { TLT_RPORT_NAME, "port" },
    { TLT_WPORT_NAME, "port" },
    { TLT_ITR_NAME, "interrupt" },
    { NULL, NULL }
  };
  lua_settop( L, 1 );
  if( lua_type( L, 1 ) == LUA_TUSERDATA ) {
    size_t i;
    lua_getmetatable( L, 1 );
    if( lua_type( L, -1 ) == LUA_TTABLE ) {
      for( i = 0; types[ i ].metatable != NULL; ++i ) {
        luaL_getmetatable( L, types[ i ].metatable );
        if( lua_rawequal( L, -1, -2 ) ) {
          lua_pushstring( L, types[ i ].type );
          return 1;
        }
        lua_pop( L, 1 );
      }
    }
  }
  lua_pushnil( L );
  return 1;
}



static void create_api( lua_State* L ) {
  tinylthread_c_api_v1* api = lua_newuserdata( L, sizeof( *api ) );
  api->version = TLT_C_API_V1_MINOR;
  lua_setfield( L, LUA_REGISTRYINDEX, TLT_C_API_V1 );
}


static void create_meta( lua_State* L, char const* name,
                         luaL_Reg const* methods,
                         luaL_Reg const* metas ) {
  if( !luaL_newmetatable( L, name ) )
    luaL_error( L, "'%s' metatable already exists", name );
#if LUA_VERSION_NUM < 503
  lua_pushstring( L, name );
  lua_setfield( L, -2, "__name" );
#endif
  lua_pushliteral( L, "locked" );
  lua_setfield( L, -2, "__metatable" );
  if( methods ) {
    lua_newtable( L );
#if LUA_VERSION_NUM < 502
    luaL_register( L, NULL, methods );
#else
    luaL_setfuncs( L, methods, 0 );
#endif
    lua_setfield( L, -2, "__index" );
  }
  if( metas ) {
#if LUA_VERSION_NUM < 502
    luaL_register( L, NULL, metas );
#else
    luaL_setfuncs( L, metas, 0 );
#endif
  }
  lua_pop( L, 1 );
}


TINYLTHREAD_API int luaopen_tinylthread( lua_State* L ) {
  luaL_Reg const functions[] = {
    { "thread", tinylthread_new_thread },
    { "mutex", tinylthread_new_mutex },
    { "pipe", tinylthread_new_pipe },
    { "sleep", tinylthread_sleep },
    { "nointerrupt", tinylthread_nointerrupt },
    { "type", tinylthread_type },
    { NULL, NULL }
  };
  luaL_Reg const thread_methods[] = {
    { "detach", tinylthread_detach },
    { "join", tinylthread_join },
    { "interrupt", tinylthread_interrupt },
    { NULL, NULL }
  };
  luaL_Reg const thread_metas[] = {
    { "__gc", tinylthread_gc },
    { "__copy@tinylthread", (lua_CFunction)tinylthread_copy },
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
    { "__copy@tinylthread", (lua_CFunction)tinylmutex_copy },
    { NULL, NULL }
  };
  luaL_Reg const rport_methods[] = {
    { "read", tinylport_read },
    { NULL, NULL }
  };
  luaL_Reg const wport_methods[] = {
    { "write", tinylport_write },
    { NULL, NULL }
  };
  luaL_Reg const port_metas[] = {
    { "__gc", tinylport_gc },
    { "__copy@tinylthread", (lua_CFunction)tinylport_copy },
    { NULL, NULL }
  };
  luaL_Reg const itr_metas[] = {
    { "__tostring", tinylitr_tostring },
    { "__copy@tinylthread", (lua_CFunction)tinylitr_copy },
    { NULL, NULL }
  };
  /* create a struct containing function pointers to functions useful
   * for other C extension modules and put it in the registry */
  create_api( L );
  /* create and register all metatables used by this module */
  create_meta( L, TLT_THRD_NAME, thread_methods, thread_metas );
  create_meta( L, TLT_MTX_NAME, mutex_methods, mutex_metas );
  create_meta( L, TLT_RPORT_NAME, rport_methods, port_metas );
  create_meta( L, TLT_WPORT_NAME, wport_methods, port_metas );
  create_meta( L, TLT_ITR_NAME, NULL, itr_metas );
  /* store the common thread main function in the registry */
  lua_pushcfunction( L, (lua_CFunction)tinylthread_thunk );
  lua_setfield( L, LUA_REGISTRYINDEX, TLT_THUNK );
  /* create a sentinel value and store it in the registry */
  lua_newuserdata( L, 0 );
  luaL_setmetatable( L, TLT_ITR_NAME );
  lua_setfield( L, LUA_REGISTRYINDEX, TLT_INTERRUPT );
#if LUA_VERSION_NUM == 501
  lua_newtable( L );
  luaL_register( L, NULL, functions );
#else
  luaL_newlib(L, functions);
#endif
  return 1;
}



#if !defined( __STDC_VERSION__ ) || \
    __STDC_VERSION__ < 201112L || \
    defined( __STDC_NO_THREADS__ ) || \
    (defined( __APPLE__ ) && defined( __MACH__ )) || \
    defined( __MINGW32__ )
#  if defined( __MINGW32__ )
/* Some small hacks necessary to build on MinGW: */
WINBASEAPI DWORD WINAPI GetThreadId(HANDLE);
#    define _ftime_s(_x) _ftime(_x)
#  endif
/* make building easier by including the source of the C11 threads
 * emulation library tinycthread */
#  include <tinycthread.c>
#endif

