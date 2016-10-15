#!/usr/bin/env lua

local tlt = require( "tinylthread" )

print( "creating mutex" )
local mtx = tlt.mutex()
print( "creating pipe" )
local rport, wport = tlt.pipe()

local code = [[
  local tlt = require( "tinylthread" )
  local mtx, port, id = ...

  while true do
    local v = port:read()
    tlt.nointerrupt()
    mtx:lock()
    print( "id:", id, "value:", v )
    mtx:unlock()
  end
]]

print( "creating threads" )
local th1 = tlt.thread( code, mtx, rport, 1 )
local th2 = tlt.thread( code, mtx, rport, 2 )
local th3 = tlt.thread( code, mtx, rport, 3 )
local th4 = tlt.thread( code, mtx, rport, 4 )

for i = 1, 20 do
  wport:write( i )
end

th1:interrupt()
th2:interrupt()
th3:interrupt()
th4:interrupt()

local function wait( th )
  local t = { th:join() }
  mtx:lock()
  print( (table.unpack or unpack)( t ) )
  mtx:unlock()
end

wait( th1 )
wait( th2 )
wait( th3 )
wait( th4 )

