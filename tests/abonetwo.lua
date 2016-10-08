#!/usr/bin/env lua

local tlt = require( "tinylthread" )

print( "creating mutex" )
local mtx = tlt.mutex()

local code = [[
  local tlt = require( "tinylthread" )
  local mtx, one, two = ...

  while true do
    mtx:lock()
    print( one )
    tlt.nointerrupt()
    tlt.sleep( math.random()*0.3 )
    print( "", two )
    mtx:unlock()
    tlt.sleep( math.random()*0.6 )
  end
]]

print( "creating threads" )
local th1 = tlt.thread( code, mtx, 1, 2 )
local th2 = tlt.thread( code, mtx, "x", "y" )
local th3 = tlt.thread( code, mtx, "n", "m" )
local th4 = tlt.thread( code, mtx, "v", "w" )

for i = 1, 5 do
  mtx:lock()
  print( "a" )
  tlt.sleep( math.random()*0.3 )
  print( "", "b" )
  mtx:unlock()
  tlt.sleep( math.random()*0.6 )
end

tlt.sleep( 0.5 )
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

