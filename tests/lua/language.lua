-- What a script can count on from the language itself: numbers and how they print and parse, math,
-- strings and patterns, tables, coroutines, errors, metatables and the collector. It is run with the
-- C library of the machine the checks run on and with the framework's own, and both have to give
-- exactly the same text.
local out = {}
local function say(...) local t = {} for i = 1, select('#', ...) do t[#t + 1] = tostring((select(i, ...))) end out[#out + 1] = table.concat(t, ' ') end

-- numbers and formatting
say(1, 1.0, -0.0, 0.1, 1/3, 2^53, 2^63, 1e300 * 1e10, -1e300 * 1e10, 100 // 7, -7 // 2, -7 % 3, 7.5 % -2, 3 / 2)
say(math.maxinteger, math.mininteger, math.maxinteger + 1 == math.mininteger, 0x10, 0xff, 0x1p4, 0x.8p1, 1e-310, 5e-324)
say(string.format('%5.2f|%-8s|%x|%X|%o|%05d|%+d|% d|%3c|%.3s', 3.14159, 'ab', 255, 255, 8, 42, 7, 7, 65, 'abcdef'))
say(string.format('%g %g %g %g %g %g', 0.0001, 0.00001, 123456, 1234567, 1e100, 2.5))
say(string.format('%.14g %.17g %.0f %.0f %.0f %.1f %.3e %E', 0.1, 0.1, 0.5, 1.5, 2.5, 0.05, 123456.789, 1e-5))
say(string.format('%10.4f|%-10.2e|%.0e|%#.0f|%#x|%.20f', math.pi, -math.pi, 5e15, 3.0, 255, 1/3))
say(string.format('%q', 'a\nb"c\0'), string.format('%q', 1/3), string.format('%q', math.mininteger))
say(string.format('%.14g', 123456789012345678), string.format('%.3f', 2.0005), string.format('%.2f', 1.005), string.format('%d', -0))
say(tonumber('10') + 1, '10' + 1, tonumber('0x10'), tonumber('1e5'), tonumber(' 12 '), tonumber('3.25e-2'), tonumber('.5'),
    tonumber('5.'), tonumber('1e'), tonumber('abc'), tonumber('10', 2), tonumber('zz', 36), tonumber('1e400'), tonumber('0x1p-2'))
say(math.floor(-3.5), math.ceil(-3.5), math.floor(3.7), math.fmod(7, 3), math.fmod(-7, 3), math.fmod(7.5, 2), math.fmod(-7.5, 2), math.abs(-4))
say(math.tointeger(3.0), math.type(1), math.type(1.0), math.type('1'), math.ult(1, -1), 7 // 0.0, -7 // 0.0, 0/0 ~= 0/0)
local function g(x) return string.format('%.12g', x) end
say(g(math.sin(1)), g(math.cos(1)), g(math.tan(1)), g(math.sin(100)), g(math.cos(-1000)), g(math.asin(0.5)), g(math.acos(0.5)))
-- Decimal text read to the nearest double, ties to even: large and small exponents, halfway cases, and more
-- digits than any double needs.
for _, text in ipairs({'1e100', '1e300', '1.7976931348623157e308', '1.7976931348623158e308', '1.797693134862315807e308',
                       '2.2250738585072014e-308', '2.2250738585072011e-308', '4.9e-324', '2.4703282292062327e-324',
                       '2.4703282292062328e-324', '0.1', '123456789012345678901234567890', '9007199254740993',
                       '1.00000000000000011102230246251565404236316680908203125', '1.00000000000000011102230246251565404236316680908203126',
                       '8.98846567431158e307', '1e-300', '3.141592653589793238462643383279502884197', '1e23', '7e-323',
                       '0.' .. string.rep('0', 340) .. '25', '1' .. string.rep('0', 400) .. 'e-400',
                       '9' .. string.rep('9', 899) .. 'e-900', '1.' .. string.rep('0', 1000) .. '1'}) do
    say(text:sub(1, 20) .. '..' .. text:sub(-6), string.format('%.17g', tonumber(text)))
end
-- Past where a simple reduction by pi/2 holds, up to the largest number.
-- Each is made exactly, without parsing a long literal: the largest is the largest double.
for _, x in ipairs({1e6, 1e7, -3e9, 1e15, 1e22, -(2^333) * 1.25, 2^996 * 1.75, 2^1023, 2^1023 + (2^1023 - 2^971)}) do
    say(g(math.sin(x)), g(math.cos(x)), g(math.tan(x)))
end
say(g(math.atan(1)), g(math.atan(1, -1)), g(math.atan(-2, -3)), g(math.exp(1)), g(math.exp(-20)), g(math.log(10)), g(math.log(8, 2)))
say(g(math.log(1000, 10)), g(math.log(2.5, 7)), g(math.sqrt(2)), g(2^0.5), g(10^-3), (-8)^(1/3) ~= (-8)^(1/3), g(1.5^20), g(math.pi))

-- strings and patterns
local s = 'The quick brown fox jumps over the lazy dog'
say(s:len(), s:upper(), s:sub(5, 9), s:sub(-3), s:rep(2, '|'):len(), s:byte(1, 3), s:find('fox'), s:find('o', 20, true))
say(s:match('(%a+) (%a+)$'), s:gsub('o', '0'), (s:gsub('%w+', string.upper, 2)), s:match('^(%w+)'), ('  trim  '):match('^%s*(.-)%s*$'))
local words = {} for w in s:gmatch('%a+') do words[#words + 1] = w end
say(#words, table.concat(words, ','), ('x=1, y=22'):gsub('(%w+)=(%w+)', '%2=%1'), ('%d'):rep(3), ('a.b'):find('.', 1, true))
say(string.pack('<i4d', 7, 1.5):byte(1, -1))
say(string.unpack('<i4d', string.pack('<i4d', -9, 0.25)))
say(utf8.char(72, 228, 8364, 128512), utf8.len('häé'), #utf8.char(128512), string.char(65, 66, 67), ('abc'):reverse())

-- tables
local t = {5, 2, 8, 1, 9, 3, 7}
table.sort(t) say(table.concat(t, ' '))
table.sort(t, function(a, b) return a > b end) say(table.concat(t, ' '))
local names = {} for i = 1, 200 do names[i] = string.format('n%03d', (i * 37) % 200) end
table.sort(names) say(names[1], names[100], names[200], #names)
table.insert(t, 1, 0) table.insert(t, 10) say(table.remove(t), table.remove(t, 1), #t, select('#', table.unpack({1, 2, nil, 4}, 1, 4)))
say(next({}), rawlen({1, 2}), rawequal(t, t), #table.pack(1, nil, 3), table.move({1, 2, 3}, 1, 3, 2)[3])

-- control flow, closures, coroutines, errors
local function counter() local n = 0 return function() n = n + 1 return n end end
local c = counter() c() c() say(c())
for i = 1, 3 do if i == 2 then goto continue end say('loop', i) ::continue:: end
local co = coroutine.create(function(a, b) local c2 = coroutine.yield(a + b) local d = coroutine.yield(c2 * 2) return d, 'done' end)
say(coroutine.resume(co, 1, 2)) say(coroutine.resume(co, 10)) say(coroutine.resume(co, 'x')) say(coroutine.resume(co)) say(coroutine.status(co))
local gen = coroutine.wrap(function() for i = 1, 3 do coroutine.yield(i * i) end end) say(gen(), gen(), gen())
say(pcall(error, 'plain')) say(type(select(2, pcall(error, {code = 7}))), select(2, pcall(error, {code = 7})).code) say(select(2, pcall(error, 'lvl', 0)))
say(pcall(function() local x = nil; return x.y end))
say(pcall(function() return 1 + {} end))
local depth = 0 local function deep() depth = depth + 1 return deep() + 1 end
local ok, msg = pcall(deep) say(ok, depth > 1000, tostring(msg):find('stack overflow') ~= nil)
say(xpcall(function() error('inner') end, function(m) return 'handled: ' .. m end))
say(select(-1, 1, 2, 3), select('#'), type(print), type(nil), tostring(true))

-- metatables and GC
local V = {} V.__index = V
V.__add = function(a, b) return setmetatable({x = a.x + b.x}, V) end
V.__tostring = function(v) return 'V(' .. v.x .. ')' end
V.__eq = function(a, b) return a.x == b.x end
V.__lt = function(a, b) return a.x < b.x end
V.__len = function() return 42 end
V.__call = function(self, y) return self.x * y end
local a, b = setmetatable({x = 1}, V), setmetatable({x = 2}, V)
say(tostring(a + b), a == setmetatable({x = 1}, V), a < b, #a, a(10), getmetatable(a) == V)
local collected = 0
for i = 1, 50 do setmetatable({}, {__gc = function() collected = collected + 1 end}) end
collectgarbage() collectgarbage()
say(collected, collectgarbage('count') > 0, math.type(collectgarbage('count')))
local big = {} for i = 1, 100000 do big[i] = i * 2 end
local sum = 0 for i = 1, #big do sum = sum + big[i] end say(sum, #big)
local str = {} for i = 1, 5000 do str[i] = tostring(i) end say(#table.concat(str))
return table.concat(out, '\n')
