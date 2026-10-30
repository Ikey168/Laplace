-- The Lua gate (#256): floats, tables, the garbage collector, and file I/O.
-- Journaled inputs, kept below: the clock (os.clock reads it to the
-- nanosecond) and random bytes.
local started = os.clock()
local noise = assert(io.open("/dev/urandom", "rb")):read(8)
local box = {0}
print(string.format("lua: box %p", box))
local acc = 0.0
for i = 1, 100000 do
  acc = acc + math.sin(i) * 0.5
  if i % 1000 == 0 then box[1] = i end
end
local keep = {}
for i = 1, 5000 do keep[i] = {i, tostring(i)} end
for i = 1, 5000, 2 do keep[i] = nil end
collectgarbage("collect")
local kept = 0
for i = 1, 5000 do if keep[i] then kept = kept + 1 end end
local path = "/tmp/gate.txt"
local f = assert(io.open(path, "w"))
for i = 1, 1000 do f:write(string.format("%4d\n", i)) end
f:close()
local size, sum = 0, 0
for line in io.lines(path) do
  size = size + #line + 1
  sum = sum + tonumber(line)
end
box.started, box.noise = started, noise
box[1] = 4242                              -- the store the gate finds backward
print(string.format("lua: acc %.6f, box %d, kept %d of 5000, file %d bytes, sum %d",
                    acc, box[1], kept, size, sum))
