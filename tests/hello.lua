#!/usr/bin/env

local tl = require( "tinylthread" )


print( "hello from main thread" )
local th1 = tl.thread[[
  print( "", "hello from child thread" )
]]
th1:join()


print( "and now passing arguments + return values:" )
local th2 = tl.thread( [[
  print( "", "arguments:", ... )
  return 2, true, nil, "b"
]], 3, false, nil, "a" )
print( "", "results:", th2:join() )


print( "and now a thread raising an error:" )
local th3 = tl.thread[[
  error( "argh!" )
]]
print( "", "results:", th3:join() )

