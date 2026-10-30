-- Reads its input from the seed image, writes a file, reads it back (#247).
local motd = assert(io.open("/etc/motd")):read("a")
print("lua: motd says: " .. motd:gsub("\n$", ""))
local path = "/tmp/lua-out.txt"
local f = assert(io.open(path, "w"))
for i = 1, 200 do f:write(string.format("line %d: %d\n", i, i * i)) end
f:close()
local sum, lines = 0, 0
for line in io.lines(path) do
  local n = tonumber(line:match(": (%d+)$"))
  sum = sum + n
  lines = lines + 1
end
print(string.format("lua: wrote and read back %d lines, sum %d", lines, sum))
assert(os.rename(path, "/tmp/lua-renamed.txt"))
assert(io.open("/tmp/lua-renamed.txt")):close()
assert(os.remove("/tmp/lua-renamed.txt"))
print("lua: renamed and removed it; " .. (io.open(path) and "still there" or "gone"))
