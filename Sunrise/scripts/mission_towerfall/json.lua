-- Minimal JSON decoder (objects, arrays, strings with escapes, numbers, true/false/null).
-- null decodes to json.null so keys are not lost; everything else maps to plain Lua values.
local json = {}
json.null = setmetatable({}, { __tostring = function() return "null" end })

local function decode_error(text, pos, message)
    error(string.format("json: %s at %d (near %q)", message, pos, text:sub(pos, pos + 15)), 0)
end

local function skip_space(text, pos)
    local _, e = text:find("^[ \t\r\n]*", pos)
    return e + 1
end

local decode_value

local BACKSLASH = string.char(92)
local escapes = { ['"'] = '"', [BACKSLASH] = BACKSLASH, ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t' }

local function decode_string(text, pos)
    local parts, i = {}, pos + 1
    while true do
        local ch = text:sub(i, i)
        if ch == "" then decode_error(text, i, "unterminated string") end
        if ch == '"' then return table.concat(parts), i + 1 end
        if ch == BACKSLASH then
            local n = text:sub(i + 1, i + 1)
            if n == "u" then
                local hex = text:sub(i + 2, i + 5)
                local code = tonumber(hex, 16)
                if not code then decode_error(text, i, "bad unicode escape") end
                if code < 0x80 then parts[#parts + 1] = string.char(code)
                elseif code < 0x800 then
                    parts[#parts + 1] = string.char(0xC0 + math.floor(code / 0x40), 0x80 + code % 0x40)
                else
                    parts[#parts + 1] = string.char(0xE0 + math.floor(code / 0x1000),
                        0x80 + math.floor(code / 0x40) % 0x40, 0x80 + code % 0x40)
                end
                i = i + 6
            else
                local rep = escapes[n]
                if not rep then decode_error(text, i, "bad escape") end
                parts[#parts + 1] = rep
                i = i + 2
            end
        else
            local j = text:find('[' .. BACKSLASH .. '"]', i) or (#text + 1)
            parts[#parts + 1] = text:sub(i, j - 1)
            i = j
        end
    end
end

local function decode_number(text, pos)
    local s, e = text:find("^-?%d+%.?%d*[eE]?[-+]?%d*", pos)
    if not s then decode_error(text, pos, "bad number") end
    local value = tonumber(text:sub(s, e))
    if not value then decode_error(text, pos, "bad number") end
    return value, e + 1
end

local function decode_array(text, pos)
    local out, i = {}, skip_space(text, pos + 1)
    if text:sub(i, i) == "]" then return out, i + 1 end
    while true do
        local value
        value, i = decode_value(text, i)
        out[#out + 1] = value
        i = skip_space(text, i)
        local ch = text:sub(i, i)
        if ch == "]" then return out, i + 1 end
        if ch ~= "," then decode_error(text, i, "expected , or ]") end
        i = skip_space(text, i + 1)
    end
end

local function decode_object(text, pos)
    local out, i = {}, skip_space(text, pos + 1)
    if text:sub(i, i) == "}" then return out, i + 1 end
    while true do
        if text:sub(i, i) ~= '"' then decode_error(text, i, "expected key") end
        local key
        key, i = decode_string(text, i)
        i = skip_space(text, i)
        if text:sub(i, i) ~= ":" then decode_error(text, i, "expected :") end
        i = skip_space(text, i + 1)
        local value
        value, i = decode_value(text, i)
        out[key] = value
        i = skip_space(text, i)
        local ch = text:sub(i, i)
        if ch == "}" then return out, i + 1 end
        if ch ~= "," then decode_error(text, i, "expected , or }") end
        i = skip_space(text, i + 1)
    end
end

decode_value = function(text, pos)
    pos = skip_space(text, pos)
    local ch = text:sub(pos, pos)
    if ch == "{" then return decode_object(text, pos) end
    if ch == "[" then return decode_array(text, pos) end
    if ch == '"' then return decode_string(text, pos) end
    if ch == "-" or ch:match("%d") then return decode_number(text, pos) end
    if text:sub(pos, pos + 3) == "true" then return true, pos + 4 end
    if text:sub(pos, pos + 4) == "false" then return false, pos + 5 end
    if text:sub(pos, pos + 3) == "null" then return json.null, pos + 4 end
    decode_error(text, pos, "unexpected character")
end

function json.decode(text)
    local value, pos = decode_value(text, 1)
    pos = skip_space(text, pos)
    if pos <= #text then decode_error(text, pos, "trailing characters") end
    return value
end

return json
