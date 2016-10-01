#include <stdlib.h>
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



/* helper functions for managing the reference count of shared
 * objects */
static void incref( lua_State* L, tinylheader* ref ) {
  if( thrd_success != mtx_lock( &(ref->mtx) ) )
    luaL_error( L, "mutex locking failed" );
  ref->cnt++;
  if( thrd_success != mtx_unlock( &(ref->mtx) ) ) {
    ref->cnt--;
    luaL_error( L, "mutex unlocking failed" );
  }
}

static size_t decref( lua_State* L, tinylheader* ref ) {
  size_t newcnt = 0;
  if( thrd_success != mtx_lock( &(ref->mtx) ) )
    luaL_error( L, "mutex locking failed" );
  newcnt = --(ref->cnt);
  if( thrd_success != mtx_unlock( &(ref->mtx) ) )
    luaL_error( L, "mutex unlocking failed" );
  return newcnt;
}

/* helper functions for accessing objects on the Lua stack */
static tinylthread* check_thread( lua_State* L, int idx ) {
  tinylthread* ud = luaL_checkudata( L, idx, TLT_THRD_NAME );
  if( !ud->t )
    luaL_error( L, "trying to use invalid thread" );
  return ud;
}

static tinylmutex* check_mutex( lua_State* L, int idx ) {
  tinylmutex* ud = luaL_checkudata( L, idx, TLT_MTX_NAME );
  if( !ud->m )
    luaL_error( L, "trying to use invalid mutex" );
  return ud;
}



static int detached_final_gc( lua_State* L ) {
  tinylthread* thread = NULL;
  int is_detached = 0;
  lua_getfield( L, LUA_REGISTRYINDEX, TLT_THISTHREAD );
  thread = lua_touserdata( L, -1 );
  if( thread != NULL &&
      thrd_success == mtx_lock( &(thread->t->thread_mutex) ) ) {
    is_detached = thread->t->is_detached;
    mtx_unlock( &(thread->t->thread_mutex) );
  }
  if( is_detached ) {
    /* the Lua states of detached threads might never be collected,
     * so we clean up as best as we can ... */
    lua_gc( L, LUA_GCCOLLECT, 0 );
    lua_gc( L, LUA_GCCOLLECT, 0 );
  }
  return 0;
}


static int mark_thread_dead( lua_State* L ) {
  int* status = lua_touserdata( L, 1 );
  tinylthread* thread = NULL;
  lua_getfield( L, LUA_REGISTRYINDEX, TLT_THISTHREAD );
  thread = lua_touserdata( L, -1 );
  if( thread != NULL &&
      thrd_success == mtx_lock( &(thread->t->thread_mutex) ) ) {
    thread->t->exit_status = *status;
    thread->t->is_dead = 1;
    mtx_unlock( &(thread->t->thread_mutex) );
  }
  return 0;
}


static int tinylthread_thunk( void* arg ) {
  lua_State* L = arg;
  int status = lua_pcall( L, lua_gettop( L ), 0, 0 );
  if( 0 != lua_cpcallr( L, detached_final_gc, NULL, 0 ) )
    lua_pop( L, 1 ); /* pop error message if necessary */
  if( 0 != lua_cpcallr( L, mark_thread_dead, &status, 0 ) )
    lua_pop( L, 1 ); /* pop error message if necessary */
  return status != 0;
}


/* all API calls on toL are protected, on fromL not! */
static void copy_to_thread( lua_State* toL, lua_State* fromL ) {
  int top = lua_gettop( fromL );
  int i = 1;
  luaL_checkstack( toL, top+LUA_MINSTACK, "copy_to_thread" );
  for( i = 1; i < top; ++i ) {
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
        /* check that the userdata has `__name` and `__tinylthread@pipe`
         * metafields, and that there exists a corresponding metatable
         * in the registry of the target Lua state */
        lua_getmetatable( fromL, i );
        if( lua_istable( fromL, -1 ) ) {
          tinylpipe_copyf copyf = 0;
          lua_pushliteral( fromL, "__name" );
          lua_rawget( fromL, -2 );
          lua_pushliteral( fromL, "__tinylthread@pipe" );
          lua_rawget( fromL, -3 );
          copyf = (tinylpipe_copyf)lua_tocfunction( fromL, -1 );
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
        luaL_error( toL, "bad argument #%d (unsupported type: 'userdata')",
                    i );
        break;
      default:
        luaL_error( toL, "bad argument #%d (unsupported type: '%s')",
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
  if( 0 != luaL_loadbuffer( childL, s, len, "=threadMain" ) )
    lua_error( childL );
  lua_replace( childL, 1 );
  return lua_gettop( childL );
}


/* all API calls on L are protected, but all API calls on fromL are
 * *unprotected* and could cause resource leaks if an unhandled
 * exception is thrown! */
static int get_error_message( lua_State* L ) {
  lua_State* fromL = lua_touserdata( L, 1 );
  size_t len = 0;
  char const* msg = 0;
  if( lua_type( fromL, -1 ) != LUA_TSTRING )
    luaL_error( L, "thread state initialization failed" );
  msg = lua_tolstring( fromL, -1, &len );
  lua_pushlstring( L, msg, len );
  lua_error( L );
  return 0;
}


static int tinylthread_newthread( lua_State* L ) {
  tinylthread* ud = NULL;
  thrd_start_t thunk = 0;
  lua_State* tempL = NULL;
  luaL_checkstring( L, 1 ); /* the Lua code */
  ud = lua_newuserdata( L, sizeof( *ud ) );
  ud->t = NULL;
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
  ud->t = malloc( sizeof( tinylthread ) );
  if( !ud->t )
    luaL_error( L, "memory allocation error" );
  ud->t->exit_status = 0;
  ud->t->is_interrupted = 0;
  ud->t->is_detached = 0;
  ud->t->is_dead = 1;
  ud->t->ref.cnt = 1;
  if( thrd_success != mtx_init( &(ud->t->ref.mtx), mtx_plain ) ) {
    free( ud->t );
    ud->t = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  if( thrd_success != mtx_init( &(ud->t->thread_mutex), mtx_plain) ) {
    mtx_destroy( &(ud->t->ref.mtx ) );
    free( ud->t );
    ud->t = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  ud->t->L = luaL_newstate();
  tempL = ud->t->L;
  if( !ud->t->L ) {
    mtx_destroy( &(ud->t->ref.mtx) );
    mtx_destroy( &(ud->t->thread_mutex) );
    free( ud->t );
    ud->t = NULL;
    luaL_error( L, "memory allocation error" );
  }
  /* prepare the Lua state for the child thread */
  if( 0 != lua_cpcallr( ud->t->L, prepare_thread_state, L, LUA_MULTRET ) ) {
    lua_cpcallr( L, get_error_message, ud->t->L, 0 );
    ud->t->L = NULL;
    lua_close( tempL );
    lua_error( L ); /* rethrow the error from `get_error_message`  */
  }
  /* get thread main function from the child's registry (must not
   * raise an error!) */
  lua_getfield( ud->t->L, LUA_REGISTRYINDEX, TLT_THUNK );
  thunk = (thrd_start_t)lua_tocfunction( ud->t->L, -1 );
  lua_pop( ud->t->L, 1 );
  /* lock this structure and create a C thread */
  if( thrd_success != mtx_lock( &(ud->t->thread_mutex) ) ) {
    ud->t->L = NULL;
    lua_close( tempL );
    luaL_error( L, "mutex locking failed" );
  }
  if( thrd_success != thrd_create( &(ud->t->thread), thunk, ud->t->L ) ) {
    mtx_unlock( &(ud->t->thread_mutex) );
    ud->t->L = NULL;
    lua_close( tempL );
    luaL_error( L, "thread spawning failed" );
  }
  ud->t->is_dead = 0;
  mtx_unlock( &(ud->t->thread_mutex) );
  return 1;
}


static int tinylthread_copy( void* p, lua_State* L, int midx ) {
  tinylthread* ud = p;
  tinylthread* copy = lua_newuserdata( L, sizeof( *copy ) );
  copy->t = NULL;
  copy->is_parent = 0;
  lua_pushvalue( L, midx );
  lua_setmetatable( L, -2 );
  if( ud->t ) {
    incref( L, &(ud->t->ref) );
    copy->t = ud->t;
  }
  return 1;
}


static int tinylthread_gc( lua_State* L ) {
  tinylthread* ud = lua_touserdata( L, 1 );
  if( ud->t ) {
    int raise_error = 0;
    int is_dead = 0;
    if( ud->is_parent ) {
      if( thrd_success == mtx_lock( &(ud->t->thread_mutex) ) ) {
        raise_error = !ud->t->is_detached && ud->t->L;
        is_dead = ud->t->is_dead;
        mtx_unlock( &(ud->t->thread_mutex) );
      }
    }
    switch( decref( L, &(ud->t->ref) ) ) {
      case 1:
        if( ud->is_parent && is_dead && ud->t->L ) {
          lua_State* tempL = ud->t->L;
          ud->t->L = NULL;
          lua_close( tempL );
        }
        break;
      case 0:
        mtx_destroy( &(ud->t->ref.mtx) );
        mtx_destroy( &(ud->t->thread_mutex) );
        free( ud->t );
        ud->t = NULL;
        break;
    }
    if( raise_error )
      luaL_error( L, "collecting non-joined thread" );
  }
  return 0;
}


static int tinylthread_detach( lua_State* L ) {
  tinylthread* ud = check_thread( L, 1 );
  if( !ud->is_parent )
    luaL_error( L, "detach request from unrelated thread" );
  /* TODO */
  return 0;
}


static int tinylthread_join( lua_State* L ) {
  tinylthread* ud = check_thread( L, 1 );
  if( !ud->is_parent )
    luaL_error( L, "join request from unrelated thread" );
  /* TODO */
  return 0;
}


static int tinylthread_interrupt( lua_State* L ) {
  tinylthread* ud = check_thread( L, 1 );
  if( thrd_success == mtx_lock( &(ud->t->thread_mutex) ) )
    luaL_error( L, "mutex locking failed" );
  ud->t->is_interrupted = 1;
  if( thrd_success != mtx_unlock( &(ud->t->thread_mutex) ) )
    luaL_error( L, "mutex unlocking failed" );
  return 0;
}


static int tinylthread_isdead( lua_State* L ) {
  tinylthread* ud = check_thread( L, 1 );
  int v = 0;
  if( thrd_success != mtx_lock( &(ud->t->thread_mutex) ) )
    luaL_error( L, "mutex locking failed" );
  v = ud->t->is_dead;
  if( thrd_success != mtx_unlock( &(ud->t->thread_mutex) ) )
    luaL_error( L, "mutex unlocking failed" );
  lua_pushboolean( L, v );
  return 1;
}


static int tinylthread_isdetached( lua_State* L ) {
  tinylthread* ud = check_thread( L, 1 );
  int v = 0;
  if( thrd_success != mtx_lock( &(ud->t->thread_mutex) ) )
    luaL_error( L, "mutex locking failed" );
  v = ud->t->is_detached;
  if( thrd_success != mtx_unlock( &(ud->t->thread_mutex) ) )
    luaL_error( L, "mutex unlocking failed" );
  lua_pushboolean( L, v );
  return 1;
}



static int tinylthread_newmutex( lua_State* L ) {
  tinylmutex* ud = lua_newuserdata( L, sizeof( *ud ) );
  ud->m = NULL;
  ud->lock_count = 0;
  luaL_setmetatable( L, TLT_MTX_NAME );
  ud->m = malloc( sizeof( tinylmutex ) );
  if( !ud->m )
    luaL_error( L, "memory allocation error" );
  if( thrd_success != mtx_init( &(ud->m->ref.mtx), mtx_plain ) ) {
    free( ud->m );
    ud->m = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  ud->m->ref.cnt = 1;
  if( thrd_success != mtx_init( &(ud->m->mutex), mtx_plain ) ) {
    mtx_destroy( &(ud->m->ref.mtx) );
    free( ud->m );
    ud->m = NULL;
    luaL_error( L, "mutex initialization failed" );
  }
  return 1;
}


static int tinylmutex_copy( void* p, lua_State* L, int midx ) {
  tinylmutex* ud = p;
  tinylmutex* copy = lua_newuserdata( L, sizeof( *copy ) );
  copy->m = NULL;
  copy->lock_count = 0;
  lua_pushvalue( L, midx );
  lua_setmetatable( L, -2 );
  if( ud->m ) {
    incref( L, &(ud->m->ref) );
    copy->m = ud->m;
  }
  return 1;
}


static int tinylmutex_gc( lua_State* L ) {
  tinylmutex* ud = lua_touserdata( L, 1 );
  if( ud->m ) {
    if( ud->lock_count > 0 )
      mtx_unlock( &(ud->m->mutex) );
    if( 0 == decref( L, &(ud->m->ref) ) ) {
      mtx_destroy( &(ud->m->ref.mtx) );
      mtx_destroy( &(ud->m->mutex) );
      free( ud->m );
    }
    ud->m = NULL;
  }
  return 0;
}


static int tinylmutex_lock( lua_State* L ) {
  tinylmutex* ud = check_mutex( L, 1 );
  if( thrd_success != mtx_lock( &(ud->m->mutex) ) )
    luaL_error( L, "mutex locking failed" );
  ud->lock_count++;
  return 0;
}


static int tinylmutex_trylock( lua_State* L ) {
  tinylmutex* ud = check_mutex( L, 1 );
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
  tinylmutex* ud = check_mutex( L, 1 );
  if( ud->lock_count == 0 )
    luaL_error( L, "trying to unlock non-locked mutex" );
  if( thrd_success != mtx_unlock( &(ud->m->mutex) ) )
    luaL_error( L, "mutex unlocking failed" );
  ud->lock_count--;
  return 0;
}



static int tinylthread_newpipe( lua_State* L ) {
  /* TODO */
  return 0;
}

static int tinylport_copy( void* p, lua_State* L, int midx ) {
  /* TODO */
  return 0;
}

static int tinylport_gc( lua_State* L ) {
  /* TODO */
  return 0;
}

static int tinylport_read( lua_State* L ) {
  /* TODO */
  return 0;
}

static int tinylport_write( lua_State* L ) {
  /* TODO */
  return 0;
}



static int tinylitr_tostring( lua_State* L ) {
  lua_pushliteral( L, "interrupted thread" );
  return 1;
}


static int tinylitr_copy( void* p, lua_State* L, int midx ) {
  (void)p;
  (void)midx;
  lua_getfield( L, LUA_REGISTRYINDEX, TLT_INTERRUPTED );
  if( lua_type( L, -1 ) != LUA_TUSERDATA ) {
    lua_pop( L, 1 );
    return 0;
  }
  return 1;
}


static int tinylthread_disable( lua_State* L ) {
  /* TODO */
  return 0;
}


static int tinylthread_check( lua_State* L ) {
  /* TODO */
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
    luaL_error( L, "%s metatable already exists", name );
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
    { "disableinterrupt", tinylthread_disable },
    { "checkinterrupt", tinylthread_check },
    { NULL, NULL }
  };
  luaL_Reg const thread_methods[] = {
    { "detach", tinylthread_detach },
    { "join", tinylthread_join },
    { "interrupt", tinylthread_interrupt },
    { "isdead", tinylthread_isdead },
    { "isdetached", tinylthread_isdetached },
    { NULL, NULL }
  };
  luaL_Reg const thread_metas[] = {
    { "__gc", tinylthread_gc },
    { "__tinylthread@pipe", (lua_CFunction)tinylthread_copy },
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
    { "__tinylthread@pipe", (lua_CFunction)tinylmutex_copy },
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
    { "__tinylthread@pipe", (lua_CFunction)tinylport_copy },
    { NULL, NULL }
  };
  luaL_Reg const itr_metas[] = {
    { "__tostring", tinylitr_tostring },
    { "__tinylthread@pipe", (lua_CFunction)tinylitr_copy },
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
  lua_setfield( L, LUA_REGISTRYINDEX, TLT_INTERRUPTED );
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

