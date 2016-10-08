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


/* similar to lua_cpcall but allows return values */
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
#else
#  define lua_cpcallr( L, f, u, r ) \
  (lua_pushcfunction( L, f ), \
   lua_pushlightuserdata( L, u ), \
   lua_pcall( L, 1, r, 0 ))
#endif


static void* getudatafromregistry( lua_State* L, char const* name ) {
  lua_getfield( L, LUA_REGISTRYINDEX, name );
  return lua_touserdata( L, -1 );
}

/* in many circumstance failure to (un-)lock a mutex should be fatal
 * (i.e. raise a Lua error). */
static void mtx_lock_or_die( lua_State* L, mtx_t* m ) {
  if( thrd_success != mtx_lock( m ) )
    luaL_error( L, "mutex locking failed" );
}

static void mtx_unlock_or_die( lua_State* L, mtx_t* m ) {
  if( thrd_success != mtx_unlock( m ) )
    luaL_error( L, "mutex unlocking failed" );
}

/* helper functions for managing the reference count of shared
 * objects */
static void incref( lua_State* L, tinylheader* ref ) {
  mtx_lock_or_die( L, &(ref->mtx) );
  ref->cnt++;
  if( thrd_success != mtx_unlock( &(ref->mtx) ) ) {
    ref->cnt--;
    luaL_error( L, "mutex unlocking failed" );
  }
}

static size_t decref( lua_State* L, tinylheader* ref ) {
  size_t newcnt = 0;
  mtx_lock_or_die( L, &(ref->mtx) );
  newcnt = --(ref->cnt);
  mtx_unlock_or_die( L, &(ref->mtx) );
  return newcnt;
}

/* helper functions for accessing objects on the Lua stack */
static tinylthread* check_thread( lua_State* L, int idx ) {
  tinylthread* ud = luaL_checkudata( L, idx, TLT_THRD_NAME );
  if( !ud->s )
    luaL_error( L, "attempt to use invalid thread" );
  return ud;
}

static tinylmutex* check_mutex( lua_State* L, int idx ) {
  tinylmutex* ud = luaL_checkudata( L, idx, TLT_MTX_NAME );
  if( !ud->s )
    luaL_error( L, "attempt to use invalid mutex" );
  return ud;
}

static tinylport* check_rport( lua_State* L, int idx ) {
  tinylport* ud = luaL_checkudata( L, idx, TLT_RPORT_NAME );
  if( !ud->s )
    luaL_error( L, "attempt to use invalid port" );
  return ud;
}

static tinylport* check_wport( lua_State* L, int idx ) {
  tinylport* ud = luaL_checkudata( L, idx, TLT_WPORT_NAME );
  if( !ud->s )
    luaL_error( L, "attempt to use invalid port" );
  return ud;
}



static int is_interrupted( tinylthread* th, int* disabled ) {
  int v = 0;
  int dummy = 0;
  if( !disabled )
    disabled = &dummy;
  if( th != NULL &&
      thrd_success == mtx_lock( &(th->s->mutex) ) ) {
    *disabled |= th->s->ignore_interrupt;
    if( th->s->is_interrupted ) {
      if( !*disabled )
        v = 1;
      th->s->ignore_interrupt = 0;
    }
    mtx_unlock( &(th->s->mutex) );
  }
  return v;
}

static void throw_interrupt( lua_State* L ) {
  getudatafromregistry( L, TLT_INTERRUPT );
  lua_error( L );
}


static void set_condition( tinylthread* th, cnd_t* c ) {
  if( th != NULL &&
      thrd_success == mtx_lock( &(th->s->mutex) ) ) {
    th->s->condition = c;
    mtx_unlock( &(th->s->mutex) );
  }
}


static int cleanup_thread( lua_State* L ) {
  int* memory_problem = lua_touserdata( L, 1 );
  tinylthread* th = getudatafromregistry( L, TLT_THISTHREAD );
  int is_detached = 0;
  lua_settop( L, 0 );
  if( th != NULL && thrd_success == mtx_lock( &(th->s->mutex) ) ) {
    is_detached = th->s->is_detached;
    mtx_unlock( &(th->s->mutex) );
  }
  lua_settop( L, 0 );
  if( is_detached ) {
    /* the Lua states of detached threads might never be collected,
     * so we clean up as best as we can ... */
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


/* all API calls on toL are protected, on fromL not! */
static void copy_to_thread( lua_State* toL, lua_State* fromL ) {
  int top = lua_gettop( fromL );
  int i = 1;
  luaL_checkstack( toL, top+LUA_MINSTACK, "copy_to_thread" );
  for( i = 1; i <= top; ++i ) {
    switch( lua_type( fromL, i ) ) {
      case LUA_TNIL:
        lua_pushnil( toL );
        break;
      case LUA_TBOOLEAN:
        lua_pushboolean( toL, lua_toboolean( fromL, i ) );
        break;
      case LUA_TSTRING: {
          size_t len = 0;
          char const* s = lua_tolstring( fromL, i, &len );
          lua_pushlstring( toL, s, len );
        }
        break;
      case LUA_TNUMBER:
        if( lua_isinteger( fromL, i ) )
          lua_pushinteger( toL, lua_tointeger( fromL, i ) );
        else
          lua_pushnumber( toL, lua_tonumber( fromL, i ) );
        break;
      case LUA_TUSERDATA:
        /* check that the userdata has `__name` and `__tinylthread@copy`
         * metafields, and that there exists a corresponding metatable
         * in the registry of the target Lua state */
        lua_getmetatable( fromL, i );
        if( lua_istable( fromL, -1 ) ) {
          tinylport_copyf copyf = 0;
          lua_pushliteral( fromL, "__name" );
          lua_rawget( fromL, -2 );
          lua_pushliteral( fromL, "__tinylthread@copy" );
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
                  lua_pop( fromL, 2 ); /* pop metatable and name */
                  break; /* success! */
                }
              }
            }
          }
          lua_pop( fromL, 1 ); /* pop name */
        }
        lua_pop( fromL, 1 ); /* pop metatable */
        luaL_error( toL, "bad value #%d (unsupported type: 'userdata')",
                    i );
        break;
      default:
        luaL_error( toL, "bad value #%d (unsupported type: '%s')",
                    i, luaL_typename( fromL, i ) );
        break;
    }
  }
}


/* all API calls on childL are protected, but all API calls on
 * parentL are *unprotected* and could cause resource leaks if
 * an unhandled exception is thrown! */
static int prepare_thread_state( lua_State* childL ) {
  lua_State* parentL = lua_touserdata( childL, 1 );
  size_t len = 0;
  char const* s = NULL;
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
  copy_to_thread( childL, parentL );
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
 * exception is thrown! */
static int get_error_message( lua_State* L ) {
  lua_State* fromL = lua_touserdata( L, 1 );
#define S "thread state initialization failed"
  size_t len = sizeof( S )-1;
  char const* msg = S;
#undef S
  if( lua_type( fromL, -1 ) == LUA_TSTRING ) {
    msg = lua_tolstring( fromL, -1, &len );
    lua_pushlstring( L, msg, len );
  }
  return 1;
}


static int tinylthread_newthread( lua_State* L ) {
  tinylthread* ud = NULL;
  thrd_start_t thunk = 0;
  lua_State* tempL = NULL;
  luaL_checkstring( L, 1 ); /* the Lua code */
  ud = lua_newuserdata( L, sizeof( *ud ) );
  ud->s = NULL;
  ud->is_parent = 1;
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
  ud->s = malloc( sizeof( *ud->s ) );
  if( !ud->s )
    luaL_error( L, "memory allocation error" );
  ud->s->condition = NULL;
  ud->s->exit_status = 0;
  ud->s->is_detached = 0;
  ud->s->is_interrupted = 0;
  ud->s->ignore_interrupt = 0;
  ud->s->ref.cnt = 1;
  if( thrd_success != mtx_init( &(ud->s->ref.mtx), mtx_plain ) ) {
    free( ud->s );
    ud->s = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  if( thrd_success != mtx_init( &(ud->s->mutex), mtx_plain ) ) {
    mtx_destroy( &(ud->s->ref.mtx ) );
    free( ud->s );
    ud->s = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  ud->s->L = tempL = luaL_newstate();
  if( !tempL )
    luaL_error( L, "memory allocation error" );
  /* prepare the Lua state for the child thread */
  if( 0 != lua_cpcallr( ud->s->L, prepare_thread_state, L, LUA_MULTRET ) ) {
    lua_cpcallr( L, get_error_message, ud->s->L, 1 );
    ud->s->L = NULL;
    lua_close( tempL );
    lua_error( L ); /* rethrow the error from `get_error_message`  */
  }
  /* get thread main function from the child's registry (must not
   * raise an error!) */
  lua_getfield( ud->s->L, LUA_REGISTRYINDEX, TLT_THUNK );
  thunk = (thrd_start_t)lua_tocfunction( ud->s->L, -1 );
  lua_pop( ud->s->L, 1 );
  /* lock this structure and create a C thread */
  if( thrd_success != mtx_lock( &(ud->s->mutex) ) ) {
    ud->s->L = NULL;
    lua_close( tempL );
    luaL_error( L, "mutex locking failed" );
  }
  if( thrd_success != thrd_create( &(ud->s->thread), thunk, ud->s->L ) ) {
    mtx_unlock( &(ud->s->mutex) );
    ud->s->L = NULL;
    lua_close( tempL );
    luaL_error( L, "thread spawning failed" );
  }
  mtx_unlock_or_die( L, &(ud->s->mutex) );
  return 1;
}


static int tinylthread_copy( void* p, lua_State* L, int midx ) {
  tinylthread* ud = p;
  tinylthread* copy = lua_newuserdata( L, sizeof( *copy ) );
  copy->s = NULL;
  copy->is_parent = 0;
  lua_pushvalue( L, midx );
  lua_setmetatable( L, -2 );
  if( ud->s ) {
    incref( L, &(ud->s->ref) );
    copy->s = ud->s;
  }
  return 1;
}


static int tinylthread_gc( lua_State* L ) {
  tinylthread* ud = lua_touserdata( L, 1 );
  if( ud->s ) {
    int raise_error = 0;
    if( ud->is_parent &&
        thrd_success == mtx_lock( &(ud->s->mutex) ) ) {
      raise_error = !ud->s->is_detached && ud->s->L != NULL;
      mtx_unlock( &(ud->s->mutex) );
    }
    if( 0 == decref( L, &(ud->s->ref) ) ) {
      mtx_destroy( &(ud->s->ref.mtx) );
      mtx_destroy( &(ud->s->mutex) );
      free( ud->s );
      ud->s = NULL;
    }
    if( raise_error )
      luaL_error( L, "collecting non-joined thread" );
  }
  return 0;
}


static int tinylthread_detach( lua_State* L ) {
  tinylthread* ud = check_thread( L, 1 );
  int is_detached = 0;
  int is_joined = 0;
  int status = 0;
  if( !ud->is_parent )
    luaL_error( L, "detach request from unrelated thread" );
  mtx_lock_or_die( L, &(ud->s->mutex) );
  is_detached = ud->s->is_detached;
  is_joined = ud->s->L == NULL;
  if( !is_detached && !is_joined ) {
    status = thrd_detach( ud->s->thread );
    if( status == thrd_success )
      ud->s->is_detached = 1;
  }
  mtx_unlock_or_die( L, &(ud->s->mutex) );
  if( is_detached )
    luaL_error( L, "attempt to detach an already detached thread" );
  if( is_joined )
    luaL_error( L, "attempt to detach an already joined thread" );
  if( status == thrd_success ) {
    lua_pushboolean( L, 1 );
    return 1;
  } else {
    lua_pushnil( L );
    lua_pushliteral( L, "thread detaching failed" );
    return 2;
  }
}


typedef struct {
  int status;
  lua_State* L;
} join_data;

static int join_return_values( lua_State* L ) {
  join_data* data = lua_touserdata( L, 1 );
  lua_pop( L, 1 );
  lua_pushboolean( L, data->status == 0 );
  copy_to_thread( L, data->L );
  return lua_gettop( L );
}

static int tinylthread_join( lua_State* L ) {
  tinylthread* ud = check_thread( L, 1 );
  int is_detached = 0;
  int is_joined = 0;
  join_data data = { 0, NULL };
  if( !ud->is_parent )
    luaL_error( L, "join request from unrelated thread" );
  mtx_lock_or_die( L, &(ud->s->mutex) );
  is_detached = ud->s->is_detached;
  is_joined = ud->s->L == NULL;
  mtx_unlock_or_die( L, &(ud->s->mutex) );
  if( is_detached )
    luaL_error( L, "attempt to join an already detached thread" );
  if( is_joined )
    luaL_error( L, "attempt to join an already joined thread" );
  if( thrd_success != thrd_join( ud->s->thread, &(data.status) ) )
    luaL_error( L, "thread joining failed" );
  mtx_lock_or_die( L, &(ud->s->mutex) );
  data.L = ud->s->L;
  ud->s->L = NULL;
  if( thrd_success != mtx_unlock( &(ud->s->mutex) ) ) {
    lua_close( data.L );
    luaL_error( L, "mutex unlocking failed" );
  }
  lua_settop( L, 0 );
  if( 0 != lua_cpcallr( L, join_return_values, &data, LUA_MULTRET ) ) {
    lua_close( data.L );
    lua_error( L );
  }
  lua_close( data.L );
  return lua_gettop( L );
}


static int tinylthread_interrupt( lua_State* L ) {
  tinylthread* ud = check_thread( L, 1 );
  mtx_lock_or_die( L, &(ud->s->mutex) );
  ud->s->is_interrupted = 1;
  if( ud->s->condition )
    cnd_broadcast( ud->s->condition );
  mtx_unlock_or_die( L, &(ud->s->mutex) );
  return 0;
}



static int tinylthread_newmutex( lua_State* L ) {
  tinylmutex* ud = lua_newuserdata( L, sizeof( *ud ) );
  ud->s = NULL;
  ud->is_owner = 0;
  luaL_setmetatable( L, TLT_MTX_NAME );
  ud->s = malloc( sizeof( *ud->s ) );
  if( !ud->s )
    luaL_error( L, "memory allocation error" );
  if( thrd_success != mtx_init( &(ud->s->ref.mtx), mtx_plain ) ) {
    free( ud->s );
    ud->s = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  ud->s->ref.cnt = 1;
  if( thrd_success != mtx_init( &(ud->s->mutex), mtx_plain ) ) {
    mtx_destroy( &(ud->s->ref.mtx) );
    free( ud->s );
    ud->s = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  if( thrd_success != cnd_init( &(ud->s->unlocked) ) ) {
    mtx_destroy( &(ud->s->ref.mtx) );
    mtx_destroy( &(ud->s->mutex) );
    free( ud->s );
    ud->s = NULL;
    luaL_error( L, "condition variable initialization failed" );
  }
  return 1;
}


static int tinylmutex_copy( void* p, lua_State* L, int midx ) {
  tinylmutex* ud = p;
  tinylmutex* copy = lua_newuserdata( L, sizeof( *copy ) );
  copy->s = NULL;
  copy->is_owner = 0;
  lua_pushvalue( L, midx );
  lua_setmetatable( L, -2 );
  if( ud->s ) {
    incref( L, &(ud->s->ref) );
    copy->s = ud->s;
  }
  return 1;
}


static int tinylmutex_gc( lua_State* L ) {
  tinylmutex* ud = lua_touserdata( L, 1 );
  if( ud->s ) {
    if( ud->is_owner && mtx_lock( &(ud->s->mutex) ) ) {
      ud->s->count = 0;
      cnd_signal( &(ud->s->unlocked) );
      mtx_unlock( &(ud->s->mutex) );
    }
    if( 0 == decref( L, &(ud->s->ref) ) ) {
      mtx_destroy( &(ud->s->ref.mtx) );
      mtx_destroy( &(ud->s->mutex) );
      cnd_destroy( &(ud->s->unlocked) );
      free( ud->s );
    }
    ud->s = NULL;
  }
  return 0;
}


static int tinylmutex_lock( lua_State* L ) {
  tinylmutex* ud = check_mutex( L, 1 );
  tinylthread* th = getudatafromregistry( L, TLT_THISTHREAD );
  int disabled = 0;
  int itr = 0;
  mtx_lock_or_die( L, &(ud->s->mutex) );
  while( !(itr=is_interrupted( th, &disabled )) &&
         ud->s->count > 0 && !ud->is_owner ) {
    set_condition( th, &(ud->s->unlocked) );
    if( thrd_success != cnd_wait( &(ud->s->unlocked), &(ud->s->mutex) ) ) {
      set_condition( th, NULL );
      mtx_unlock( &(ud->s->mutex) );
      luaL_error( L, "waiting on condition variable failed" );
    }
    set_condition( th, NULL );
  }
  if( itr ) {
    mtx_unlock( &(ud->s->mutex) );
    throw_interrupt( L );
  }
  ud->is_owner = 1;
  ud->s->count++;
  mtx_unlock_or_die( L, &(ud->s->mutex) );
  lua_pushboolean( L, 1 );
  return 1;
}


static int tinylmutex_trylock( lua_State* L ) {
  tinylmutex* ud = check_mutex( L, 1 );
  tinylthread* th = getudatafromregistry( L, TLT_THISTHREAD );
  if( is_interrupted( th, NULL ) )
    throw_interrupt( L );
  mtx_lock_or_die( L, &(ud->s->mutex) );
  if( ud->s->count > 0 && !ud->is_owner ) {
    mtx_unlock_or_die( L, &(ud->s->mutex) );
    lua_pushboolean( L, 0 );
  } else {
    ud->s->count++;
    ud->is_owner = 1;
    mtx_unlock_or_die( L, &(ud->s->mutex) );
    lua_pushboolean( L, 1 );
  }
  return 1;
}


static int tinylmutex_unlock( lua_State* L ) {
  tinylmutex* ud = check_mutex( L, 1 );
  tinylthread* th = getudatafromregistry( L, TLT_THISTHREAD );
  int locked = 0;
  int owner = ud->is_owner;
  mtx_lock_or_die( L, &(ud->s->mutex) );
  locked = ud->s->count > 0;
  if( locked && owner &&
      --(ud->s->count) == 0 &&
      ((ud->is_owner = 0), 1) &&
      thrd_success != cnd_signal( &(ud->s->unlocked) ) ) {
    mtx_unlock( &(ud->s->mutex) );
    luaL_error( L, "signaling waiting threads failed" );
  }
  mtx_unlock_or_die( L, &(ud->s->mutex) );
  if( !locked )
    luaL_error( L, "mutex is already unlocked" );
  if( !owner )
    luaL_error( L, "mutex is locked by another thread" );
  if( is_interrupted( th, NULL ) )
    throw_interrupt( L );
  lua_pushboolean( L, 1 );
  return 1;
}



static int tinylthread_newpipe( lua_State* L ) {
  tinylport* ud1 = lua_newuserdata( L, sizeof( *ud1 ) );
  tinylport* ud2 = lua_newuserdata( L, sizeof( *ud2 ) );
  ud1->s = ud2->s = NULL;
  luaL_setmetatable( L, TLT_WPORT_NAME );
  lua_pushvalue( L, -2 );
  luaL_setmetatable( L, TLT_RPORT_NAME );
  lua_pop( L, 1 );
  ud1->is_reader = 1;
  ud2->is_reader = 0;
  ud1->s = malloc( sizeof( *ud1->s ) );
  if( !ud1->s )
    luaL_error( L, "memory allocation error" );
  if( thrd_success != mtx_init( &(ud1->s->ref.mtx), mtx_plain ) ) {
    free( ud1->s );
    ud1->s = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  if( thrd_success != mtx_init( &(ud1->s->mutex), mtx_plain ) ) {
    mtx_destroy( &(ud1->s->ref.mtx) );
    free( ud1->s );
    ud1->s = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  if( thrd_success != cnd_init( &(ud1->s->data_copied) ) ) {
    mtx_destroy( &(ud1->s->ref.mtx) );
    mtx_destroy( &(ud1->s->mutex) );
    free( ud1->s );
    ud1->s = NULL;
    luaL_error( L, "condition variable initialization failed" );
  }
  if( thrd_success != cnd_init( &(ud1->s->waiting_senders) ) ) {
    mtx_destroy( &(ud1->s->ref.mtx) );
    mtx_destroy( &(ud1->s->mutex) );
    cnd_destroy( &(ud1->s->data_copied) );
    free( ud1->s );
    ud1->s = NULL;
    luaL_error( L, "condition variable initialization failed" );
  }
  if( thrd_success != cnd_init( &(ud1->s->waiting_receivers) ) ) {
    mtx_destroy( &(ud1->s->ref.mtx) );
    mtx_destroy( &(ud1->s->mutex) );
    cnd_destroy( &(ud1->s->data_copied) );
    cnd_destroy( &(ud1->s->waiting_senders) );
    free( ud1->s );
    ud1->s = NULL;
    luaL_error( L, "condition variable initialization failed" );
  }
  ud1->s->ref.cnt = 2;
  ud2->s = ud1->s;
  return 2;
}


static int tinylport_copy( void* p, lua_State* L, int midx ) {
  tinylport* ud = p;
  tinylport* copy = lua_newuserdata( L, sizeof( *copy ) );
  copy->s = NULL;
  copy->is_reader = ud->is_reader;
  lua_pushvalue( L, midx );
  lua_setmetatable( L, -2 );
  if( ud->s ) {
    incref( L, &(ud->s->ref) );
    copy->s = ud->s;
  }
  return 1;
}


static int tinylport_gc( lua_State* L ) {
  tinylport* ud = lua_touserdata( L, 1 );
  if( ud->s ) {
    if( thrd_success == mtx_lock( &(ud->s->mutex) ) ) {
      if( ud->is_reader ) {
        if( 0 == --(ud->s->rports) )
          cnd_broadcast( &(ud->s->waiting_senders) );
      } else {
        if( 0 == --(ud->s->wports) )
          cnd_broadcast( &(ud->s->waiting_receivers) );
      }
      mtx_unlock( &(ud->s->mutex) );
    }
    if( 0 == decref( L, &(ud->s->ref) ) ) {
      mtx_destroy( &(ud->s->ref.mtx) );
      mtx_destroy( &(ud->s->mutex) );
      cnd_destroy( &(ud->s->data_copied) );
      cnd_destroy( &(ud->s->waiting_senders) );
      cnd_destroy( &(ud->s->waiting_receivers) );
      free( ud->s );
    }
    ud->s = NULL;
  }
  return 0;
}


static int tinylport_read( lua_State* L ) {
  tinylport* ud = check_rport( L, 1 );
  (void)ud;
  /* TODO */
  return 0;
}


static int tinylport_write( lua_State* L ) {
  tinylport* ud = check_wport( L, 1 );
  (void)ud;
  /* TODO */
  return 0;
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
  tinylthread* th = getudatafromregistry( L, TLT_THISTHREAD );
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
    while( !(itr=is_interrupted( th, &disabled )) &&
           -1 == (ret=thrd_sleep( duration, remaining )) ) {
      struct timespec* temp = duration;
      duration = remaining;
      remaining = temp;
    }
  }
  if( itr || is_interrupted( th, &disabled ) )
    throw_interrupt( L );
  if( ret < 0 )
    luaL_error( L, "sleep failed" );
  return 0;
}


static int tinylthread_nointerrupt( lua_State* L ) {
  tinylthread* th = getudatafromregistry( L, TLT_THISTHREAD );
  if( th != NULL ) {
    mtx_lock_or_die( L, &(th->s->mutex) );
    th->s->ignore_interrupt = 1;
    mtx_unlock_or_die( L, &(th->s->mutex) );
  }
  return 0;
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
    { "thread", tinylthread_newthread },
    { "mutex", tinylthread_newmutex },
    { "pipe", tinylthread_newpipe },
    { "sleep", tinylthread_sleep },
    { "nointerrupt", tinylthread_nointerrupt },
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
    { "__tinylthread@copy", (lua_CFunction)tinylthread_copy },
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
    { "__tinylthread@copy", (lua_CFunction)tinylmutex_copy },
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
    { "__tinylthread@copy", (lua_CFunction)tinylport_copy },
    { NULL, NULL }
  };
  luaL_Reg const itr_metas[] = {
    { "__tostring", tinylitr_tostring },
    { "__tinylthread@copy", (lua_CFunction)tinylitr_copy },
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
    defined( __STDC_NO_THREADS__ )
/* make building easier by including the source of the C11 threads
 * emulation library tinycthread */
#  include <tinycthread.c>
#endif

