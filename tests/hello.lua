#!/usr/bin/env lua

local tlt = require( "tinylthread" )


print( "hello from main thread" )
local th1 = tlt.thread[[
  print( "", "hello from child thread" )
]]
th1:join()


print( "and now passing arguments + return values:" )
local th2 = tlt.thread( [[
  print( "", "arguments:", ... )
  return 2, true, nil, "b"
]], 3, false, nil, "a" )
print( "", "results:", th2:join() )


print( "and now a thread raising an error:" )
local th3 = tlt.thread[[
  error( "argh!" )
]]
print( "", "results:", th3:join() )

