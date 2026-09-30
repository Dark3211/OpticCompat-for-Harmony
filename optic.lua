clua_version = 2.056

local blam, json, deepcopy, util = (function()

local function field(kind, offset, bitLevel)
    local value = {type = kind, offset = offset}
    if bitLevel ~= nil then value.bitLevel = bitLevel end
    return value
end

local scalarReaders = {
    byte = read_byte,
    short = read_short,
    word = read_word,
    dword = read_dword,
    float = read_float
}

local readValue

local function readReflexive(address, definition)
    local count = read_byte(address - 0x4)
    local base = read_dword(address)
    local result = {}
    for i = 1, count do
        local itemAddress = base + (i - 1) * definition.jump
        local item = {}
        for name, property in pairs(definition.rows) do
            item[name] = readValue(itemAddress + property.offset, property)
        end
        result[i] = item
    end
    return result
end

readValue = function(address, definition)
    if definition.type == "bit" then
        local value = read_bit(address, definition.bitLevel)
        return value == 1 or value == true
    end
    if definition.type == "table" then
        return readReflexive(address, definition)
    end
    local reader = scalarReaders[definition.type]
    if not reader then error("Unsupported BLAM field type: " .. tostring(definition.type)) end
    return reader(address)
end

local bindingMeta = {
    __index = function(object, property)
        local definition = object.structure[property]
        if not definition then
            error("Unable to read an invalid property ('" .. tostring(property) .. "')", 2)
        end
        return readValue(object.address + definition.offset, definition)
    end
}

local function bind(address, structure)
    if not address or address == 0 then return nil end
    return setmetatable({address = address, structure = structure}, bindingMeta)
end

local bipedStructure = {
    tagId = field("dword", 0x0),
    maximumBodyVitality = field("float", 0xD8),
    maximumShieldVitality = field("float", 0xDC),
    health = field("float", 0xE0),
    shield = field("float", 0xE4),
    vehicleObjectId = field("dword", 0x11C),
    weaponPTH = field("bit", 0x208, 11),
    zoomLevel = field("byte", 0x320),
    mostRecentDamagerPlayer = field("dword", 0x43C)
}

local playerStructure = {
    id = field("word", 0x0),
    team = field("byte", 0x20),
    objectId = field("dword", 0x34),
    index = field("byte", 0x67),
    kills = field("word", 0x9C)
}

local weaponStructure = {tagId = field("dword", 0x0)}
local firstPersonStructure = {weaponObjectId = field("dword", 0x10)}

local bitmapStructure = {
    usageFlags = field("word", 0x6),
    sequences = {
        type = "table", offset = 0x58, jump = 0x40,
        rows = {
            firstBitmapIndex = field("word", 0x20),
            bitmapCount = field("word", 0x22),
            sprites = {
                type = "table", offset = 0x38, jump = 0x20,
                rows = {
                    bitmapIndex = field("word", 0x0),
                    left = field("float", 0x8),
                    right = field("float", 0xC),
                    top = field("float", 0x10),
                    bottom = field("float", 0x14)
                }
            }
        }
    },
    bitmaps = {
        type = "table", offset = 0x64, jump = 0x30,
        rows = {
            width = field("word", 0x4),
            height = field("word", 0x6),
            format = field("word", 0xC),
            hardwareFormat = field("dword", 0x28),
            baseAddress = field("dword", 0x2C)
        }
    }
}

local weaponHudStructure = {
    childHud = field("dword", 0xC),
    crosshairs = {
        type = "table", offset = 0x88, jump = 0x68,
        rows = {
            type = field("word", 0x0),
            bitmap = field("dword", 0x30),
            overlays = {
                type = "table", offset = 0x38, jump = 0x6C,
                rows = {
                    x = field("short", 0x0),
                    y = field("short", 0x2),
                    widthScale = field("float", 0x4),
                    heightScale = field("float", 0x8),
                    scalingFlags = field("word", 0xC),
                    sequenceIndex = field("short", 0x46),
                    flags = field("dword", 0x48)
                }
            }
        }
    }
}

local TAG_DATA_HEADER = 0x40440000
local FIRST_PERSON = 0x40000EB8

local function decodeTagClass(value)
    if value == nil then return nil end
    local hex = ("%08x"):format(value)
    return (hex:gsub("..", function(pair)
        return string.char(tonumber(pair, 16))
    end))
end

local blam = {}

function blam.isNull(value)
    return value == nil or value == 0xFF or value == 0xFFFF or value == 0xFFFFFFFF
end

function blam.getTag(idOrPath, tagClass)
    local address
    if type(idOrPath) == "number" then
        local id = idOrPath
        if id < 0xFFFF then
            local tagArray = read_dword(TAG_DATA_HEADER)
            id = read_dword(tagArray + id * 0x20 + 0xC)
        end
        address = get_tag(id)
    elseif type(idOrPath) == "string" then
        address = get_tag(tagClass, idOrPath)
    else
        return nil
    end
    if not address or address == 0 then return nil end
    local pathAddress = read_dword(address + 0x10)
    return {
        address = address,
        class = decodeTagClass(read_dword(address)),
        index = read_word(address + 0xC),
        id = read_dword(address + 0xC),
        path = pathAddress ~= 0 and read_string(pathAddress) or "",
        data = read_dword(address + 0x14),
        indexed = read_dword(address + 0x18)
    }
end

function blam.player(address)
    return bind(address, playerStructure)
end

function blam.biped(address)
    return bind(address, bipedStructure)
end

function blam.weapon(address)
    return bind(address, weaponStructure)
end

function blam.firstPerson(address)
    return bind(address or FIRST_PERSON, firstPersonStructure)
end

local function bindTag(tagId, structure)
    if tagId == nil or tagId == 0 then return nil end
    local tag = blam.getTag(tagId)
    if not tag or not tag.data or tag.data == 0 then return nil end
    return bind(tag.data, structure)
end

function blam.bitmap(tagId)
    return bindTag(tagId, bitmapStructure)
end

function blam.weaponHudInterface(tagId)
    return bindTag(tagId, weaponHudStructure)
end

local json = { _version = "0.1.2" }

local encode

local escape_char_map = {
  [ "\\" ] = "\\",
  [ "\"" ] = "\"",
  [ "\b" ] = "b",
  [ "\f" ] = "f",
  [ "\n" ] = "n",
  [ "\r" ] = "r",
  [ "\t" ] = "t",
}

local escape_char_map_inv = { [ "/" ] = "/" }
for k, v in pairs(escape_char_map) do
  escape_char_map_inv[v] = k
end

local function escape_char(c)
  return "\\" .. (escape_char_map[c] or string.format("u%04x", c:byte()))
end

local function encode_nil(val)
  return "null"
end

local function encode_table(val, stack)
  local res = {}
  stack = stack or {}

  if stack[val] then error("circular reference") end

  stack[val] = true

  if rawget(val, 1) ~= nil or next(val) == nil then

    local n = 0
    for k in pairs(val) do
      if type(k) ~= "number" then
        error("invalid table: mixed or invalid key types")
      end
      n = n + 1
    end
    if n ~= #val then
      error("invalid table: sparse array")
    end

    for i, v in ipairs(val) do
      table.insert(res, encode(v, stack))
    end
    stack[val] = nil
    return "[" .. table.concat(res, ",") .. "]"

  else

    for k, v in pairs(val) do
      if type(k) ~= "string" then
        error("invalid table: mixed or invalid key types")
      end
      table.insert(res, encode(k, stack) .. ":" .. encode(v, stack))
    end
    stack[val] = nil
    return "{" .. table.concat(res, ",") .. "}"
  end
end

local function encode_string(val)
  return '"' .. val:gsub('[%z\1-\31\\"]', escape_char) .. '"'
end

local function encode_number(val)

  if val ~= val or val <= -math.huge or val >= math.huge then
    error("unexpected number value '" .. tostring(val) .. "'")
  end
  return string.format("%.14g", val)
end

local type_func_map = {
  [ "nil"     ] = encode_nil,
  [ "table"   ] = encode_table,
  [ "string"  ] = encode_string,
  [ "number"  ] = encode_number,
  [ "boolean" ] = tostring,
}

encode = function(val, stack)
  local t = type(val)
  local f = type_func_map[t]
  if f then
    return f(val, stack)
  end
  error("unexpected type '" .. t .. "'")
end

function json.encode(val)
  return ( encode(val) )
end

local parse

local function create_set(...)
  local res = {}
  for i = 1, select("#", ...) do
    res[ select(i, ...) ] = true
  end
  return res
end

local space_chars   = create_set(" ", "\t", "\r", "\n")
local delim_chars   = create_set(" ", "\t", "\r", "\n", "]", "}", ",")
local escape_chars  = create_set("\\", "/", '"', "b", "f", "n", "r", "t", "u")
local literals      = create_set("true", "false", "null")

local literal_map = {
  [ "true"  ] = true,
  [ "false" ] = false,
  [ "null"  ] = nil,
}

local function next_char(str, idx, set, negate)
  for i = idx, #str do
    if set[str:sub(i, i)] ~= negate then
      return i
    end
  end
  return #str + 1
end

local function decode_error(str, idx, msg)
  local line_count = 1
  local col_count = 1
  for i = 1, idx - 1 do
    col_count = col_count + 1
    if str:sub(i, i) == "\n" then
      line_count = line_count + 1
      col_count = 1
    end
  end
  error( string.format("%s at line %d col %d", msg, line_count, col_count) )
end

local function codepoint_to_utf8(n)

  local f = math.floor
  if n <= 0x7f then
    return string.char(n)
  elseif n <= 0x7ff then
    return string.char(f(n / 64) + 192, n % 64 + 128)
  elseif n <= 0xffff then
    return string.char(f(n / 4096) + 224, f(n % 4096 / 64) + 128, n % 64 + 128)
  elseif n <= 0x10ffff then
    return string.char(f(n / 262144) + 240, f(n % 262144 / 4096) + 128,
                       f(n % 4096 / 64) + 128, n % 64 + 128)
  end
  error( string.format("invalid unicode codepoint '%x'", n) )
end

local function parse_unicode_escape(s)
  local n1 = tonumber( s:sub(1, 4),  16 )
  local n2 = tonumber( s:sub(7, 10), 16 )

  if n2 then
    return codepoint_to_utf8((n1 - 0xd800) * 0x400 + (n2 - 0xdc00) + 0x10000)
  else
    return codepoint_to_utf8(n1)
  end
end

local function parse_string(str, i)
  local res = ""
  local j = i + 1
  local k = j

  while j <= #str do
    local x = str:byte(j)

    if x < 32 then
      decode_error(str, j, "control character in string")

    elseif x == 92 then
      res = res .. str:sub(k, j - 1)
      j = j + 1
      local c = str:sub(j, j)
      if c == "u" then
        local hex = str:match("^[dD][89aAbB]%x%x\\u%x%x%x%x", j + 1)
                 or str:match("^%x%x%x%x", j + 1)
                 or decode_error(str, j - 1, "invalid unicode escape in string")
        res = res .. parse_unicode_escape(hex)
        j = j + #hex
      else
        if not escape_chars[c] then
          decode_error(str, j - 1, "invalid escape char '" .. c .. "' in string")
        end
        res = res .. escape_char_map_inv[c]
      end
      k = j + 1

    elseif x == 34 then
      res = res .. str:sub(k, j - 1)
      return res, j + 1
    end

    j = j + 1
  end

  decode_error(str, i, "expected closing quote for string")
end

local function parse_number(str, i)
  local x = next_char(str, i, delim_chars)
  local s = str:sub(i, x - 1)
  local n = tonumber(s)
  if not n then
    decode_error(str, i, "invalid number '" .. s .. "'")
  end
  return n, x
end

local function parse_literal(str, i)
  local x = next_char(str, i, delim_chars)
  local word = str:sub(i, x - 1)
  if not literals[word] then
    decode_error(str, i, "invalid literal '" .. word .. "'")
  end
  return literal_map[word], x
end

local function parse_array(str, i)
  local res = {}
  local n = 1
  i = i + 1
  while 1 do
    local x
    i = next_char(str, i, space_chars, true)

    if str:sub(i, i) == "]" then
      i = i + 1
      break
    end

    x, i = parse(str, i)
    res[n] = x
    n = n + 1

    i = next_char(str, i, space_chars, true)
    local chr = str:sub(i, i)
    i = i + 1
    if chr == "]" then break end
    if chr ~= "," then decode_error(str, i, "expected ']' or ','") end
  end
  return res, i
end

local function parse_object(str, i)
  local res = {}
  i = i + 1
  while 1 do
    local key, val
    i = next_char(str, i, space_chars, true)

    if str:sub(i, i) == "}" then
      i = i + 1
      break
    end

    if str:sub(i, i) ~= '"' then
      decode_error(str, i, "expected string for key")
    end
    key, i = parse(str, i)

    i = next_char(str, i, space_chars, true)
    if str:sub(i, i) ~= ":" then
      decode_error(str, i, "expected ':' after key")
    end
    i = next_char(str, i + 1, space_chars, true)

    val, i = parse(str, i)

    res[key] = val

    i = next_char(str, i, space_chars, true)
    local chr = str:sub(i, i)
    i = i + 1
    if chr == "}" then break end
    if chr ~= "," then decode_error(str, i, "expected '}' or ','") end
  end
  return res, i
end

local char_func_map = {
  [ '"' ] = parse_string,
  [ "0" ] = parse_number,
  [ "1" ] = parse_number,
  [ "2" ] = parse_number,
  [ "3" ] = parse_number,
  [ "4" ] = parse_number,
  [ "5" ] = parse_number,
  [ "6" ] = parse_number,
  [ "7" ] = parse_number,
  [ "8" ] = parse_number,
  [ "9" ] = parse_number,
  [ "-" ] = parse_number,
  [ "t" ] = parse_literal,
  [ "f" ] = parse_literal,
  [ "n" ] = parse_literal,
  [ "[" ] = parse_array,
  [ "{" ] = parse_object,
}

parse = function(str, idx)
  local chr = str:sub(idx, idx)
  local f = char_func_map[chr]
  if f then
    return f(str, idx)
  end
  decode_error(str, idx, "unexpected character '" .. chr .. "'")
end

function json.decode(str)
  if type(str) ~= "string" then
    error("expected argument of type string, got " .. type(str))
  end
  local res, idx = parse(str, next_char(str, 1, space_chars, true))
  idx = next_char(str, idx, space_chars, true)
  if idx <= #str then
    decode_error(str, idx, "trailing garbage")
  end
  return res
end


local function updateTable(destination, source)
    if source then
        for key, value in pairs(source) do destination[key] = value end
    end
    return destination
end

local function deepcopy(value, seen)
    if type(value) ~= "table" then return value end
    seen = seen or {}
    if seen[value] then return seen[value] end
    local copy = {}
    seen[value] = copy
    for key, item in pairs(value) do
        copy[deepcopy(key, seen)] = deepcopy(item, seen)
    end
    return setmetatable(copy, deepcopy(getmetatable(value), seen))
end

local function splitPlain(value, separator)
    if separator == nil or separator == "" then return 1 end
    local result, position = {}, 1
    while true do
        local first, last = string.find(value, separator, position, true)
        if not first then
            result[#result + 1] = string.sub(value, position)
            return result
        end
        result[#result + 1] = string.sub(value, position, first - 1)
        position = last + 1
    end
end

local function createSprites(size)
    local result = {}
    local standard = {
        {"kill", "normal_kill"}, {"rocketKill", "rocket_kill"},
        {"supercombine", "needler_kill"}, {"doubleKill", "double_kill"},
        {"tripleKill", "triple_kill"}, {"overkill", "overkill"},
        {"killtacular", "killtacular"}, {"killtrocity", "killtrocity"},
        {"killimanjaro", "killimanjaro"}, {"killtastrophe", "killtastrophe"},
        {"killpocalypse", "killpocalypse"}, {"killionaire", "killionaire"},
        {"killingSpree", "killing_spree"}, {"killingFrenzy", "killing_frenzy"},
        {"runningRiot", "running_riot"}, {"rampage", "rampage"},
        {"comebackKill", "comeback_kill"}, {"firstStrike", "first_strike"},
        {"fromTheGrave", "from_the_grave"}, {"closeCall", "close_call"},
        {"snapshot", "snapshot"}, {"flagCaptured", "flag_captured"},
        {"flagRunner", "flag_runner"}, {"flagChampion", "flag_champion"}
    }
    for _, item in ipairs(standard) do
        result[item[1]] = {name = item[2], width = size, height = size}
    end

    local aliases = {
        untouchable = {"untouchable", "nightmare"},
        invincible = {"invincible", "boogeyman"},
        inconceivable = {"inconceivable", "grim_reaper"},
        unfriggenbelievable = {"unfriggenbelievable", "demon"}
    }
    for key, item in pairs(aliases) do
        result[key] = {name = item[1], width = size, height = size, alias = item[2]}
    end

    result.headShot = {name = "headshot", message = "Head Shot", width = size, height = size}

    local hitmarkers = {
        hitmarkerHit = "hitmarker",
        hitmarkerShield = "hitmarker_shield",
        hitmarkerShieldBroken = "hitmarker_shield_broken",
        hitmarkerVehicle = "hitmarker_vehicle",
        hitmarkerCritical = "hitmarker_critical",
        hitmarkerKill = "hitmarker_kill"
    }
    for key, name in pairs(hitmarkers) do
        result[key] = {
            name = name,
            width = size,
            height = size,
            renderGroup = "crosshair",
            noHudMessage = true
        }
    end

    local iconSize = size * 0.42
    local icons = {
        hitmarkerCriticalIcon = "hitmarker_critical_icon",
        hitmarkerShieldIcon = "hitmarker_shield_icon",
        hitmarkerShieldBrokenIcon = "hitmarker_shield_broken_icon",
        hitmarkerVehicleIcon = "hitmarker_vehicle_icon"
    }
    for key, name in pairs(icons) do
        result[key] = {name = name, width = iconSize, height = iconSize, noHudMessage = true}
    end
    return result
end

local function newHitmarkerCache()
    return {
        hitmarker = {},
        hitmarker_shield = {},
        hitmarker_shield_broken = {},
        hitmarker_vehicle = {},
        hitmarker_critical = {},
        hitmarker_kill = {}
    }
end

return blam, json, deepcopy, {
    update = updateTable,
    split = splitPlain,
    createSprites = createSprites,
    newHitmarkerCache = newHitmarkerCache
}
end)()

local harmony = require "mods.harmony"
local optic = harmony.optic

local opticVersion = "3.9.1-pro"

local configuration = {
    enableSound = true,
    hitmarker = true,
    hudMessages = true,
    style = "halo_4",
    volume = 50,

    damageNumbers = true,
    criticalHitmarker = true,
    shieldHitmarker = true,
    shieldBreakHitmarker = true,
    vehicleHitmarker = true,
    headshotMedal = true,
    nativeDamageTelemetry = true,

    headshotNodePatterns = {"head", "helmet"},

    combatTelemetryVersion = 5,
    criticalDamageThreshold = 48,
    headshotMinDamage = 30,
    headshotWeaponPatterns = {"sniper", "pistol", "magnum"},

    hitmarkerNormalColor = 1,
    hitmarkerCriticalColor = 2,
    damageNormalColor = 1,
    damageCriticalColor = 2
}

local OPTIC_COLOR_PALETTE = {
    {id=1,  name="white",     hex="#FFFFFF", r=255, g=255, b=255},
    {id=2,  name="light_red", hex="#FF5C5C", r=255, g=92,  b=92},
    {id=3,  name="red",       hex="#FF3030", r=255, g=48,  b=48},
    {id=4,  name="orange",    hex="#FF9A3D", r=255, g=154, b=61},
    {id=5,  name="gold",      hex="#FFD54A", r=255, g=213, b=74},
    {id=6,  name="yellow",    hex="#FFF45C", r=255, g=244, b=92},
    {id=7,  name="lime",      hex="#B7FF4A", r=183, g=255, b=74},
    {id=8,  name="green",     hex="#4DFF88", r=77,  g=255, b=136},
    {id=9,  name="mint",      hex="#54FFD0", r=84,  g=255, b=208},
    {id=10, name="cyan",      hex="#4DEBFF", r=77,  g=235, b=255},
    {id=11, name="sky",       hex="#5CB6FF", r=92,  g=182, b=255},
    {id=12, name="blue",      hex="#4B72FF", r=75,  g=114, b=255},
    {id=13, name="indigo",    hex="#665CFF", r=102, g=92,  b=255},
    {id=14, name="violet",    hex="#9A5CFF", r=154, g=92,  b=255},
    {id=15, name="purple",    hex="#C45CFF", r=196, g=92,  b=255},
    {id=16, name="magenta",   hex="#FF5CE1", r=255, g=92,  b=225},
    {id=17, name="pink",      hex="#FF72A8", r=255, g=114, b=168},
    {id=18, name="silver",    hex="#D8DEE9", r=216, g=222, b=233},
    {id=19, name="gray",      hex="#A0A8B5", r=160, g=168, b=181},
    {id=20, name="teal",      hex="#38D9C5", r=56,  g=217, b=197}
}

local OPTIC_COLOR_BY_NAME = {}
for _, color in ipairs(OPTIC_COLOR_PALETTE) do
    OPTIC_COLOR_BY_NAME[color.name] = color
    OPTIC_COLOR_BY_NAME[color.name:gsub("_", "")] = color
end

local function resolveOpticPaletteColor(value, fallbackId)
    local numeric = tonumber(value)
    if numeric then
        numeric = math.floor(numeric)
        if numeric >= 1 and numeric <= #OPTIC_COLOR_PALETTE then
            return OPTIC_COLOR_PALETTE[numeric]
        end
    end

    local key = tostring(value or ""):lower():gsub("[%s%-]", "_")
    local color = OPTIC_COLOR_BY_NAME[key] or OPTIC_COLOR_BY_NAME[key:gsub("_", "")]
    if color then return color end

    return OPTIC_COLOR_PALETTE[fallbackId or 1]
end

local function configuredOpticColor(configKey, fallbackId)
    return resolveOpticPaletteColor(configuration[configKey], fallbackId)
end

local function hitmarkerOverrideColor(name)
    if name == "hitmarker" then
        return configuredOpticColor("hitmarkerNormalColor", 1)
    elseif name == "hitmarker_critical" then
        return configuredOpticColor("hitmarkerCriticalColor", 2)
    end
    return nil
end

local function damageOverrideColor(critical)
    if critical then
        return configuredOpticColor("damageCriticalColor", 2)
    end
    return configuredOpticColor("damageNormalColor", 1)
end

local events = {
    fallingDead = "falling dead",
    guardianKill = "guardian kill",
    vehicleKill = "vehicle kill",
    playerKill = "player kill",
    betrayed = "betrayed",
    suicide = "suicide",
    localKilledPlayer = "local killed player",
    localDoubleKill = "local double kill",
    localTripleKill = "local triple kill",
    localKilltacular = "local killtacular",
    localKillingSpree = "local killing spree",
    localRunningRiot = "local running riot",
    localCtfScore = "local ctf score",
    ctfEnemyScore = "ctf enemy score",
    ctfAllyScore = "ctf ally score",
    ctfEnemyStoleFlag = "ctf enemy stole flag",
    ctfEnemyReturnedFLag = "ctf enemy returned flag",
    ctfAllyStoleFlag = "ctf ally stole flag",
    ctfAllyReturnedFlag = "ctf ally returned flag",
    ctfFriendlyFlagIdleReturned = "ctf friendly flag idle returned",
    ctfEnemyFlagIdleReturned = "ctf enemy flag idle returned"
}

local deathEvents = {
    [events.fallingDead] = true,
    [events.guardianKill] = true,
    [events.vehicleKill] = true,
    [events.playerKill] = true,
    [events.betrayed] = true,
    [events.suicide] = true
}

local soundsEvents = {hitmarker = "ting"}

local imagesPath = "%s/images/%s.png"
local soundsPath = "%s/sounds/%s.mp3"
local opticStylePath = "%s/sprites.style"
local playerData = {
    deaths = 0,
    kills = 0,
    noKillSinceDead = false,
    killingSpreeCount = 0,
    dyingSpreeCount = 0,
    multiKillCount = 0,
    multiKillTimestamp = nil,
    flagCaptures = 0
}
local defaultPlayerData = deepcopy(playerData)

local screenWidth = read_word(0x637CF2)
local screenHeight = read_word(0x637CF0)

if type(optic.get_resolution) == "function" then
    screenWidth, screenHeight = optic.get_resolution()
end
if not screenWidth or not screenHeight or
   screenWidth <= 0 or screenHeight <= 0 then
    screenWidth = read_word(0x637CF2)
    screenHeight = read_word(0x637CF0)
end

local defaultMedalSize = (screenHeight / 15) - 1
local medalsLoaded = false

local function image(spriteName)
    return imagesPath:format(configuration.style, spriteName)
end

local function audio(spriteName)
    return soundsPath:format(configuration.style, spriteName)
end

local sprites
local sounds
local medalsQueue = {}
local harmonySprites = {}
local harmonySounds = {}

local harmonySpritePaths = {}
local hitmarkerSpriteCache = util.newHitmarkerCache()
local hitmarkerImageBoundsCache = {}
local weaponHudTagCache = {}
local haloReticleAlphaCache = {}
local hitModelNameCache = {}
local proceduralHitmarkerCache = util.newHitmarkerCache()
local proceduralHitFlashCache = util.newHitmarkerCache()
local proceduralHitmarkerFxCache = util.newHitmarkerCache()
local damageNumberSpriteCache = {}
local damageNumberFadeAnimation
local damageNumberRenderQueues = {}
local damageNumberNextQueue = 1

local HITMARKER_ARM_LENGTH_1080 = 11.0

local HITMARKER_ARM_THICKNESS_1080 = 2.70

local HITMARKER_KILL_ARM_THICKNESS_1080 = 3.25

local HITMARKER_RETICLE_PADDING_1080 = 6.0

local HITMARKER_KILL_EXTRA_PADDING_1080 = 2.0

local HITMARKER_FLASH_LENGTH_1080 = 14.0
local HITMARKER_FLASH_THICKNESS_1080 = 2.2

local HITMARKER_COMBO_WINDOW_MS = 350
local HITMARKER_COMBO_MAX_VISUAL_LEVEL = 3

local HITMARKER_KILL_SOUND_SUPPRESS_MS = 260

local HITMARKER_NORMAL_CONFIRM_DELAY_MS = 95
local HITMARKER_SHIELD_CONFIRM_DELAY_MS = 175

local HITMARKER_SHIELD_BREAK_CRITICAL_FOLLOWUP_MS = 115
local HITMARKER_KILL_DEDUPE_WINDOW_MS = 360

local HITMARKER_NORMAL_MOTION_1080 = 8.0

local HITMARKER_KILL_EXPANSION_1080 = 32.0

local DAMAGE_TRACK_EPSILON = 0.0005

local DAMAGE_HIT_MATCH_WINDOW_MS = 560
local DAMAGE_UNPAIRED_LIFETIME_MS = 900
local DAMAGE_NUMBER_GROUP_DELAY_MS = 35
local DAMAGE_NUMBER_GROUP_WINDOW_MS = 110
local DAMAGE_NUMBER_DURATION_MS = 560
local DAMAGE_NUMBER_Y_OFFSET_1080 = 56.0
local DAMAGE_NUMBER_MAX_LINES = 3
local DAMAGE_NUMBER_LINE_GAP_1080 = -10.0
local DAMAGE_KILL_MATCH_WINDOW_MS = 1400
local HEADSHOT_CONFIRM_WINDOW_MS = 700
local HEADSHOT_SHIELD_EPSILON = 0.015
local VICTIM_SNAPSHOT_GRACE_MS = 1400

local hitmarkerComboState = {
    count = 0,
    lastHitMs = nil,
    suppressWhiteUntilMs = nil
}

local pendingNormalHitmarkers = {}
local pendingCriticalFollowups = {}

local damageTracker = {
    players = {},
    nextPlayers = {},
    scanPlayers = {},
    candidates = {},
    unpaired = {},
    recentLocalDamage = {},
    lastLocalDamage = nil,
    lastHitSoundMs = nil,
    lastHitWeaponPath = nil,
    display = {amount = 0, critical = false, dueMs = nil, startedMs = nil}
}

local NATIVE_DAMAGE_MATCH_WINDOW_MS = 760
local NATIVE_DAMAGE_HISTORY_MS = 1400
local NATIVE_SOUND_MATCH_WINDOW_MS = 190
local nativeDamageTracker = {
    available = nil,
    recent = {}
}

local HITMARKER_EFFECT_KIND = {
    hitmarker = 0,
    hitmarker_kill = 1,
    hitmarker_critical = 2,
    hitmarker_shield = 3,
    hitmarker_shield_broken = 4,
    hitmarker_vehicle = 5
}

local HITMARKER_STATUS_ICON = {
    hitmarker_critical = "hitmarker_critical_icon",
    hitmarker_shield = "hitmarker_shield_icon",
    hitmarker_shield_broken = "hitmarker_shield_broken_icon",
    hitmarker_vehicle = "hitmarker_vehicle_icon"
}

local function isHitmarkerFxName(name)
    return HITMARKER_EFFECT_KIND[name] ~= nil
end

local function getHitmarkerEffectKind(name)
    return HITMARKER_EFFECT_KIND[name] or 0
end

local function getHitmarkerSpriteForKind(kind)
    if kind == "critical" and configuration.criticalHitmarker then
        return sprites.hitmarkerCritical
    elseif kind == "shield_broken" and configuration.shieldBreakHitmarker then
        return sprites.hitmarkerShieldBroken
    elseif kind == "vehicle" and configuration.vehicleHitmarker then
        return sprites.hitmarkerVehicle
    elseif kind == "shield" and configuration.shieldHitmarker then
        return sprites.hitmarkerShield
    end
    return sprites.hitmarkerHit
end

local function getHitmarkerResolutionScale()
    local resolutionScale = screenHeight / 1080.0
    return math.max(0.65, math.min(2.5, resolutionScale))
end

local function getHitmarkerGeometry(name, extraPadding)
    local resolutionScale = getHitmarkerResolutionScale()

    local armLength =
        HITMARKER_ARM_LENGTH_1080 * resolutionScale

    local armThickness =
        HITMARKER_ARM_THICKNESS_1080 * resolutionScale

    local padding =
        HITMARKER_RETICLE_PADDING_1080 * resolutionScale

    if name == "hitmarker_kill" then
        armThickness =
            HITMARKER_KILL_ARM_THICKNESS_1080 * resolutionScale

        padding = padding +
            HITMARKER_KILL_EXTRA_PADDING_1080 * resolutionScale
    end

    padding = padding + (tonumber(extraPadding) or 0)

    return armLength, armThickness, padding
end

local function getHitmarkerNowMs()
    if harmony.time and
       type(harmony.time.get_milliseconds) == "function" then
        return tonumber(harmony.time.get_milliseconds()) or 0
    end

    return math.floor((os.clock() or 0) * 1000)
end

local function resetHitmarkerCombo()
    hitmarkerComboState.count = 0
    hitmarkerComboState.lastHitMs = nil
end

local function registerHitmarkerHit()
    local now = getHitmarkerNowMs()
    local last = hitmarkerComboState.lastHitMs

    if last and (now - last) <= HITMARKER_COMBO_WINDOW_MS then
        hitmarkerComboState.count = hitmarkerComboState.count + 1
    else
        hitmarkerComboState.count = 1
    end

    hitmarkerComboState.lastHitMs = now

    return math.min(
        hitmarkerComboState.count,
        HITMARKER_COMBO_MAX_VISUAL_LEVEL
    )
end

local function getHitmarkerComboLevel()
    local now = getHitmarkerNowMs()
    local last = hitmarkerComboState.lastHitMs

    if not last or (now - last) > HITMARKER_COMBO_WINDOW_MS then
        return 1
    end

    return math.max(
        1,
        math.min(
            hitmarkerComboState.count,
            HITMARKER_COMBO_MAX_VISUAL_LEVEL
        )
    )
end

local function prepareHitmarkerKillCombo()
    local now = getHitmarkerNowMs()
    local last = hitmarkerComboState.lastHitMs

    if not last or
       (now - last) > HITMARKER_KILL_SOUND_SUPPRESS_MS then
        registerHitmarkerHit()
        now = getHitmarkerNowMs()
    end

    local level = getHitmarkerComboLevel()

    hitmarkerComboState.suppressWhiteUntilMs =
        now + HITMARKER_KILL_SOUND_SUPPRESS_MS

    return level
end

local function shouldSuppressPostKillWhiteHitmarker()
    local untilMs =
        hitmarkerComboState.suppressWhiteUntilMs

    if not untilMs then
        return false
    end

    local now = getHitmarkerNowMs()

    if now <= untilMs then
        hitmarkerComboState.suppressWhiteUntilMs = nil
        return true
    end

    hitmarkerComboState.suppressWhiteUntilMs = nil
    return false
end

local function queueNormalHitmarker(comboLevel)
    local now = getHitmarkerNowMs()

    local pending = {
        createdMs = now,
        dueMs = now + HITMARKER_NORMAL_CONFIRM_DELAY_MS,
        comboLevel = comboLevel,
        damage = nil,
        critical = false,
        followupCritical = false,
        hitmarkerKind = "normal",
        damageEvent = nil
    }

    for index = #damageTracker.unpaired, 1, -1 do
        local event = damageTracker.unpaired[index]
        local age = math.abs(now - event.timeMs)

        if age <= DAMAGE_HIT_MATCH_WINDOW_MS then
            pending.damage = event.damage
            pending.critical = event.critical
            pending.followupCritical = event.followupCritical == true
            pending.hitmarkerKind = event.hitmarkerKind or
                (event.critical and "critical" or "normal")
            pending.damageEvent = event
            event.hitMatched = true
            table.remove(damageTracker.unpaired, index)
            break
        end
    end

    pendingNormalHitmarkers[#pendingNormalHitmarkers + 1] = pending
    damageTracker.lastHitSoundMs = now

    return pending
end

local function cancelPendingNormalHitmarkerForKill()
    local now = getHitmarkerNowMs()

    if #pendingCriticalFollowups > 0 then
        pendingCriticalFollowups = {}
    end

    for index = #pendingNormalHitmarkers, 1, -1 do
        local pending = pendingNormalHitmarkers[index]
        local age = now - pending.createdMs

        if age >= 0 and age <= HITMARKER_KILL_DEDUPE_WINDOW_MS then
            table.remove(pendingNormalHitmarkers, index)

            return true
        end
    end

    return false
end

local function safeGetTag(tagId)
    if not tagId or blam.isNull(tagId) then
        return nil
    end
    local ok, tag = pcall(blam.getTag, tagId)
    if ok then
        return tag
    end
    return nil
end

local WEAPON_HUD_INTERFACE_TAG_ID_OFFSET = 0x48C

local function getWeaponHudTag(weaponTag)
    if not weaponTag or not weaponTag.data then
        return nil
    end

    local cacheKey = weaponTag.id or weaponTag.data
    local cached = weaponHudTagCache[cacheKey]
    if cached ~= nil then
        return cached or nil
    end

    local hudAddress = weaponTag.data + WEAPON_HUD_INTERFACE_TAG_ID_OFFSET
    local hudTagId = read_dword(hudAddress)

    if hudTagId and not blam.isNull(hudTagId) then
        local tag = safeGetTag(hudTagId)
        if tag and tag.class == "wphi" then
            weaponHudTagCache[cacheKey] = tag
            return tag
        end
    end

    weaponHudTagCache[cacheKey] = false
    return nil
end

local function getCurrentWeaponTag()
    local weaponObjectId

    if type(optic.get_first_person_weapon_object_id) == "function" then
        local ok, value = pcall(optic.get_first_person_weapon_object_id)
        if ok then weaponObjectId = value end
    end

    if not weaponObjectId or blam.isNull(weaponObjectId) then
        local firstPerson = blam.firstPerson()
        if not firstPerson or blam.isNull(firstPerson.weaponObjectId) then
            weaponHudTagCache.__currentWeapon = nil
            return nil
        end
        weaponObjectId = firstPerson.weaponObjectId
    end

    local cached = weaponHudTagCache.__currentWeapon
    if cached and cached.objectId == weaponObjectId then
        return cached.tag
    end

    local weaponAddress = get_object(weaponObjectId)
    if not weaponAddress then
        weaponHudTagCache.__currentWeapon = nil
        return nil
    end

    local weapon = blam.weapon(weaponAddress)
    if not weapon or blam.isNull(weapon.tagId) then
        weaponHudTagCache.__currentWeapon = nil
        return nil
    end

    local tag = safeGetTag(weapon.tagId)
    weaponHudTagCache.__currentWeapon = {
        objectId = weaponObjectId,
        tag = tag
    }
    return tag
end

local function safePlayer(index)
    local okAddress, address = pcall(get_player, index)
    if not okAddress or not address then return nil end

    local ok, player = pcall(blam.player, address)
    if ok then return player end
    return nil
end

local function safeBiped(index)
    local okAddress, address = pcall(get_dynamic_player, index)
    if not okAddress or not address then return nil end

    local ok, biped = pcall(blam.biped, address)
    if ok then return biped end
    return nil
end

local function canonicalPlayerTeam(player)
    local team = tonumber(player and player.team)
    if team == 0 or team == 1 then return team end
    return nil
end

local function detectTeamCombatContext(localPlayer)
    local localTeam = canonicalPlayerTeam(localPlayer)
    if localTeam == nil then return false, nil end

    local localIndex = tonumber(localPlayer and localPlayer.index)
    local hasOpponent = false

    for playerIndex = 0, 15 do
        if playerIndex ~= localIndex then
            local player = safePlayer(playerIndex)
            if player then
                local team = canonicalPlayerTeam(player)
                if team ~= nil and team ~= localTeam then
                    hasOpponent = true
                    break
                end
            end
        end
    end

    return hasOpponent, localTeam
end

local function isFriendlySnapshot(snapshot, localTeam, teamGameLikely)
    if not teamGameLikely or localTeam == nil or not snapshot then
        return false
    end
    local team = tonumber(snapshot.team)
    return team ~= nil and team == localTeam
end

local function isLocalDamager(damager, localPlayer)
    local value = tonumber(damager)
    if not value or not localPlayer then return false end

    local lowWord = value % 0x10000
    local playerId = tonumber(localPlayer.id)
    if playerId and
       (value == playerId or lowWord == (playerId % 0x10000)) then
        return true
    end

    local playerIndex = tonumber(localPlayer.index)
    return playerIndex ~= nil and
           (value == playerIndex or lowWord == (playerIndex % 0x10000))
end

local function isHeadshotCapableWeapon(path)
    path = tostring(path or ""):lower()

    local patterns = configuration.headshotWeaponPatterns
    if type(patterns) ~= "table" then
        patterns = {"sniper", "pistol", "magnum"}
    end

    for _, pattern in ipairs(patterns) do
        pattern = tostring(pattern or ""):lower()
        if pattern ~= "" and path:find(pattern, 1, true) then
            return true
        end
    end

    return false
end

local function sameHaloDatumId(a, b)
    a = tonumber(a)
    b = tonumber(b)
    if not a or not b or a < 0 or b < 0 then return false end
    if a == 0xFFFFFFFF or b == 0xFFFFFFFF then return false end
    return a == b or (a % 0x10000) == (b % 0x10000)
end

local function isLocalResponsibleUnit(unitId, localPlayer)
    local unit = tonumber(unitId)
    local localObjectId = tonumber(localPlayer and localPlayer.objectId)
    if not unit or not localObjectId then return false end
    if unit == 0xFFFFFFFF or localObjectId == 0xFFFFFFFF then return false end
    return unit == localObjectId
end

local function hasIntegerFlag(value, flag)
    value = math.floor(tonumber(value) or 0)
    flag = math.floor(tonumber(flag) or 1)
    return math.floor(value / flag) % 2 == 1
end

local DAMAGE_EFFECT_FLAGS_OFFSET = 0x1C8
local DAMAGE_EFFECT_HEADSHOT_FLAG = 0x00000002
local DAMAGE_EFFECT_MP_HEADSHOT_FLAG = 0x00000800
local hitDamageEffectHeadshotCache = {}

local function damageEffectAllowsHeadshot(effectTagId)
    effectTagId = tonumber(effectTagId)
    if not effectTagId or effectTagId == 0xFFFFFFFF then return false end

    local cached = hitDamageEffectHeadshotCache[effectTagId]
    if cached ~= nil then return cached end

    local tag = safeGetTag(effectTagId)
    if not tag or not tag.data then
        hitDamageEffectHeadshotCache[effectTagId] = false
        return false
    end

    local ok, flags = pcall(read_dword, tag.data + DAMAGE_EFFECT_FLAGS_OFFSET)
    if not ok or not flags then
        hitDamageEffectHeadshotCache[effectTagId] = false
        return false
    end

    local result =
        hasIntegerFlag(flags, DAMAGE_EFFECT_HEADSHOT_FLAG) or
        hasIntegerFlag(flags, DAMAGE_EFFECT_MP_HEADSHOT_FLAG)

    hitDamageEffectHeadshotCache[effectTagId] = result
    return result
end

local BIPED_MODEL_DEPENDENCY_TAG_ID_OFFSET = 0x34
local MODEL_NODE_COUNT_OFFSET = 0xB8
local MODEL_NODE_LIST_OFFSET = 0xBC
local MODEL_NODE_STRIDE = 0x9C
local MODEL_REGION_COUNT_OFFSET = 0xC4
local MODEL_REGION_LIST_OFFSET = 0xC8
local MODEL_REGION_STRIDE = 0x4C

local function safeReadString(address)
    address = tonumber(address)
    if not address or address <= 0 then return nil end
    local ok, value = pcall(read_string, address)
    if ok and type(value) == "string" and value ~= "" then
        return value
    end
    return nil
end

local function getHitModelInfo(bipedTagId)
    bipedTagId = tonumber(bipedTagId)
    if not bipedTagId or bipedTagId == 0xFFFFFFFF then return nil end

    local cached = hitModelNameCache[bipedTagId]
    if cached ~= nil then return cached or nil end

    local bipedTag = safeGetTag(bipedTagId)
    if not bipedTag or not bipedTag.data then
        hitModelNameCache[bipedTagId] = false
        return nil
    end

    local okModelId, modelId = pcall(
        read_dword,
        bipedTag.data + BIPED_MODEL_DEPENDENCY_TAG_ID_OFFSET
    )
    if not okModelId or not modelId or blam.isNull(modelId) then
        hitModelNameCache[bipedTagId] = false
        return nil
    end

    local modelTag = safeGetTag(modelId)
    if not modelTag or not modelTag.data then
        hitModelNameCache[bipedTagId] = false
        return nil
    end

    local okNC, nodeCount = pcall(read_dword, modelTag.data + MODEL_NODE_COUNT_OFFSET)
    local okNP, nodeList = pcall(read_dword, modelTag.data + MODEL_NODE_LIST_OFFSET)
    local okRC, regionCount = pcall(read_dword, modelTag.data + MODEL_REGION_COUNT_OFFSET)
    local okRP, regionList = pcall(read_dword, modelTag.data + MODEL_REGION_LIST_OFFSET)

    if not okNC or not okNP or not okRC or not okRP then
        hitModelNameCache[bipedTagId] = false
        return nil
    end

    local info = {
        modelTagId = modelId,
        nodeCount = math.max(0, math.min(256, tonumber(nodeCount) or 0)),
        nodeList = tonumber(nodeList) or 0,
        regionCount = math.max(0, math.min(64, tonumber(regionCount) or 0)),
        regionList = tonumber(regionList) or 0
    }

    hitModelNameCache[bipedTagId] = info
    return info
end

local function resolveHitNodeRegionNames(bipedTagId, nodeIndex, regionIndex)
    local info = getHitModelInfo(bipedTagId)
    if not info then return nil, nil end

    nodeIndex = tonumber(nodeIndex) or -1
    regionIndex = tonumber(regionIndex) or -1
    local nodeName = nil
    local regionName = nil

    if nodeIndex >= 0 and nodeIndex < info.nodeCount and info.nodeList > 0 then
        nodeName = safeReadString(info.nodeList + nodeIndex * MODEL_NODE_STRIDE)
    end

    if regionIndex >= 0 and regionIndex < info.regionCount and info.regionList > 0 then
        regionName = safeReadString(info.regionList + regionIndex * MODEL_REGION_STRIDE)
    end

    return nodeName, regionName
end

local function isHeadshotNodeName(name)
    name = tostring(name or ""):lower()
    if name == "" then return false end

    local patterns = configuration.headshotNodePatterns
    if type(patterns) ~= "table" then patterns = {"head", "helmet"} end

    for _, pattern in ipairs(patterns) do
        pattern = tostring(pattern or ""):lower()
        if pattern ~= "" and name:find(pattern, 1, true) then
            return true
        end
    end
    return false
end

local function refreshNativeDamageProbeState(force)
    if force then nativeDamageTracker.available = nil end
    if nativeDamageTracker.available ~= nil then
        return nativeDamageTracker.available
    end

    if not configuration.nativeDamageTelemetry or
       type(optic.native_damage_probe_available) ~= "function" then
        nativeDamageTracker.available = false
        return false
    end

    local ok, available = pcall(optic.native_damage_probe_available)
    nativeDamageTracker.available = ok and available == true

    return nativeDamageTracker.available
end

local function findTrackedSnapshotByObjectId(objectId)
    for playerIndex, snapshot in pairs(damageTracker.players) do
        if snapshot and sameHaloDatumId(snapshot.objectId, objectId) then
            return playerIndex, snapshot
        end
    end
    return nil, nil
end

local function findTrackedSnapshotByVehicleObjectId(objectId)
    for playerIndex, snapshot in pairs(damageTracker.players) do
        if snapshot and snapshot.vehicleObjectId and
           sameHaloDatumId(snapshot.vehicleObjectId, objectId) then
            return playerIndex, snapshot
        end
    end
    return nil, nil
end

local function resolveNativeImpactTargetTagId(event)
    local _, snapshot = findTrackedSnapshotByObjectId(event.targetObjectId)
    if snapshot and snapshot.tagId then return snapshot.tagId end

    local okAddress, address = pcall(get_object, event.targetObjectId)
    if okAddress and address then
        local okBiped, biped = pcall(blam.biped, address)
        if okBiped and biped then return tonumber(biped.tagId) end
    end
    return nil
end

local function bindNativeImpactToPending(event, snapshot, pending, now)
    if not event or not snapshot or not pending then return false end

    event.soundPaired = true
    event.soundTimeMs = tonumber(pending.createdMs) or now
    pending.targetObjectId = tonumber(snapshot.objectId)
    pending.nativeSequence = tonumber(event.sequence)
    pending.nativeImpact = event

    if pending.damageEvent and
       not sameHaloDatumId(pending.damageEvent.objectId, pending.targetObjectId) then
        pending.damageEvent.hitMatched = false
        damageTracker.unpaired[#damageTracker.unpaired + 1] = pending.damageEvent
        pending.damageEvent = nil
        pending.damage = nil
        pending.critical = false
        pending.followupCritical = false
        pending.hitmarkerKind = "normal"
    end

    if pending.damageEvent then
        return true
    end

    local shieldBefore = math.max(0.0, tonumber(snapshot.shield) or 0.0)
    local inVehicle = snapshot.vehicleObjectId and
                      not blam.isNull(snapshot.vehicleObjectId)

    if shieldBefore > HEADSHOT_SHIELD_EPSILON and
       configuration.shieldHitmarker then
        pending.hitmarkerKind = "shield"
        pending.provisionalShield = true
        pending.dueMs = math.max(
            pending.dueMs,
            (tonumber(pending.createdMs) or now) + HITMARKER_SHIELD_CONFIRM_DELAY_MS
        )
    elseif inVehicle and configuration.vehicleHitmarker then
        pending.hitmarkerKind = "vehicle"
    end

    return true
end

local function tryBindNativeImpactToRecentPending(
    event,
    localPlayer,
    now,
    teamGameLikely,
    localTeam
)
    if not event or event.soundPaired then return false end

    local playerIndex, snapshot = findTrackedSnapshotByObjectId(event.targetObjectId)
    if not snapshot then
        playerIndex, snapshot = findTrackedSnapshotByVehicleObjectId(event.targetObjectId)
    end
    if not snapshot then return false end

    local localIndex = tonumber(localPlayer and localPlayer.index)
    if playerIndex == localIndex then return false end

    if isFriendlySnapshot(snapshot, localTeam, teamGameLikely) then
        return false
    end

    local bestPending = nil
    local bestAge = nil
    for index = #pendingNormalHitmarkers, 1, -1 do
        local pending = pendingNormalHitmarkers[index]
        local age = now - (tonumber(pending.createdMs) or now)
        local targetCompatible =
            not pending.targetObjectId or
            sameHaloDatumId(pending.targetObjectId, snapshot.objectId)

        if age >= 0 and age <= NATIVE_SOUND_MATCH_WINDOW_MS and
           targetCompatible and
           (not bestAge or age < bestAge) then
            bestPending = pending
            bestAge = age
        end
    end

    if bestPending then
        return bindNativeImpactToPending(event, snapshot, bestPending, now)
    end

    return false
end

local function drainNativeDamageEvents(
    localPlayer,
    now,
    currentWeaponPath,
    teamGameLikely,
    localTeam
)
    if not refreshNativeDamageProbeState(false) or
       type(optic.poll_native_damage_event) ~= "function" then
        return false
    end

    local capturedWeaponPath = damageTracker.lastHitWeaponPath or
                               currentWeaponPath or ""

    for _ = 1, 64 do
        local ok, event = pcall(optic.poll_native_damage_event)
        if not ok or not event then break end

        local attributedToLocalPlayer =
            isLocalDamager(event.responsiblePlayerId, localPlayer) or
            isLocalResponsibleUnit(event.responsibleUnitId, localPlayer)

        if attributedToLocalPlayer then
            event.timeMs = now
            event.weaponPath = capturedWeaponPath
            event.targetTagId = resolveNativeImpactTargetTagId(event)
            event.nodeName, event.regionName = resolveHitNodeRegionNames(
                event.targetTagId,
                event.nodeIndex,
                event.regionIndex
            )
            event.exactHeadHit =
                isHeadshotNodeName(event.nodeName) or
                isHeadshotNodeName(event.regionName)
            event.damageEffectHeadshot =
                damageEffectAllowsHeadshot(event.effectTagId)

            event.headshotCapable = event.damageEffectHeadshot == true
            event.hitPaired = false
            event.soundPaired = false
            event.soundTimeMs = nil
            event.headshotConsumed = false

            nativeDamageTracker.recent[#nativeDamageTracker.recent + 1] = event
            tryBindNativeImpactToRecentPending(
                event,
                localPlayer,
                now,
                teamGameLikely,
                localTeam
            )

        end
    end

    local recentWriteIndex = 1
    local recentCount = #nativeDamageTracker.recent
    for recentReadIndex = 1, recentCount do
        local event = nativeDamageTracker.recent[recentReadIndex]
        if (now - (tonumber(event.timeMs) or now)) <= NATIVE_DAMAGE_HISTORY_MS then
            if recentWriteIndex ~= recentReadIndex then
                nativeDamageTracker.recent[recentWriteIndex] = event
            end
            recentWriteIndex = recentWriteIndex + 1
        end
    end
    for index = recentCount, recentWriteIndex, -1 do
        nativeDamageTracker.recent[index] = nil
    end

    return true
end

local function consumeNativeImpactForObject(objectId, now)
    for index = #nativeDamageTracker.recent, 1, -1 do
        local event = nativeDamageTracker.recent[index]
        local age = now - (tonumber(event.timeMs) or now)
        if age >= 0 and age <= NATIVE_DAMAGE_MATCH_WINDOW_MS and
           not event.hitPaired and
           sameHaloDatumId(event.targetObjectId, objectId) then
            event.hitPaired = true
            return event
        end
    end
    return nil
end

local function claimNativeImpactForHitSound(
    localPlayer,
    now,
    teamGameLikely,
    localTeam
)
    if not refreshNativeDamageProbeState(false) then return nil, nil end

    local localIndex = tonumber(localPlayer and localPlayer.index)

    for index = #nativeDamageTracker.recent, 1, -1 do
        local event = nativeDamageTracker.recent[index]
        local age = now - (tonumber(event.timeMs) or now)

        if age >= 0 and age <= NATIVE_SOUND_MATCH_WINDOW_MS and
           not event.soundPaired then
            local playerIndex, snapshot =
                findTrackedSnapshotByObjectId(event.targetObjectId)
            if not snapshot then
                playerIndex, snapshot =
                    findTrackedSnapshotByVehicleObjectId(event.targetObjectId)
            end

            if snapshot and playerIndex ~= localIndex and
               not isFriendlySnapshot(snapshot, localTeam, teamGameLikely) then
                event.soundPaired = true
                event.soundTimeMs = now
                return event, snapshot
            end
        end
    end

    return nil, nil
end

local function applyProvisionalNativeStatusToPending(pending)
    if not pending then return end

    local okLocalAddress, localAddress = pcall(get_player)
    if not okLocalAddress or not localAddress then return end
    local okLocal, localPlayer = pcall(blam.player, localAddress)
    if not okLocal or not localPlayer then return end

    local now = getHitmarkerNowMs()
    local weaponTag = getCurrentWeaponTag()
    local currentWeaponPath = weaponTag and weaponTag.path or ""
    local teamGameLikely, localTeam =
        detectTeamCombatContext(localPlayer)

    drainNativeDamageEvents(
        localPlayer,
        now,
        currentWeaponPath,
        teamGameLikely,
        localTeam
    )

    local nativeImpact, snapshot =
        claimNativeImpactForHitSound(
            localPlayer,
            now,
            teamGameLikely,
            localTeam
        )
    if not nativeImpact or not snapshot then return end

    bindNativeImpactToPending(nativeImpact, snapshot, pending, now)
end

local function getDamageCriticalThreshold()
    return math.max(
        1,
        tonumber(configuration.criticalDamageThreshold) or 48
    )
end

local function getHeadshotMinDamage()
    return math.max(
        1,
        tonumber(configuration.headshotMinDamage) or 30
    )
end

local function associateDamageWithHitmarker(event)
    local bestIndex = nil
    local bestAge = nil
    local bestPriority = -1

    for index = #pendingNormalHitmarkers, 1, -1 do
        local pending = pendingNormalHitmarkers[index]
        local age = math.abs(event.timeMs - pending.createdMs)
        local targetMatch =
            pending.targetObjectId and
            sameHaloDatumId(pending.targetObjectId, event.objectId)
        local sequenceMatch =
            pending.nativeSequence and event.nativeImpact and
            tonumber(pending.nativeSequence) == tonumber(event.nativeImpact.sequence)

        local eligible =
            not pending.targetObjectId or targetMatch or sequenceMatch
        local priority = sequenceMatch and 3 or (targetMatch and 2 or 1)

        if eligible and age <= DAMAGE_HIT_MATCH_WINDOW_MS and
           (priority > bestPriority or
            (priority == bestPriority and (not bestAge or age < bestAge))) then
            bestIndex = index
            bestAge = age
            bestPriority = priority
        end
    end

    if bestIndex then
        local pending = pendingNormalHitmarkers[bestIndex]
        pending.damage = event.damage
        pending.critical = event.critical
        pending.followupCritical = event.followupCritical == true
        pending.hitmarkerKind = event.hitmarkerKind or
            (event.critical and "critical" or "normal")
        pending.damageEvent = event
        pending.targetObjectId = pending.targetObjectId or event.objectId
        if event.nativeImpact and event.nativeImpact.sequence then
            pending.nativeSequence = pending.nativeSequence or event.nativeImpact.sequence
        end
        event.hitMatched = true

        return true
    end

    damageTracker.unpaired[#damageTracker.unpaired + 1] = event
    return false
end

local function queueDamageNumber(amount, critical)
    if not configuration.damageNumbers then return end

    local now = getHitmarkerNowMs()
    local display = damageTracker.display

    if display.dueMs and display.startedMs and
       (now - display.startedMs) <= DAMAGE_NUMBER_GROUP_WINDOW_MS then
        display.amount = display.amount + amount
        display.critical = display.critical or critical
        display.dueMs = now + DAMAGE_NUMBER_GROUP_DELAY_MS
    else
        display.amount = amount
        display.critical = critical
        display.startedMs = now
        display.dueMs = now + DAMAGE_NUMBER_GROUP_DELAY_MS
    end
end

local function getDamageNumberSprite(amount, critical)
    if type(optic.create_damage_number_sprite) ~= "function" then
        return nil, nil, nil
    end

    amount = math.max(0, math.min(9999, math.floor((tonumber(amount) or 0) + 0.5)))
    local rs = getHitmarkerResolutionScale()
    local color = damageOverrideColor(critical == true)
    local key = tostring(amount) .. "|" .. tostring(critical and 1 or 0) ..
                "|" .. string.format("%.2f", rs) ..
                "|rgb=" .. color.r .. "," .. color.g .. "," .. color.b

    local cached = damageNumberSpriteCache[key]
    if cached then
        return cached.handle, cached.width, cached.height
    end

    local ok, handle, width, height = pcall(
        optic.create_damage_number_sprite,
        amount,
        critical == true,
        rs,
        color.r,
        color.g,
        color.b
    )

    if not ok then
        ok, handle, width, height = pcall(
            optic.create_damage_number_sprite,
            amount,
            critical == true,
            rs
        )
    end

    if not ok or not handle or not width or not height then
        return nil, nil, nil
    end

    cached = {
        handle = handle,
        width = tonumber(width),
        height = tonumber(height)
    }
    damageNumberSpriteCache[key] = cached
    return cached.handle, cached.width, cached.height
end

local function renderDamageNumber(amount, critical)

    local numericAmount = tonumber(amount) or 0.0
    critical = (critical == true) and
               numericAmount >= getDamageCriticalThreshold()

    local handle, width, height = getDamageNumberSprite(numericAmount, critical)
    if not handle or not width or not height then return false end

    if #damageNumberRenderQueues > 0 then
        local queue = damageNumberRenderQueues[damageNumberNextQueue]
        damageNumberNextQueue = damageNumberNextQueue + 1
        if damageNumberNextQueue > #damageNumberRenderQueues then
            damageNumberNextQueue = 1
        end

        if queue ~= nil then
            if type(optic.clear_render_queue) == "function" then
                pcall(optic.clear_render_queue, queue)
            end

            local ok = pcall(optic.render_sprite, handle, queue)
            if ok then return true end
        end
    end

    local rs = getHitmarkerResolutionScale()
    local x = (screenWidth - width) * 0.5
    local y = (screenHeight * 0.5) -
              (DAMAGE_NUMBER_Y_OFFSET_1080 * rs) -
              (height * 0.5)

    if damageNumberFadeAnimation then
        optic.render_sprite(
            handle,
            x,
            y,
            255,
            0,
            DAMAGE_NUMBER_DURATION_MS,
            hitmarkerEnterAnimation,
            damageNumberFadeAnimation
        )
    else
        optic.render_sprite(
            handle,
            x,
            y,
            255,
            0,
            DAMAGE_NUMBER_DURATION_MS
        )
    end

    return true
end

local function processDamageNumberDisplay(now)
    local display = damageTracker.display
    if not display.dueMs or now < display.dueMs then return end

    local amount = math.max(1, math.floor(display.amount + 0.5))
    local critical = display.critical

    display.amount = 0
    display.critical = false
    display.dueMs = nil
    display.startedMs = nil

    renderDamageNumber(amount, critical)
end

local function resetDamageTracker()
    damageTracker.players = {}
    damageTracker.nextPlayers = {}
    damageTracker.scanPlayers = {}
    damageTracker.candidates = {}
    damageTracker.unpaired = {}
    damageTracker.recentLocalDamage = {}
    damageTracker.lastLocalDamage = nil
    damageTracker.lastHitSoundMs = nil
    damageTracker.lastHitWeaponPath = nil
    damageTracker.display = {
        amount = 0,
        critical = false,
        dueMs = nil,
        startedMs = nil
    }

    nativeDamageTracker.recent = {}
    if type(optic.clear_native_damage_events) == "function" then
        pcall(optic.clear_native_damage_events)
    end
end

local function pollDamageTracker()
    local okLocalAddress, localAddress = pcall(get_player)
    if not okLocalAddress or not localAddress then return end

    local okLocal, localPlayer = pcall(blam.player, localAddress)
    if not okLocal or not localPlayer then return end

    local localIndex = tonumber(localPlayer.index)
    local now = getHitmarkerNowMs()
    local weaponTag = getCurrentWeaponTag()
    local currentWeaponPath = weaponTag and weaponTag.path or ""
    local candidates = damageTracker.candidates
    local candidateCount = 0
    local scannedPlayers = damageTracker.scanPlayers
    local localTeam = canonicalPlayerTeam(localPlayer)
    local teamGameLikely = false

    for playerIndex = 0, 15 do
        local player = safePlayer(playerIndex)
        scannedPlayers[playerIndex] = player or false

        if playerIndex ~= localIndex and player and localTeam ~= nil then
            local team = canonicalPlayerTeam(player)
            if team ~= nil and team ~= localTeam then
                teamGameLikely = true
            end
        end
    end

    local nativeAvailable =
        drainNativeDamageEvents(
            localPlayer,
            now,
            currentWeaponPath,
            teamGameLikely,
            localTeam
        )

    local previousPlayers = damageTracker.players
    local nextPlayers = damageTracker.nextPlayers

    for playerIndex = 0, 15 do
        local player = scannedPlayers[playerIndex]
        if player == false then player = nil end
        local biped = safeBiped(playerIndex)

        if player and biped then
            local playerId = tonumber(player.id)
            local objectId = tonumber(player.objectId)
            local tagId = tonumber(biped.tagId)
            local team = tonumber(player.team)
            local health = math.max(0.0, tonumber(biped.health) or 0.0)
            local shield = math.max(0.0, tonumber(biped.shield) or 0.0)
            local maximumBodyVitality =
                math.max(0.0, tonumber(biped.maximumBodyVitality) or 0.0)
            local maximumShieldVitality =
                math.max(0.0, tonumber(biped.maximumShieldVitality) or 0.0)
            local vehicleObjectId = tonumber(biped.vehicleObjectId)
            local previous = previousPlayers[playerIndex]

            if previous and previous.objectId == objectId and
               playerIndex ~= localIndex then
                local shieldLoss = math.max(0.0, previous.shield - shield)
                local healthLoss = math.max(0.0, previous.health - health)
                local totalLoss = shieldLoss + healthLoss
                local sameTeam =
                    teamGameLikely and
                    localTeam ~= nil and
                    team == localTeam

                if totalLoss > DAMAGE_TRACK_EPSILON and not sameTeam then
                    candidateCount = candidateCount + 1
                    local candidate = candidates[candidateCount]
                    if not candidate then
                        candidate = {}
                        candidates[candidateCount] = candidate
                    end

                    candidate.playerIndex = playerIndex
                    candidate.playerId = playerId
                    candidate.objectId = objectId
                    candidate.tagId = tagId
                    candidate.team = team
                    candidate.biped = biped
                    candidate.vehicleObjectId = vehicleObjectId
                    candidate.health = health
                    candidate.shield = shield
                    candidate.maximumBodyVitality = maximumBodyVitality
                    candidate.maximumShieldVitality = maximumShieldVitality
                    candidate.previous = previous
                    candidate.totalLoss = totalLoss
                    candidate.exactLocalDamager =
                        isLocalDamager(
                            biped.mostRecentDamagerPlayer,
                            localPlayer
                        )
                end
            end

            local snapshot = nextPlayers[playerIndex]
            if not snapshot then
                snapshot = {}
                nextPlayers[playerIndex] = snapshot
            end

            snapshot.playerId = playerId
            snapshot.objectId = objectId
            snapshot.tagId = tagId
            snapshot.team = team
            snapshot.health = health
            snapshot.shield = shield
            snapshot.maximumBodyVitality = maximumBodyVitality
            snapshot.maximumShieldVitality = maximumShieldVitality
            snapshot.vehicleObjectId = vehicleObjectId
            snapshot.lastSeenMs = now
        else
            local previous = previousPlayers[playerIndex]
            if previous and
               (now - (tonumber(previous.lastSeenMs) or now)) <= VICTIM_SNAPSHOT_GRACE_MS then
                local snapshot = nextPlayers[playerIndex]
                if not snapshot then
                    snapshot = {}
                    nextPlayers[playerIndex] = snapshot
                end

                snapshot.playerId = previous.playerId
                snapshot.objectId = previous.objectId
                snapshot.tagId = previous.tagId
                snapshot.team = previous.team
                snapshot.health = previous.health
                snapshot.shield = previous.shield
                snapshot.maximumBodyVitality = previous.maximumBodyVitality
                snapshot.maximumShieldVitality = previous.maximumShieldVitality
                snapshot.vehicleObjectId = previous.vehicleObjectId
                snapshot.lastSeenMs = previous.lastSeenMs
            else
                nextPlayers[playerIndex] = nil
            end
        end

        scannedPlayers[playerIndex] = nil
    end

    damageTracker.players = nextPlayers
    damageTracker.nextPlayers = previousPlayers

    local recentTing =
        damageTracker.lastHitSoundMs and
        math.abs(now - damageTracker.lastHitSoundMs) <= DAMAGE_HIT_MATCH_WINDOW_MS

    for candidateIndex = 1, candidateCount do
        local candidate = candidates[candidateIndex]
        local nativeImpact =
            consumeNativeImpactForObject(candidate.objectId, now)

        if not nativeImpact and candidate.vehicleObjectId and
           not blam.isNull(candidate.vehicleObjectId) then
            nativeImpact = consumeNativeImpactForObject(
                candidate.vehicleObjectId,
                now
            )
        end

        local localDamage
        if nativeAvailable then
            localDamage = nativeImpact ~= nil
        else

            localDamage =
                recentTing and
                (candidate.exactLocalDamager or candidateCount == 1)
        end

        if localDamage then
            local previous = candidate.previous
            local health = candidate.health
            local shield = candidate.shield

            local bodyMax = math.max(
                tonumber(previous.maximumBodyVitality) or 0.0,
                tonumber(candidate.maximumBodyVitality) or 0.0
            )
            local shieldMax = math.max(
                tonumber(previous.maximumShieldVitality) or 0.0,
                tonumber(candidate.maximumShieldVitality) or 0.0
            )

            local validBodyMax = bodyMax > DAMAGE_TRACK_EPSILON
            local validShieldMax = shieldMax > DAMAGE_TRACK_EPSILON

            local shieldBeforeVitality = validShieldMax and
                (math.max(0.0, previous.shield) * shieldMax) or 0.0
            local shieldAfterVitality = validShieldMax and
                (math.max(0.0, shield) * shieldMax) or 0.0
            local healthBeforeVitality = validBodyMax and
                (math.max(0.0, previous.health) * bodyMax) or 0.0
            local healthAfterVitality = validBodyMax and
                (math.max(0.0, health) * bodyMax) or 0.0

            local shieldDamageExact = math.max(
                0.0,
                shieldBeforeVitality - shieldAfterVitality
            )
            local healthDamageExact = math.max(
                0.0,
                healthBeforeVitality - healthAfterVitality
            )
            local damageExact = shieldDamageExact + healthDamageExact
            local hasRealVitality = validBodyMax or validShieldMax
            local damage = hasRealVitality and
                math.max(1, math.floor(damageExact + 0.5)) or nil

            local shieldDamage = math.floor(shieldDamageExact + 0.5)
            local healthDamage = math.floor(healthDamageExact + 0.5)

            local wasUnshielded =
                previous.shield <= HEADSHOT_SHIELD_EPSILON
            local lethal = health <= DAMAGE_TRACK_EPSILON
            local shieldHit = shieldDamageExact > DAMAGE_TRACK_EPSILON
            local shieldBroken =
                shieldHit and
                previous.shield > DAMAGE_TRACK_EPSILON and
                shield <= HEADSHOT_SHIELD_EPSILON

            local recentWeaponPath =
                (nativeImpact and nativeImpact.weaponPath) or
                (recentTing and damageTracker.lastHitWeaponPath) or
                currentWeaponPath
            local weaponPath = recentWeaponPath or ""

            local exactHeadHit =
                nativeImpact and nativeImpact.exactHeadHit == true
            local exactHeadshotCapable =
                nativeImpact and nativeImpact.damageEffectHeadshot == true

            local headshotCandidate
            if nativeAvailable then
                headshotCandidate =
                    exactHeadHit and
                    exactHeadshotCapable and
                    lethal
            else
                headshotCandidate =
                    damage ~= nil and
                    damage >= getHeadshotMinDamage() and
                    lethal and
                    wasUnshielded and
                    isHeadshotCapableWeapon(weaponPath)
            end

            local critical =
                hasRealVitality and
                damageExact >= getDamageCriticalThreshold()

            local inVehicle =
                candidate.vehicleObjectId and
                not blam.isNull(candidate.vehicleObjectId)

            local hitmarkerKind = "normal"
            local followupCritical = false
            if shieldBroken and configuration.shieldBreakHitmarker then
                hitmarkerKind = "shield_broken"
                followupCritical = critical and configuration.criticalHitmarker
            elseif inVehicle and configuration.vehicleHitmarker then
                hitmarkerKind = "vehicle"
            elseif shieldHit and configuration.shieldHitmarker then
                hitmarkerKind = "shield"
            elseif critical and configuration.criticalHitmarker then
                hitmarkerKind = "critical"
            end

            local exactAttribution =
                nativeImpact ~= nil

            local event = {
                timeMs = now,
                playerIndex = candidate.playerIndex,
                playerId = candidate.playerId,
                objectId = candidate.objectId,
                damage = damage,
                critical = critical,
                followupCritical = followupCritical,
                hitmarkerKind = hitmarkerKind,
                lethal = lethal,
                headshotCandidate = headshotCandidate,
                hitMatched = false,
                headshotConsumed = false,
                damageNumberQueued = false,
                shieldBefore = previous.shield,
                shieldAfter = shield,
                healthBefore = previous.health,
                healthAfter = health,
                shieldDamage = shieldDamage,
                healthDamage = healthDamage,
                shieldDamageExact = shieldDamageExact,
                healthDamageExact = healthDamageExact,
                damageExact = damageExact,
                shieldVitalityBefore = shieldBeforeVitality,
                shieldVitalityAfter = shieldAfterVitality,
                healthVitalityBefore = healthBeforeVitality,
                healthVitalityAfter = healthAfterVitality,
                maximumBodyVitality = bodyMax,
                maximumShieldVitality = shieldMax,
                realVitalityAvailable = hasRealVitality,
                shieldHit = shieldHit,
                shieldBroken = shieldBroken,
                inVehicle = inVehicle,
                weaponPath = weaponPath,
                nativeImpact = nativeImpact,
                exactHeadHit = exactHeadHit,
                nodeName = nativeImpact and nativeImpact.nodeName or nil,
                regionName = nativeImpact and nativeImpact.regionName or nil,
                exactAttribution = exactAttribution
            }

            damageTracker.lastLocalDamage = event
            damageTracker.recentLocalDamage[#damageTracker.recentLocalDamage + 1] = event
            associateDamageWithHitmarker(event)
            if exactAttribution and
               hasRealVitality and damageExact > DAMAGE_TRACK_EPSILON then
                if lethal then

                    event.damageNumberQueued = false
                else
                    queueDamageNumber(damageExact, critical)
                    event.damageNumberQueued = true
                end
            end

        end

        candidate.biped = nil
        candidate.previous = nil
    end

    local writeIndex = 1
    local unpairedCount = #damageTracker.unpaired
    for readIndex = 1, unpairedCount do
        local entry = damageTracker.unpaired[readIndex]
        if (now - entry.timeMs) <= DAMAGE_UNPAIRED_LIFETIME_MS then
            if writeIndex ~= readIndex then
                damageTracker.unpaired[writeIndex] = entry
            end
            writeIndex = writeIndex + 1
        end
    end
    for index = unpairedCount, writeIndex, -1 do
        damageTracker.unpaired[index] = nil
    end

    local recentDamageLifetime = math.max(
        HEADSHOT_CONFIRM_WINDOW_MS,
        DAMAGE_KILL_MATCH_WINDOW_MS
    )
    writeIndex = 1
    local recentDamageCount = #damageTracker.recentLocalDamage
    for readIndex = 1, recentDamageCount do
        local entry = damageTracker.recentLocalDamage[readIndex]
        if (now - entry.timeMs) <= recentDamageLifetime then
            if writeIndex ~= readIndex then
                damageTracker.recentLocalDamage[writeIndex] = entry
            end
            writeIndex = writeIndex + 1
        end
    end
    for index = recentDamageCount, writeIndex, -1 do
        damageTracker.recentLocalDamage[index] = nil
    end
end

local function hitVictimIdentifierMatches(event, victimId)
    local value = tonumber(victimId)
    if not value or not event then return false end

    local lowWord = value % 0x10000
    local playerIndex = tonumber(event.playerIndex)
    if playerIndex and
       (value == playerIndex or lowWord == (playerIndex % 0x10000)) then
        return true
    end

    local playerId = tonumber(event.playerId)
    if playerId and
       (value == playerId or lowWord == (playerId % 0x10000)) then
        return true
    end

    local objectId = tonumber(event.objectId)
    return objectId ~= nil and
           (value == objectId or lowWord == (objectId % 0x10000))
end

local function consumeRecentHeadshotEvent(victimId, strictVictim)
    local now = getHitmarkerNowMs()
    local fallback = nil
    local fallbackCount = 0

    for index = #damageTracker.recentLocalDamage, 1, -1 do
        local event = damageTracker.recentLocalDamage[index]
        local age = now - event.timeMs

        if age >= 0 and age <= HEADSHOT_CONFIRM_WINDOW_MS and
           not event.headshotConsumed and event.headshotCandidate then
            if hitVictimIdentifierMatches(event, victimId) then
                event.headshotConsumed = true
                return event
            end

            fallback = event
            fallbackCount = fallbackCount + 1
        end
    end

    if not strictVictim and fallbackCount == 1 then
        fallback.headshotConsumed = true
        return fallback
    end

    return nil
end

local function findTrackedVictimSnapshot(victimId)
    local value = tonumber(victimId)
    if not value then return nil, nil end

    local lowWord = value % 0x10000

    for playerIndex, snapshot in pairs(damageTracker.players) do
        local indexValue = tonumber(playerIndex)
        if indexValue and
           (value == indexValue or lowWord == (indexValue % 0x10000)) then
            return playerIndex, snapshot
        end

        local playerId = tonumber(snapshot.playerId)
        if playerId and
           (value == playerId or lowWord == (playerId % 0x10000)) then
            return playerIndex, snapshot
        end

        local objectId = tonumber(snapshot.objectId)
        if objectId and
           (value == objectId or lowWord == (objectId % 0x10000)) then
            return playerIndex, snapshot
        end
    end

    return nil, nil
end

local function findRecentKillDamageEvent(victimId)
    local now = getHitmarkerNowMs()
    local fallback = nil
    local fallbackCount = 0

    for index = #damageTracker.recentLocalDamage, 1, -1 do
        local event = damageTracker.recentLocalDamage[index]
        local age = now - (tonumber(event.timeMs) or now)

        if age >= 0 and age <= DAMAGE_KILL_MATCH_WINDOW_MS and
           event.lethal == true and
           event.realVitalityAvailable == true and
           (tonumber(event.damageExact) or 0.0) > DAMAGE_TRACK_EPSILON then
            if hitVictimIdentifierMatches(event, victimId) then
                return event
            end

            fallback = event
            fallbackCount = fallbackCount + 1
        end
    end

    if fallbackCount == 1 then
        return fallback
    end

    return nil
end

local function hasRecentNativeKillImpact(victimId, snapshot)
    if not refreshNativeDamageProbeState(false) then
        return false
    end

    local now = getHitmarkerNowMs()
    local objectId = snapshot and snapshot.objectId or nil
    local vehicleObjectId = snapshot and snapshot.vehicleObjectId or nil

    for index = #nativeDamageTracker.recent, 1, -1 do
        local event = nativeDamageTracker.recent[index]
        local age = now - (tonumber(event.timeMs) or now)

        if age >= 0 and age <= DAMAGE_KILL_MATCH_WINDOW_MS then
            local target = event.targetObjectId
            if (objectId and sameHaloDatumId(target, objectId)) or
               (vehicleObjectId and sameHaloDatumId(target, vehicleObjectId)) or
               sameHaloDatumId(target, victimId) then
                return true
            end
        end
    end

    return false
end

local function buildKillDamageFromLastSnapshot(victimId)
    local now = getHitmarkerNowMs()
    local _, snapshot = findTrackedVictimSnapshot(victimId)
    if not snapshot then return nil end

    local age = now - (tonumber(snapshot.lastSeenMs) or now)
    if age < 0 or age > DAMAGE_KILL_MATCH_WINDOW_MS then
        return nil
    end

    if not hasRecentNativeKillImpact(victimId, snapshot) then
        return nil
    end

    local bodyMax = tonumber(snapshot.maximumBodyVitality) or 0.0
    local shieldMax = tonumber(snapshot.maximumShieldVitality) or 0.0
    local healthRatio = math.max(0.0, tonumber(snapshot.health) or 0.0)
    local shieldRatio = math.max(0.0, tonumber(snapshot.shield) or 0.0)

    local hasBody = bodyMax > DAMAGE_TRACK_EPSILON
    local hasShield = shieldMax > DAMAGE_TRACK_EPSILON
    if not hasBody and not hasShield then
        return nil
    end

    local healthRemaining = hasBody and (healthRatio * bodyMax) or 0.0
    local shieldRemaining = hasShield and (shieldRatio * shieldMax) or 0.0
    local damageExact = healthRemaining + shieldRemaining

    if damageExact <= DAMAGE_TRACK_EPSILON then
        return nil
    end

    return {
        damageExact = damageExact,
        critical = damageExact >= getDamageCriticalThreshold(),
        inferredFromPreKillSnapshot = true,
        objectId = snapshot.objectId,
        playerId = snapshot.playerId,
        maximumBodyVitality = bodyMax,
        maximumShieldVitality = shieldMax,
        healthVitalityBefore = healthRemaining,
        shieldVitalityBefore = shieldRemaining
    }
end

local function renderKillDamageForVictim(victimId)
    if not configuration.damageNumbers then return nil end

    local event = findRecentKillDamageEvent(victimId)
    if event and event.exactAttribution == true then
        local amount = tonumber(event.damageExact) or 0.0
        if amount > DAMAGE_TRACK_EPSILON then
            local rendered = renderDamageNumber(amount, event.critical == true)
            event.damageNumberQueued = rendered == true
            event.killDamageRendered = rendered == true
            return event
        end
    end

    local fallback = buildKillDamageFromLastSnapshot(victimId)
    if fallback then
        renderDamageNumber(fallback.damageExact, fallback.critical == true)
        return fallback
    end

    return nil
end

local function consumeNativeHeadshotForKill(victimId)
    if not refreshNativeDamageProbeState(false) then return nil end

    local now = getHitmarkerNowMs()
    local _, snapshot = findTrackedVictimSnapshot(victimId)
    local targetObjectId = snapshot and snapshot.objectId or nil
    local lastTing = damageTracker.lastHitSoundMs

    local function targetMatches(event)
        if targetObjectId and sameHaloDatumId(event.targetObjectId, targetObjectId) then
            return true
        end
        return sameHaloDatumId(event.targetObjectId, victimId)
    end

    local best = nil
    local bestSoundAge = nil
    for index = #nativeDamageTracker.recent, 1, -1 do
        local event = nativeDamageTracker.recent[index]
        local age = now - (tonumber(event.timeMs) or now)
        if age >= 0 and age <= HEADSHOT_CONFIRM_WINDOW_MS and
           not event.headshotConsumed and targetMatches(event) and
           event.soundPaired and event.soundTimeMs and lastTing then
            local soundAge = math.abs((tonumber(event.soundTimeMs) or now) - lastTing)
            if soundAge <= NATIVE_SOUND_MATCH_WINDOW_MS and
               (not bestSoundAge or soundAge < bestSoundAge) then
                best = event
                bestSoundAge = soundAge
            end
        end
    end

    if best then
        best.headshotConsumed = true
        if best.exactHeadHit == true and best.damageEffectHeadshot == true then
            return {
                timeMs = best.timeMs,
                objectId = best.targetObjectId,
                damage = nil,
                critical = true,
                headshotCandidate = true,
                headshotConsumed = true,
                weaponPath = best.weaponPath,
                nodeName = best.nodeName,
                regionName = best.regionName,
                nativeHeadshot = true,
                killFallback = false,
                nativeSequence = best.sequence
            }
        end

        return nil
    end

    local newest = nil
    for index = #nativeDamageTracker.recent, 1, -1 do
        local event = nativeDamageTracker.recent[index]
        local age = now - (tonumber(event.timeMs) or now)
        if age >= 0 and age <= HEADSHOT_CONFIRM_WINDOW_MS and
           not event.headshotConsumed and targetMatches(event) then
            newest = event
            break
        end
    end

    if not newest then
        local unique = nil
        local count = 0
        for index = #nativeDamageTracker.recent, 1, -1 do
            local event = nativeDamageTracker.recent[index]
            local age = now - (tonumber(event.timeMs) or now)
            if age >= 0 and age <= HEADSHOT_CONFIRM_WINDOW_MS and
               not event.headshotConsumed and
               event.exactHeadHit == true and
               event.damageEffectHeadshot == true then
                unique = event
                count = count + 1
            end
        end
        if count == 1 then newest = unique end
    end

    if not newest or newest.headshotConsumed then return nil end
    newest.headshotConsumed = true

    if newest.exactHeadHit == true and newest.damageEffectHeadshot == true then
        return {
            timeMs = newest.timeMs,
            objectId = newest.targetObjectId,
            damage = nil,
            critical = true,
            headshotCandidate = true,
            headshotConsumed = true,
            weaponPath = newest.weaponPath,
            nodeName = newest.nodeName,
            regionName = newest.regionName,
            nativeHeadshot = true,
            killFallback = false,
            nativeSequence = newest.sequence
        }
    end

    return nil
end

local function buildLooseHeadshotKillFallback()
    local now = getHitmarkerNowMs()
    local lastHit = damageTracker.lastHitSoundMs

    if not lastHit or
       math.abs(now - lastHit) > HEADSHOT_CONFIRM_WINDOW_MS then
        return nil
    end

    local weaponPath = damageTracker.lastHitWeaponPath or ""
    if not isHeadshotCapableWeapon(weaponPath) then
        return nil
    end

    local candidate = nil
    local candidateCount = 0

    for index = #damageTracker.recentLocalDamage, 1, -1 do
        local event = damageTracker.recentLocalDamage[index]
        local age = now - event.timeMs

        if age >= 0 and age <= HEADSHOT_CONFIRM_WINDOW_MS and
           not event.headshotConsumed then
            local maybeUnshielded =
                (tonumber(event.shieldAfter) or 1.0) <= HEADSHOT_SHIELD_EPSILON or
                (tonumber(event.shieldBefore) or 1.0) <= HEADSHOT_SHIELD_EPSILON

            if maybeUnshielded and
               isHeadshotCapableWeapon(event.weaponPath or weaponPath) then
                candidate = event
                candidateCount = candidateCount + 1
            end
        end
    end

    if candidateCount == 1 then
        candidate.headshotConsumed = true
        candidate.headshotCandidate = true
        candidate.critical = true
        candidate.killFallback = true
        return candidate
    end

    return nil
end

local function buildHeadshotKillFallback(victimId)
    local now = getHitmarkerNowMs()
    local lastHit = damageTracker.lastHitSoundMs

    if not lastHit or
       math.abs(now - lastHit) > HEADSHOT_CONFIRM_WINDOW_MS then
        return nil
    end

    local weaponPath = damageTracker.lastHitWeaponPath or ""
    if not isHeadshotCapableWeapon(weaponPath) then
        return nil
    end

    local playerIndex, snapshot =
        findTrackedVictimSnapshot(victimId)

    if not snapshot or
       (tonumber(snapshot.shield) or 0.0) > HEADSHOT_SHIELD_EPSILON then
        return nil
    end

    return {
        timeMs = now,
        playerIndex = playerIndex,
        playerId = snapshot.playerId,
        objectId = snapshot.objectId,
        damage = nil,
        critical = true,
        headshotCandidate = true,
        headshotConsumed = true,
        shieldBefore = snapshot.shield,
        shieldAfter = 0.0,
        healthBefore = snapshot.health,
        healthAfter = 0.0,
        weaponPath = weaponPath,
        exactAttribution = false,
        killFallback = true
    }
end

local HUD_SCALE_DONT_SCALE_OFFSET = 0x1
local HUD_SCALE_USE_HIGH_RES = 0x4

local BITMAP_HALF_HUD_SCALE = 0x10
local BITMAP_FORCE_HUD_USE_HIGHRES_SCALE = 0x80

local CROSSHAIR_OVERLAY_NOT_A_SPRITE = 0x2
local CROSSHAIR_OVERLAY_NOT_ON_DEFAULT_ZOOM = 0x4
local CROSSHAIR_OVERLAY_SHOW_SNIPER_DATA = 0x8
local CROSSHAIR_OVERLAY_HIDE_AREA_OUTSIDE_RETICLE = 0x10
local CROSSHAIR_OVERLAY_ONE_ZOOM_LEVEL = 0x20
local CROSSHAIR_OVERLAY_ONLY_ON_DEFAULT_ZOOM = 0x40
local CROSSHAIR_OVERLAY_RUNTIME_INVALID = 0x80

local BASE_HUD_HEIGHT = 480

local function hasMask(value, mask)
    value = tonumber(value) or 0
    return math.floor(value / mask) % 2 == 1
end

local function normalizeHudScale(value)
    value = tonumber(value)
    if not value or math.abs(value) < 0.000001 then
        return 1.0
    end
    return value
end

local function getCurrentZoomLevel()
    local playerAddress = get_dynamic_player()
    if not playerAddress then return 0xFF end
    local ok, player = pcall(blam.biped, playerAddress)
    if not ok or not player or player.zoomLevel == nil then return 0xFF end
    return player.zoomLevel
end

local function isCrosshairOverlayVisible(overlay, zoomLevel)
    local flags = tonumber(overlay.flags) or 0
    if hasMask(flags, CROSSHAIR_OVERLAY_RUNTIME_INVALID) then return false end
    if hasMask(flags, CROSSHAIR_OVERLAY_SHOW_SNIPER_DATA) or
       hasMask(flags, CROSSHAIR_OVERLAY_HIDE_AREA_OUTSIDE_RETICLE) then

        return false
    end

    local zoomed = zoomLevel ~= 0xFF
    if hasMask(flags, CROSSHAIR_OVERLAY_NOT_ON_DEFAULT_ZOOM) and not zoomed then
        return false
    end
    if hasMask(flags, CROSSHAIR_OVERLAY_ONLY_ON_DEFAULT_ZOOM) and zoomed then
        return false
    end
    return true
end

local function getOverlayOffsetScale(overlay)
    local flags = tonumber(overlay.scalingFlags) or 0
    local referenceHeight =
        hasMask(flags, HUD_SCALE_USE_HIGH_RES) and 960 or BASE_HUD_HEIGHT

    if hasMask(flags, HUD_SCALE_DONT_SCALE_OFFSET) then
        return 1.0
    end

    return screenHeight / referenceHeight
end

local function getBitmapHudDivisor(bitmap, overlay)
    local bitmapFlags = tonumber(bitmap.usageFlags) or 0
    local overlayFlags = tonumber(overlay.scalingFlags) or 0
    local divisor = 1

    if hasMask(bitmapFlags, BITMAP_HALF_HUD_SCALE) then
        divisor = divisor * 2
    end

    if hasMask(bitmapFlags, BITMAP_FORCE_HUD_USE_HIGHRES_SCALE) and
       not hasMask(overlayFlags, HUD_SCALE_USE_HIGH_RES) then
        divisor = divisor * 2
    end

    return divisor
end

local function getWeaponHudChain(rootTag)
    local chain = {}
    local tag = rootTag
    local seen = {}

    for _ = 1, 16 do
        if not tag or not tag.id or seen[tag.id] then break end
        seen[tag.id] = true

        local ok, hud = pcall(blam.weaponHudInterface, tag.id)
        if not ok or not hud then break end
        chain[#chain + 1] = {tag = tag, hud = hud}

        local childId = hud.childHud
        if not childId or blam.isNull(childId) then break end
        local child = safeGetTag(childId)
        if not child or child.class ~= "wphi" then break end
        tag = child
    end

    return chain
end

local function getCrosshairFrameIndex(crosshairType, overlay, sequence, zoomLevel)
    local count = sequence and sequence.sprites and #sequence.sprites or 0
    if count <= 0 then return nil end

    if crosshairType == 1 then
        if zoomLevel == 0xFF then return nil end
        local flags = tonumber(overlay.flags) or 0
        local runtimeZoom = zoomLevel + 1

        if hasMask(flags, CROSSHAIR_OVERLAY_ONE_ZOOM_LEVEL) then
            if runtimeZoom == 0 then return nil end
            return 0
        end

        local frame = runtimeZoom -
            (hasMask(flags, CROSSHAIR_OVERLAY_NOT_ON_DEFAULT_ZOOM) and 1 or 0)
        if frame < 0 then return nil end
        if frame >= count then frame = count - 1 end
        return frame
    end

    return 0
end

local function measureCrosshairType(hud, wantedType, zoomLevel)
    local spriteBounds = {}
    local directBounds = {}

    local bestZoomCandidate = nil

    local function include(box, left, top, right, bottom)
        if not left or not top or not right or not bottom then return end

        local l = math.min(left, right)
        local r = math.max(left, right)
        local t = math.min(top, bottom)
        local b = math.max(top, bottom)

        if r <= l or b <= t then return end

        box.minX = math.min(box.minX or l, l)
        box.minY = math.min(box.minY or t, t)
        box.maxX = math.max(box.maxX or r, r)
        box.maxY = math.max(box.maxY or b, b)
        box.found = true
    end

    local function getContentBounds(entry, clipLeft, clipTop, clipRight, clipBottom)
        local hardware = tonumber(entry.hardwareFormat)
        local baseAddress = tonumber(entry.baseAddress)
        local bw = tonumber(entry.width) or 0
        local bh = tonumber(entry.height) or 0
        local format = tonumber(entry.format)

        if bw <= 0 or bh <= 0 then
            return nil
        end

        if hardware and hardware ~= 0 and
           type(optic.get_halo_texture_alpha_bounds) == "function" then

            local key =
                tostring(hardware) .. "|" ..
                string.format("%.6f,%.6f,%.6f,%.6f",
                    clipLeft, clipTop, clipRight, clipBottom)

            local cached = haloReticleAlphaCache[key]

            if cached then
                return cached.width, cached.height,
                       cached.left, cached.top,
                       cached.right, cached.bottom,
                       "d3d-content"
            end

            local ok, texW, texH, aLeft, aTop, aRight, aBottom =
                pcall(
                    optic.get_halo_texture_alpha_bounds,
                    hardware,
                    clipLeft, clipTop, clipRight, clipBottom
                )

            if ok and texW and texH and
               aLeft and aTop and aRight and aBottom then

                local result = {
                    width = tonumber(texW),
                    height = tonumber(texH),
                    left = tonumber(aLeft),
                    top = tonumber(aTop),
                    right = tonumber(aRight),
                    bottom = tonumber(aBottom)
                }

                if result.width and result.height and
                   result.left and result.top and
                   result.right and result.bottom and
                   result.right > result.left and
                   result.bottom > result.top then

                    haloReticleAlphaCache[key] = result

                    return result.width, result.height,
                           result.left, result.top,
                           result.right, result.bottom,
                           "d3d-content"
                end
            end
        end

        if baseAddress and baseAddress ~= 0 and
           format ~= nil and
           type(optic.get_halo_bitmap_alpha_bounds) == "function" then

            local ok, aLeft, aTop, aRight, aBottom =
                pcall(
                    optic.get_halo_bitmap_alpha_bounds,
                    baseAddress,
                    bw,
                    bh,
                    format,
                    clipLeft, clipTop, clipRight, clipBottom
                )

            if ok and aLeft and aTop and aRight and aBottom then
                return bw, bh,
                       tonumber(aLeft), tonumber(aTop),
                       tonumber(aRight), tonumber(aBottom),
                       "cpu"
            end
        end

        return nil
    end

    for _, crosshair in ipairs(hud.crosshairs or {}) do
        if crosshair.type == wantedType and
           crosshair.bitmap and
           not blam.isNull(crosshair.bitmap) then

            local okBitmap, bitmap = pcall(blam.bitmap, crosshair.bitmap)

            if okBitmap and bitmap and crosshair.overlays then
                for _, overlay in ipairs(crosshair.overlays) do
                    if isCrosshairOverlayVisible(overlay, zoomLevel) then
                        local seqIndex = tonumber(overlay.sequenceIndex) or -1

                        if seqIndex >= 0 then
                            local widthScale =
                                normalizeHudScale(overlay.widthScale)
                            local heightScale =
                                normalizeHudScale(overlay.heightScale)

                            local offsetScale =
                                getOverlayOffsetScale(overlay)

                            local offsetX =
                                (tonumber(overlay.x) or 0) * offsetScale
                            local offsetY =
                                (tonumber(overlay.y) or 0) * offsetScale

                            local flags = tonumber(overlay.flags) or 0
                            local isSprite =
                                not hasMask(
                                    flags,
                                    CROSSHAIR_OVERLAY_NOT_A_SPRITE
                                )

                            local bitmapIndex
                            local clipLeft, clipTop, clipRight, clipBottom
                            local frameIndex

                            if isSprite then
                                local sequence =
                                    bitmap.sequences and
                                    bitmap.sequences[seqIndex + 1]

                                frameIndex =
                                    getCrosshairFrameIndex(
                                        wantedType,
                                        overlay,
                                        sequence,
                                        zoomLevel
                                    )

                                local frame =
                                    frameIndex ~= nil and
                                    sequence and
                                    sequence.sprites and
                                    sequence.sprites[frameIndex + 1]

                                if frame then
                                    bitmapIndex = tonumber(frame.bitmapIndex)
                                    clipLeft = tonumber(frame.left)
                                    clipRight = tonumber(frame.right)
                                    clipTop = tonumber(frame.top)
                                    clipBottom = tonumber(frame.bottom)
                                end
                            else
                                bitmapIndex = seqIndex
                                clipLeft, clipTop, clipRight, clipBottom =
                                    0, 0, 1, 1
                            end

                            local entry =
                                bitmapIndex ~= nil and
                                bitmap.bitmaps and
                                bitmap.bitmaps[bitmapIndex + 1]

                            if entry and
                               clipLeft ~= nil and
                               clipTop ~= nil and
                               clipRight ~= nil and
                               clipBottom ~= nil then

                                local texW, texH,
                                      pixelLeft, pixelTop,
                                      pixelRight, pixelBottom,
                                      contentSource =
                                    getContentBounds(
                                        entry,
                                        clipLeft, clipTop,
                                        clipRight, clipBottom
                                    )

                                if not texW then
                                    local bw = tonumber(entry.width) or 0
                                    local bh = tonumber(entry.height) or 0

                                    if bw > 0 and bh > 0 then
                                        texW, texH = bw, bh
                                        pixelLeft =
                                            math.min(clipLeft, clipRight) * bw
                                        pixelRight =
                                            math.max(clipLeft, clipRight) * bw
                                        pixelTop =
                                            math.min(clipTop, clipBottom) * bh
                                        pixelBottom =
                                            math.max(clipTop, clipBottom) * bh
                                        contentSource = "clip"
                                    end
                                end

                                if texW and texH and
                                   pixelLeft and pixelTop and
                                   pixelRight and pixelBottom then

                                    local bitmapDivisor =
                                        getBitmapHudDivisor(bitmap, overlay)

                                    local bitmapScale = 1.0 / bitmapDivisor

                                    local clipCenterX =
                                        (
                                            math.min(clipLeft, clipRight) * texW +
                                            math.max(clipLeft, clipRight) * texW
                                        ) * 0.5

                                    local clipCenterY =
                                        (
                                            math.min(clipTop, clipBottom) * texH +
                                            math.max(clipTop, clipBottom) * texH
                                        ) * 0.5

                                    local x1 =
                                        offsetX +
                                        (pixelLeft - clipCenterX) *
                                        widthScale * bitmapScale

                                    local x2 =
                                        offsetX +
                                        (pixelRight - clipCenterX) *
                                        widthScale * bitmapScale

                                    local y1 =
                                        offsetY +
                                        (pixelTop - clipCenterY) *
                                        heightScale * bitmapScale

                                    local y2 =
                                        offsetY +
                                        (pixelBottom - clipCenterY) *
                                        heightScale * bitmapScale

                                    local nativeW = math.abs(x2 - x1)
                                    local nativeH = math.abs(y2 - y1)

                                    if wantedType == 1 and isSprite and
                                       nativeW > 0 and nativeH > 0 then
                                        local centerX = (x1 + x2) * 0.5
                                        local centerY = (y1 + y2) * 0.5
                                        local distance2 =
                                            centerX * centerX + centerY * centerY

                                        if not bestZoomCandidate or
                                           distance2 < bestZoomCandidate.distance2 or
                                           (distance2 == bestZoomCandidate.distance2 and
                                            nativeW * nativeH <
                                            bestZoomCandidate.width *
                                            bestZoomCandidate.height) then
                                            bestZoomCandidate = {
                                                width = nativeW,
                                                height = nativeH,
                                                centerX = centerX,
                                                centerY = centerY,
                                                distance2 = distance2
                                            }
                                        end
                                    end

                                    if isSprite then
                                        if nativeW <= screenWidth * 0.25 and
                                           nativeH <= screenHeight * 0.25 then
                                            include(
                                                spriteBounds,
                                                x1, y1, x2, y2
                                            )
                                        end
                                    elseif nativeW <= screenWidth * 0.20 and
                                           nativeH <= screenHeight * 0.20 then
                                        include(
                                            directBounds,
                                            x1, y1, x2, y2
                                        )
                                    end
                                end
                            end
                        end
                    end
                end
            end
        end
    end

    if wantedType == 1 and bestZoomCandidate then
        local width =
            math.max(1, math.floor(bestZoomCandidate.width + 0.5))
        local height =
            math.max(1, math.floor(bestZoomCandidate.height + 0.5))

        local centerDistance =
            math.sqrt(bestZoomCandidate.distance2)

        if centerDistance <= math.max(96, math.max(width, height) * 4) and
           width <= screenWidth * 0.20 and
           height <= screenHeight * 0.20 then
            return width, height
        end

    end

    local box
    local source

    if spriteBounds.found then
        box = spriteBounds
        source = "sprite"
    elseif directBounds.found then
        box = directBounds
        source = "direct"
    else
        return nil, nil
    end

    local halfWidth =
        math.max(math.abs(box.minX), math.abs(box.maxX))
    local halfHeight =
        math.max(math.abs(box.minY), math.abs(box.maxY))

    local width =
        math.max(1, math.floor(halfWidth * 2 + 0.5))
    local height =
        math.max(1, math.floor(halfHeight * 2 + 0.5))

    if width > screenWidth * 0.25 or
       height > screenHeight * 0.25 then
        return nil, nil
    end

    return width, height
end

local function getCurrentCrosshairSize()

    local weaponTag = getCurrentWeaponTag()
    if not weaponTag then
        return nil, nil
    end

    local zoomLevel = getCurrentZoomLevel()
    local sizeCache = weaponHudTagCache.__crosshairSize
    if not sizeCache then
        sizeCache = {}
        weaponHudTagCache.__crosshairSize = sizeCache
    end

    local cacheKey =
        tostring(weaponTag.id or weaponTag.data or "") .. "|" ..
        tostring(zoomLevel) .. "|" ..
        tostring(screenWidth) .. "x" .. tostring(screenHeight)

    local cached = sizeCache[cacheKey]
    if cached then
        return cached.width, cached.height
    end

    local rootHudTag = getWeaponHudTag(weaponTag)
    if not rootHudTag then
        return nil, nil
    end

    local aimW, aimH
    local zoomW, zoomH

    for _, node in ipairs(getWeaponHudChain(rootHudTag)) do
        local w, h = measureCrosshairType(node.hud, 0, zoomLevel)
        if w and h then
            aimW = math.max(aimW or 0, w)
            aimH = math.max(aimH or 0, h)
        end

        if zoomLevel ~= 0xFF then
            w, h = measureCrosshairType(node.hud, 1, zoomLevel)
            if w and h then
                local area = w * h
                local currentArea =
                    zoomW and zoomH and (zoomW * zoomH) or nil

                if not currentArea or area < currentArea then
                    zoomW = w
                    zoomH = h
                end
            end
        end
    end

    local width, height
    if zoomLevel ~= 0xFF and zoomW and zoomH then
        width, height = zoomW, zoomH
    else
        width, height = aimW, aimH
    end

    if width and height then
        sizeCache[cacheKey] = {width = width, height = height}
    end

    return width, height
end

local function getAnimatedHitmarkerFxSprite(name, reticleWidth, reticleHeight, comboLevel)
    local cache = proceduralHitmarkerFxCache[name]
    local effectKind = getHitmarkerEffectKind(name)
    local isKill = effectKind == 1
    local path = harmonySpritePaths[name]

    if not path and name == "hitmarker_critical" then
        path = harmonySpritePaths.hitmarker_kill
    elseif not path and not isKill then
        path = harmonySpritePaths.hitmarker
    end

    if not cache or not path or
       type(optic.create_procedural_hitmarker_fx) ~= "function" then
        return nil, nil, nil, nil
    end

    local rs = getHitmarkerResolutionScale()
    local armLength, armThickness, padding = getHitmarkerGeometry(name, 0)
    local flashLength = HITMARKER_FLASH_LENGTH_1080 * rs
    local flashThickness = HITMARKER_FLASH_THICKNESS_1080 * rs
    local motion = (isKill and HITMARKER_KILL_EXPANSION_1080
                     or HITMARKER_NORMAL_MOTION_1080) * rs

    local rw = math.max(1, math.floor((tonumber(reticleWidth) or 1) + 0.5))
    local rh = math.max(1, math.floor((tonumber(reticleHeight) or 1) + 0.5))
    comboLevel = math.max(1, math.min(HITMARKER_COMBO_MAX_VISUAL_LEVEL,
                                     tonumber(comboLevel) or 1))

    local overrideColor = hitmarkerOverrideColor(name)
    local rgbKey = overrideColor and
        ("|rgb=" .. overrideColor.r .. "," .. overrideColor.g .. "," .. overrideColor.b) or
        "|rgb=pack"

    local key = path .. "|" .. tostring(rw) .. "x" .. tostring(rh) ..
                "|e=" .. tostring(effectKind) ..
                "|c=" .. tostring(comboLevel) ..
                "|l=" .. string.format("%.2f", armLength) ..
                "|t=" .. string.format("%.2f", armThickness) ..
                "|p=" .. string.format("%.2f", padding) ..
                "|m=" .. string.format("%.2f", motion) .. rgbKey

    local c = cache[key]
    if c then
        return c.handle, (screenWidth - c.width) / 2,
               (screenHeight - c.height) / 2, c.duration
    end

    local ok, handle, cw, ch, duration
    if overrideColor then
        ok, handle, cw, ch, duration = pcall(
            optic.create_procedural_hitmarker_fx,
            path, rw, rh, armLength, armThickness, padding,
            effectKind, comboLevel, motion,
            flashLength, flashThickness,
            overrideColor.r, overrideColor.g, overrideColor.b)

        if not ok then
            ok, handle, cw, ch, duration = pcall(
                optic.create_procedural_hitmarker_fx,
                path, rw, rh, armLength, armThickness, padding,
                effectKind, comboLevel, motion,
                flashLength, flashThickness)
        end
    else
        ok, handle, cw, ch, duration = pcall(
            optic.create_procedural_hitmarker_fx,
            path, rw, rh, armLength, armThickness, padding,
            effectKind, comboLevel, motion,
            flashLength, flashThickness)
    end

    if not ok or not handle or not cw or not ch or not duration then
        return nil, nil, nil, nil
    end

    c = {handle = handle, width = tonumber(cw), height = tonumber(ch),
         duration = tonumber(duration)}
    cache[key] = c

    return c.handle, (screenWidth - c.width) / 2,
           (screenHeight - c.height) / 2, c.duration
end

local function getProceduralHitmarkerSprite(
    name,
    reticleWidth,
    reticleHeight,
    extraPadding
)
    local cache = proceduralHitmarkerCache[name]
    local path = harmonySpritePaths[name]

    if not path and name == "hitmarker_critical" then
        path = harmonySpritePaths.hitmarker_kill
    elseif not path and name ~= "hitmarker_kill" then
        path = harmonySpritePaths.hitmarker
    end

    if not cache or not path or
       type(optic.create_procedural_hitmarker) ~= "function" then
        return nil, nil, nil
    end

    local armLength, armThickness, padding =
        getHitmarkerGeometry(name, extraPadding)

    local rw = math.max(1, math.floor((tonumber(reticleWidth) or 1) + 0.5))
    local rh = math.max(1, math.floor((tonumber(reticleHeight) or 1) + 0.5))

    local overrideColor = hitmarkerOverrideColor(name)
    local rgbKey = overrideColor and
        ("|rgb=" .. overrideColor.r .. "," .. overrideColor.g .. "," .. overrideColor.b) or
        "|rgb=pack"

    local key =
        path .. "|" .. tostring(rw) .. "x" .. tostring(rh) ..
        "|l=" .. string.format("%.2f", armLength) ..
        "|t=" .. string.format("%.2f", armThickness) ..
        "|p=" .. string.format("%.2f", padding) .. rgbKey

    local cached = cache[key]
    if cached then
        return cached.handle,
               (screenWidth - cached.width) / 2,
               (screenHeight - cached.height) / 2
    end

    local ok, handle, canvasWidth, canvasHeight
    if overrideColor then
        ok, handle, canvasWidth, canvasHeight = pcall(
            optic.create_procedural_hitmarker,
            path, rw, rh, armLength, armThickness, padding,
            overrideColor.r, overrideColor.g, overrideColor.b
        )

        if not ok then
            ok, handle, canvasWidth, canvasHeight = pcall(
                optic.create_procedural_hitmarker,
                path, rw, rh, armLength, armThickness, padding
            )
        end
    else
        ok, handle, canvasWidth, canvasHeight = pcall(
            optic.create_procedural_hitmarker,
            path, rw, rh, armLength, armThickness, padding
        )
    end

    if not ok or not handle or not canvasWidth or not canvasHeight then
        return nil, nil, nil
    end

    cached = {
        handle = handle,
        width = tonumber(canvasWidth),
        height = tonumber(canvasHeight)
    }
    cache[key] = cached

    return cached.handle,
           (screenWidth - cached.width) / 2,
           (screenHeight - cached.height) / 2
end

local function getProceduralHitFlashSprite(name)
    local cache = proceduralHitFlashCache[name]
    local path = harmonySpritePaths[name]

    if not cache or not path or
       type(optic.create_procedural_hit_flash) ~= "function" then
        return nil, nil, nil
    end

    local resolutionScale =
        getHitmarkerResolutionScale()

    local flashLength =
        HITMARKER_FLASH_LENGTH_1080 * resolutionScale

    local flashThickness =
        HITMARKER_FLASH_THICKNESS_1080 * resolutionScale

    local key =
        path ..
        "|l=" .. string.format("%.2f", flashLength) ..
        "|t=" .. string.format("%.2f", flashThickness)

    local cached = cache[key]
    if cached then
        return cached.handle,
               (screenWidth - cached.width) / 2,
               (screenHeight - cached.height) / 2
    end

    local ok, handle, canvasWidth, canvasHeight =
        pcall(
            optic.create_procedural_hit_flash,
            path,
            flashLength,
            flashThickness
        )

    if not ok or not handle or not canvasWidth or not canvasHeight then
        return nil, nil, nil
    end

    cached = {
        handle = handle,
        width = tonumber(canvasWidth),
        height = tonumber(canvasHeight)
    }

    cache[key] = cached

    return cached.handle,
           (screenWidth - cached.width) / 2,
           (screenHeight - cached.height) / 2
end

local function getHitmarkerTargetSize(reticleWidth, reticleHeight)
    local width = math.max(1, tonumber(reticleWidth) or 1)
    local height = math.max(1, tonumber(reticleHeight) or 1)

    local reticleDiameter = math.max(width, height)

    local targetDiameter = math.max(
        HITMARKER_MIN_DIAMETER,
        reticleDiameter + HITMARKER_EXTRA_DIAMETER
    )

    targetDiameter = math.min(
        HITMARKER_MAX_DIAMETER,
        math.floor(targetDiameter + 0.5)
    )

    return targetDiameter, targetDiameter
end

local function getHitmarkerImageBounds(name)
    local path = harmonySpritePaths[name]
    if not path then
        return nil
    end

    local cached = hitmarkerImageBoundsCache[path]
    if cached then
        return cached
    end

    if type(optic.get_image_content_bounds) ~= "function" then
        return nil
    end

    local ok, sourceWidth, sourceHeight, left, top, right, bottom =
        pcall(optic.get_image_content_bounds, path)

    if not ok then
        return nil
    end

    sourceWidth = tonumber(sourceWidth)
    sourceHeight = tonumber(sourceHeight)
    left = tonumber(left)
    top = tonumber(top)
    right = tonumber(right)
    bottom = tonumber(bottom)

    if not sourceWidth or not sourceHeight or
       not left or not top or not right or not bottom or
       sourceWidth <= 0 or sourceHeight <= 0 or
       right <= left or bottom <= top then
        return nil
    end

    cached = {
        sourceWidth = sourceWidth,
        sourceHeight = sourceHeight,
        left = left,
        top = top,
        right = right,
        bottom = bottom,
        contentWidth = right - left,
        contentHeight = bottom - top
    }

    hitmarkerImageBoundsCache[path] = cached

    return cached
end

local function getSizedHitmarkerSprite(name, targetWidth, targetHeight)
    local cache = hitmarkerSpriteCache[name]
    local path = harmonySpritePaths[name]
    if not cache or not path then
        return harmonySprites[name], nil, nil
    end

    targetWidth = math.max(1, tonumber(targetWidth) or 1)
    targetHeight = math.max(1, tonumber(targetHeight) or 1)

    local bounds = getHitmarkerImageBounds(name)
    if not bounds then

        local fallbackWidth = math.max(1, math.floor(targetWidth + 0.5))
        local fallbackHeight = math.max(1, math.floor(targetHeight + 0.5))
        local fallbackKey = path .. "|legacy|" ..
                            tostring(fallbackWidth) .. "x" .. tostring(fallbackHeight)

        if cache[fallbackKey] then
            return cache[fallbackKey],
                   (screenWidth - fallbackWidth) / 2,
                   (screenHeight - fallbackHeight) / 2
        end

        local ok, handle =
            pcall(optic.create_sprite, path, fallbackWidth, fallbackHeight)
        if ok and handle then
            cache[fallbackKey] = handle
            return handle,
                   (screenWidth - fallbackWidth) / 2,
                   (screenHeight - fallbackHeight) / 2
        end

        return harmonySprites[name], nil, nil
    end

    local scaleX = targetWidth / bounds.contentWidth
    local scaleY = targetHeight / bounds.contentHeight

    local fullWidth =
        math.max(1, math.floor(bounds.sourceWidth * scaleX + 0.5))
    local fullHeight =
        math.max(1, math.floor(bounds.sourceHeight * scaleY + 0.5))

    if fullWidth > screenWidth * 4 or fullHeight > screenHeight * 4 then
        return harmonySprites[name], nil, nil
    end

    local key = path .. "|" .. tostring(fullWidth) .. "x" .. tostring(fullHeight)
    local handle = cache[key]

    if not handle then
        local ok, created =
            pcall(optic.create_sprite, path, fullWidth, fullHeight)
        if not ok or not created then
            return harmonySprites[name], nil, nil
        end

        handle = created
        cache[key] = handle
    end

    local actualScaleX = fullWidth / bounds.sourceWidth
    local actualScaleY = fullHeight / bounds.sourceHeight

    local visibleWidth = bounds.contentWidth * actualScaleX
    local visibleHeight = bounds.contentHeight * actualScaleY

    local drawX =
        (screenWidth * 0.5) -
        (visibleWidth * 0.5) -
        (bounds.left * actualScaleX)

    local drawY =
        (screenHeight * 0.5) -
        (visibleHeight * 0.5) -
        (bounds.top * actualScaleY)

    return handle, drawX, drawY
end

local function loadOpticStyle()
    local styleFile = read_file(opticStylePath:format(configuration.style))
    if (styleFile) then
        local style = json.decode(styleFile)
        if (style) then
            defaultMedalSize = (screenHeight / style.medalSizeFactor) - 1
            return true
        end
    end
    console_out("Error, Optic style does not have a style.json file!")
    return false
end

local function loadOpticConfiguration()
    local opticConfiguration = read_file("optic.json")
    if (opticConfiguration) then
        local loadedConfiguration = json.decode(opticConfiguration) or {}
        local telemetryVersion =
            tonumber(loadedConfiguration.combatTelemetryVersion) or 1

        configuration = util.update(configuration, loadedConfiguration)

        if telemetryVersion < 2 then
            if loadedConfiguration.criticalDamageThreshold ~= nil then
                configuration.criticalDamageThreshold =
                    math.max(
                        1,
                        (tonumber(loadedConfiguration.criticalDamageThreshold) or 40) * 0.5
                    )
            else
                configuration.criticalDamageThreshold = 20
            end

            if loadedConfiguration.headshotMinDamage ~= nil then
                configuration.headshotMinDamage =
                    math.max(
                        1,
                        (tonumber(loadedConfiguration.headshotMinDamage) or 40) * 0.5
                    )
            else
                configuration.headshotMinDamage = 20
            end

            telemetryVersion = 2
            configuration.combatTelemetryVersion = 2
        end

        if telemetryVersion < 3 then
            if loadedConfiguration.criticalDamageThreshold ~= nil then
                configuration.criticalDamageThreshold =
                    math.max(
                        1,
                        math.floor(((tonumber(configuration.criticalDamageThreshold) or 20) * 1.5) + 0.5)
                    )
            else
                configuration.criticalDamageThreshold = 30
            end

            if loadedConfiguration.headshotMinDamage ~= nil then
                configuration.headshotMinDamage =
                    math.max(
                        1,
                        math.floor(((tonumber(configuration.headshotMinDamage) or 20) * 1.5) + 0.5)
                    )
            else
                configuration.headshotMinDamage = 30
            end

            configuration.combatTelemetryVersion = 3
        end

        if telemetryVersion < 4 then
            configuration.criticalDamageThreshold = 48
            configuration.combatTelemetryVersion = 4
            telemetryVersion = 4
        end

        if telemetryVersion < 5 then
            configuration.criticalDamageThreshold = 48
            configuration.combatTelemetryVersion = 5
            telemetryVersion = 5
        end

        loadOpticStyle()
        return true
    end
    return false
end

local function saveOpticConfiguration()
    return not not write_file("optic.json", json.encode(configuration))
end

function OnScriptLoad()
    loadOpticConfiguration()
    refreshNativeDamageProbeState(true)
    resetDamageTracker()

    sprites = util.createSprites(defaultMedalSize)

    sounds = {
        suicide = {name = "suicide"},
        betrayal = {name = "betrayal"},
        hit = {name = "hit"}
    }

    for event, sprite in pairs(sprites) do
        if (sprite.name) then
            local medalImagePath = image(sprite.name)
            local medalSoundPath = audio(sprite.name)
            if not file_exists(medalImagePath) and sprite.alias then
                medalImagePath = image(sprite.alias)
                medalSoundPath = audio(sprite.alias)
            end
            if (file_exists(medalImagePath)) then
                harmonySpritePaths[sprite.name] = medalImagePath
                if (file_exists(medalSoundPath)) then
                    harmonySprites[sprite.name] = optic.create_sprite(medalImagePath, sprite.width,
                                                                      sprite.height)
                    if configuration.enableSound then
                        harmonySounds[sprite.name] = optic.create_sound(medalSoundPath)
                        sprites[event].hasAudio = true
                    end
                else

                    harmonySprites[sprite.name] = optic.create_sprite(medalImagePath, sprite.width,
                                                                      sprite.height)
                end
            end
        end
    end

    if not harmonySprites.headshot and
       type(optic.create_procedural_headshot_medal) == "function" then
        local ok, handle, width, height = pcall(
            optic.create_procedural_headshot_medal,
            defaultMedalSize
        )

        if ok and handle then
            harmonySprites.headshot = handle
            if sprites.headShot then
                sprites.headShot.width = tonumber(width) or defaultMedalSize
                sprites.headShot.height = tonumber(height) or defaultMedalSize
            end
        end
    end

    for event, sound in pairs(sounds) do
        if (sound.name) then
            local soundPath = audio(sound.name)
            if (file_exists(soundPath)) then
                harmonySounds[sound.name] = optic.create_sound(soundPath)
            end
        end
    end

    local fadeInAnimation = optic.create_animation(300)
    optic.set_animation_property(fadeInAnimation, "ease in", "position x", defaultMedalSize)
    optic.set_animation_property(fadeInAnimation, "ease in", "opacity", 255)

    local fadeOutAnimation = optic.create_animation(400)
    optic.set_animation_property(fadeOutAnimation, "ease out", "opacity", -255)

    local slideAnimation = optic.create_animation(250)
    optic.set_animation_property(slideAnimation, 0.4, 0.0, 0.6, 1.0, "position x", defaultMedalSize)

    hitmarkerEnterAnimation = optic.create_animation(0)

    hitmarkerNormalFadeAnimation = optic.create_animation(90)
    optic.set_animation_property(
        hitmarkerNormalFadeAnimation,
        "linear",
        "opacity",
        -255
    )

    hitmarkerEchoFadeAnimation = optic.create_animation(105)
    optic.set_animation_property(
        hitmarkerEchoFadeAnimation,
        "linear",
        "opacity",
        -255
    )

    hitmarkerFlashFadeAnimation = optic.create_animation(70)
    optic.set_animation_property(
        hitmarkerFlashFadeAnimation,
        "linear",
        "opacity",
        -255
    )

    hitmarkerKillFadeAnimation = optic.create_animation(130)
    optic.set_animation_property(
        hitmarkerKillFadeAnimation,
        "linear",
        "opacity",
        -255
    )

    damageNumberFadeAnimation = optic.create_animation(DAMAGE_NUMBER_DURATION_MS)
    optic.set_animation_property(
        damageNumberFadeAnimation,
        "ease out",
        "opacity",
        -255
    )
    optic.set_animation_property(
        damageNumberFadeAnimation,
        "ease out",
        "position y",
        -10.0 * getHitmarkerResolutionScale()
    )

    damageNumberRenderQueues = {}
    damageNumberNextQueue = 1
    local damageRs = getHitmarkerResolutionScale()
    local damageWidth = math.max(64, math.min(336, math.floor(112.0 * damageRs + 0.5)))
    local damageHeight = math.max(28, math.min(126, math.floor(42.0 * damageRs + 0.5)))
    local damageX = (screenWidth - damageWidth) * 0.5
    local damageBaseY = (screenHeight * 0.5) -
                        (DAMAGE_NUMBER_Y_OFFSET_1080 * damageRs) -
                        (damageHeight * 0.5)
    local damageLineGap = DAMAGE_NUMBER_LINE_GAP_1080 * damageRs

    for line = 1, DAMAGE_NUMBER_MAX_LINES do
        local damageY = damageBaseY -
                        ((line - 1) * (damageHeight + damageLineGap))
        damageNumberRenderQueues[line] = optic.create_render_queue(
            damageX,
            damageY,
            255,
            0,
            DAMAGE_NUMBER_DURATION_MS,
            1,
            hitmarkerEnterAnimation,
            damageNumberFadeAnimation
        )
    end

    renderQueue = optic.create_render_queue(50, (screenHeight / 2) - (defaultMedalSize / 2), 255, 0,
                                            4000, 0, fadeInAnimation, fadeOutAnimation,
                                            slideAnimation)

    if configuration.enableSound then
        AudioEngine = optic.create_audio_engine()
        harmony.optic.set_audio_engine_gain(AudioEngine, configuration.volume or 50)

        harmonySounds.__eventAudioEngine = optic.create_audio_engine()
        harmony.optic.set_audio_engine_gain(
            harmonySounds.__eventAudioEngine,
            configuration.volume or 50
        )

        harmonySounds.__hitAudioEngine = optic.create_audio_engine()
        harmony.optic.set_audio_engine_gain(
            harmonySounds.__hitAudioEngine,
            configuration.volume or 50
        )
    end

    medalsLoaded = true

    harmony.set_callback("multiplayer sound", "OnMultiplayerSound")
    harmony.set_callback("multiplayer event", "OnMultiplayerEvent")

end

local function toSentenceCase(name)
    return string.gsub(" " .. name:gsub("_", " "), "%W%l", string.upper):sub(2)
end

local function renderHitmarkerStatusIcon(hitmarkerName, reticleWidth, reticleHeight, duration)
    local iconName = HITMARKER_STATUS_ICON[hitmarkerName]
    if not iconName then return end

    local icon = harmonySprites[iconName]
    if not icon then return end

    local iconSize = math.max(18, defaultMedalSize * 0.42)
    local rs = getHitmarkerResolutionScale()
    local reticleRadius = math.max(
        tonumber(reticleWidth) or 0,
        tonumber(reticleHeight) or 0
    ) * 0.5
    local gap = 24.0 * rs
    local x
    local y

    if hitmarkerName == "hitmarker_critical" then
        x = (screenWidth - iconSize) * 0.5
        y = (screenHeight * 0.5) + reticleRadius + gap
    else
        x = (screenWidth * 0.5) + reticleRadius + gap
        y = (screenHeight - iconSize) * 0.5
    end

    optic.render_sprite(
        icon,
        x,
        y,
        255,
        0,
        tonumber(duration) or 165,
        hitmarkerEnterAnimation,
        hitmarkerNormalFadeAnimation
    )
end

local function medal(sprite, forcedComboLevel)
    if not medalsLoaded then
        console_out("Error, medals were not loaded properly!")
        return
    end

    medalsQueue[#medalsQueue + 1] = sprite.name
    local renderGroup = sprite.renderGroup
    local harmonySprite = harmonySprites[sprite.name]

    if sprite.name == "hitmarker_critical" and not harmonySprite then
        harmonySprite = harmonySprites.hitmarker_kill
    elseif isHitmarkerFxName(sprite.name) and
           sprite.name ~= "hitmarker_kill" and
           not harmonySprite then
        harmonySprite = harmonySprites.hitmarker
    end

    if not harmonySprite then return end

    if renderGroup then
        local renderWidth = sprite.width
        local renderHeight = sprite.height
        local x = (screenWidth - renderWidth) / 2
        local y = (screenHeight - renderHeight) / 2
        local crosshairWidth, crosshairHeight = nil, nil
        local statusDuration = 165

        if isHitmarkerFxName(sprite.name) then
            local comboLevel = forcedComboLevel or getHitmarkerComboLevel()
            crosshairWidth, crosshairHeight = getCurrentCrosshairSize()

            if crosshairWidth and crosshairHeight then
                local fxSprite, fxX, fxY, fxDuration =
                    getAnimatedHitmarkerFxSprite(
                        sprite.name,
                        crosshairWidth,
                        crosshairHeight,
                        comboLevel
                    )

                if fxSprite then
                    optic.render_sprite(fxSprite, fxX, fxY, 255, 0, fxDuration)
                    statusDuration = fxDuration
                    harmonySprite = nil
                end
            end
        end

        if harmonySprite and isHitmarkerFxName(sprite.name) then
            if not crosshairWidth or not crosshairHeight then
                crosshairWidth, crosshairHeight = getCurrentCrosshairSize()
            end

            if crosshairWidth and crosshairHeight then
                local pSprite, px, py = getProceduralHitmarkerSprite(
                    sprite.name, crosshairWidth, crosshairHeight, 0
                )
                if pSprite then
                    local isKill = sprite.name == "hitmarker_kill"
                    statusDuration = isKill and 225 or 165
                    optic.render_sprite(
                        pSprite, px, py, 255, 0, statusDuration,
                        hitmarkerEnterAnimation,
                        isKill and hitmarkerKillFadeAnimation
                               or hitmarkerNormalFadeAnimation
                    )
                    harmonySprite = nil
                end
            end
        end

        if harmonySprite then
            local isKill = sprite.name == "hitmarker_kill"
            statusDuration = isKill and 225 or 165
            optic.render_sprite(
                harmonySprite, x, y, 255, 0, statusDuration,
                hitmarkerEnterAnimation,
                isKill and hitmarkerKillFadeAnimation
                       or hitmarkerNormalFadeAnimation
            )
        end

        if HITMARKER_STATUS_ICON[sprite.name] then
            if not crosshairWidth or not crosshairHeight then
                crosshairWidth, crosshairHeight = getCurrentCrosshairSize()
            end
            renderHitmarkerStatusIcon(
                sprite.name,
                crosshairWidth,
                crosshairHeight,
                statusDuration
            )
        end
    else
        optic.render_sprite(harmonySprite, renderQueue)

        if sprite.hasAudio and configuration.enableSound then
            local harmonyAudio = harmonySounds[sprite.name]
            if harmonyAudio then
                optic.play_sound(harmonyAudio, AudioEngine)
            end
        end
    end

    if configuration.hudMessages and not sprite.name:find("hitmarker") then
        hud_message(sprite.message or toSentenceCase(sprite.name))
    end
end

local function scheduleCriticalFollowup(comboLevel, sourceCreatedMs)
    if not configuration.criticalHitmarker then return end

    local now = getHitmarkerNowMs()
    pendingCriticalFollowups[#pendingCriticalFollowups + 1] = {
        dueMs = now + HITMARKER_SHIELD_BREAK_CRITICAL_FOLLOWUP_MS,
        comboLevel = comboLevel or 1,
        sourceCreatedMs = sourceCreatedMs or now
    }

end

local function processCriticalFollowups(now)
    if not configuration.hitmarker or #pendingCriticalFollowups == 0 then
        return
    end

    local writeIndex = 1
    local followupCount = #pendingCriticalFollowups

    for readIndex = 1, followupCount do
        local followup = pendingCriticalFollowups[readIndex]
        if now >= followup.dueMs then
            medal(sprites.hitmarkerCritical, followup.comboLevel)
        else
            if writeIndex ~= readIndex then
                pendingCriticalFollowups[writeIndex] = followup
            end
            writeIndex = writeIndex + 1
        end
    end

    for index = followupCount, writeIndex, -1 do
        pendingCriticalFollowups[index] = nil
    end
end

function OnTick()

    if configuration.damageNumbers or
       configuration.criticalHitmarker or
       configuration.shieldHitmarker or
       configuration.shieldBreakHitmarker or
       configuration.vehicleHitmarker or
       configuration.headshotMedal or
       configuration.nativeDamageTelemetry then
        pollDamageTracker()
    end

    local now = getHitmarkerNowMs()
    processDamageNumberDisplay(now)
    processCriticalFollowups(now)

    if not configuration.hitmarker or #pendingNormalHitmarkers == 0 then
        return
    end

    local writeIndex = 1
    local pendingCount = #pendingNormalHitmarkers

    for readIndex = 1, pendingCount do
        local pending = pendingNormalHitmarkers[readIndex]
        if now >= pending.dueMs then
            local kind = pending.hitmarkerKind or
                         (pending.critical and "critical" or "normal")
            local selectedSprite = getHitmarkerSpriteForKind(kind)

            medal(selectedSprite, pending.comboLevel)

            if kind == "shield_broken" and
               pending.followupCritical == true and
               configuration.criticalHitmarker then
                scheduleCriticalFollowup(
                    pending.comboLevel,
                    pending.createdMs
                )
            end
        else
            if writeIndex ~= readIndex then
                pendingNormalHitmarkers[writeIndex] = pending
            end
            writeIndex = writeIndex + 1
        end
    end

    for index = pendingCount, writeIndex, -1 do
        pendingNormalHitmarkers[index] = nil
    end
end

function OnMultiplayerSound(soundEventName)

    if (soundEventName == soundsEvents.hitmarker) then
        if configuration.enableSound then
            local hitSound = harmonySounds.hit
            local hitEngine = harmonySounds.__hitAudioEngine
            if hitSound and hitEngine then
                pcall(optic.play_sound, hitSound, hitEngine, true)
            end
        end

        if (configuration.hitmarker) then
            if shouldSuppressPostKillWhiteHitmarker() then
            else
                local comboLevel =
                    registerHitmarkerHit()

                local hitWeaponTag = getCurrentWeaponTag()
                if hitWeaponTag and hitWeaponTag.path then
                    damageTracker.lastHitWeaponPath = hitWeaponTag.path
                end

                local pending = queueNormalHitmarker(comboLevel)
                applyProvisionalNativeStatusToPending(pending)
            end
        end
    end

    if (soundEventName:find("kill") or soundEventName:find("running")) then
        return false
    end
    return true
end

local function isPreviousMedalKillVariation()
    local lastMedal = medalsQueue[#medalsQueue]
    if (lastMedal and lastMedal:find("kill") and lastMedal ~= "normal_kill") then
        medalsQueue[#medalsQueue] = nil
        return true
    end
    return false
end

function OnMultiplayerEvent(eventName, localId, killerId, victimId)
    if eventName == events.localKilledPlayer then

        if configuration.damageNumbers or
           configuration.headshotMedal or
           configuration.criticalHitmarker or
           configuration.shieldHitmarker or
           configuration.shieldBreakHitmarker or
           configuration.vehicleHitmarker then
            pollDamageTracker()
        end

        renderKillDamageForVictim(victimId)
        local headshotEvent = nil
        if configuration.headshotMedal then
            local nativeReady = refreshNativeDamageProbeState(false)
            headshotEvent = consumeRecentHeadshotEvent(victimId, nativeReady)

            if not headshotEvent and nativeReady then

                headshotEvent = consumeNativeHeadshotForKill(victimId)
            elseif not headshotEvent then

                headshotEvent =
                    buildHeadshotKillFallback(victimId) or
                    buildLooseHeadshotKillFallback()
            end
        end

        local player = blam.biped(get_dynamic_player())
        local victim = blam.biped(victimId)
        if player then
            if configuration.headshotMedal and headshotEvent then
                medal(sprites.headShot)
            end

            local tag = getCurrentWeaponTag()
            if tag and blam.isNull(player.vehicleObjectId) then
                if tag.path:find("sniper") then

                    if blam.isNull(player.zoomLevel) and player.weaponPTH then
                        medal(sprites.snapshot)
                    end
                elseif tag.path:find("rocket") then
                    medal(sprites.rocketKill)
                elseif tag.path:find("needler") then
                    medal(sprites.supercombine)
                end
            end
            local localPlayer = blam.player(get_player())
            local allServerKills = 0
            for playerIndex = 0, 15 do
                local playerData = blam.player(get_player(playerIndex))
                if (playerData and playerData.index ~= localPlayer.index) then
                    allServerKills = allServerKills + playerData.kills
                end
            end
            if (allServerKills == 0 and localPlayer.kills == 1) then
                medal(sprites.firstStrike)
            end
            if player.health <= 0.25 then
                medal(sprites.closeCall)
            end
            if not headshotEvent and (not isPreviousMedalKillVariation()) then
                medal(sprites.kill)
            end
            if (configuration.hitmarker) then
                cancelPendingNormalHitmarkerForKill()

                local killComboLevel =
                    prepareHitmarkerKillCombo()

                medal(
                    sprites.hitmarkerKill,
                    killComboLevel
                )

                resetHitmarkerCombo()
            end

            if localId == killerId then
                playerData.killingSpreeCount = playerData.killingSpreeCount + 1

                if (playerData.killingSpreeCount == 5) then
                    medal(sprites.killingSpree)
                elseif (playerData.killingSpreeCount == 10) then
                    medal(sprites.killingFrenzy)
                elseif (playerData.killingSpreeCount == 15) then
                    medal(sprites.runningRiot)
                elseif (playerData.killingSpreeCount == 20) then
                    medal(sprites.rampage)
                elseif (playerData.killingSpreeCount == 25) then
                    medal(sprites.untouchable)
                elseif (playerData.killingSpreeCount == 30) then
                    medal(sprites.invincible)
                elseif (playerData.killingSpreeCount == 35) then
                    medal(sprites.inconceivable)
                elseif (playerData.killingSpreeCount == 40) then
                    medal(sprites.unfriggenbelievable)
                end

                if (playerData.dyingSpreeCount <= -3) then
                    playerData.dyingSpreeCount = 0
                    medal(sprites.comebackKill)
                end

                if not playerData.multiKillTimestamp then
                    playerData.multiKillTimestamp = harmony.time.set_timestamp()
                    playerData.multiKillCount = 1
                else
                    playerData.multiKillCount = playerData.multiKillCount + 1

                    local timeSinceLastMultiKill = harmony.time.get_elapsed_milliseconds(playerData.multiKillTimestamp)
                    if timeSinceLastMultiKill < 4500 then
                        if (playerData.multiKillCount == 2) then
                            medal(sprites.doubleKill)
                        elseif (playerData.multiKillCount == 3) then
                            medal(sprites.tripleKill)
                        elseif (playerData.multiKillCount == 4) then
                            medal(sprites.overkill)
                        elseif (playerData.multiKillCount == 5) then
                            medal(sprites.killtacular)
                        elseif (playerData.multiKillCount == 6) then
                            medal(sprites.killtrocity)
                        elseif (playerData.multiKillCount == 7) then
                            medal(sprites.killimanjaro)
                        elseif (playerData.multiKillCount == 8) then
                            medal(sprites.killtastrophe)
                        elseif (playerData.multiKillCount == 9) then
                            medal(sprites.killpocalypse)
                        elseif (playerData.multiKillCount == 10) then
                            medal(sprites.killionaire)
                        end

                        if(playerData.multiKillCount < 10) then
                            playerData.multiKillTimestamp = harmony.time.set_timestamp()
                        else
                            playerData.multiKillTimestamp = nil
                            playerData.multiKillCount = 0
                        end
                    else
                        playerData.multiKillTimestamp = harmony.time.set_timestamp()
                        playerData.multiKillCount = 1
                    end
                end
            end
        else
            medal(sprites.fromTheGrave)
        end
    end

    if eventName == events.localCtfScore then
        playerData.flagCaptures = playerData.flagCaptures + 1
        medal(sprites.flagCaptured)
        if (playerData.flagCaptures == 2) then
            medal(sprites.flagRunner)
        elseif (playerData.flagCaptures == 3) then
            medal(sprites.flagChampion)
        end
    end

    -- Killing-spree deaths must match the local player by Halo datum/slot,
    -- not only by the full 32-bit ID (whose generation/salt can differ).
    if deathEvents[eventName] and sameHaloDatumId(localId, victimId) then
        playerData.killingSpreeCount = 0
    end

    if localId == victimId then
        if deathEvents[eventName] then
            playerData.dyingSpreeCount = playerData.dyingSpreeCount - 1
            playerData.multiKillCount = 0
            playerData.multiKillTimestamp = nil
        end

        if configuration.enableSound then
            local sound
            if eventName == events.suicide then
                sound = harmonySounds.suicide
            elseif eventName == events.betrayed then
                sound = harmonySounds.betrayal
            end

            local eventEngine = harmonySounds.__eventAudioEngine
            if sound and eventEngine then
                pcall(optic.play_sound, sound, eventEngine, true)
            end
        end
    end

    if eventName == events.localDoubleKill or
       eventName == events.localTripleKill or
       eventName == events.localKilltacular or
       eventName == events.localKillingSpree or
       eventName == events.localRunningRiot then
        return false
    end

    return true
end

local function resetOpticColorCaches()
    proceduralHitmarkerCache = util.newHitmarkerCache()
    proceduralHitFlashCache = util.newHitmarkerCache()
    proceduralHitmarkerFxCache = util.newHitmarkerCache()
    damageNumberSpriteCache = {}
end

local function applyConfiguredVolume()
    local gain = configuration.volume
    local engines = {
        AudioEngine,
        harmonySounds.__eventAudioEngine,
        harmonySounds.__hitAudioEngine
    }
    for i = 1, #engines do
        if engines[i] then optic.set_audio_engine_gain(engines[i], gain) end
    end
end

local COLOR_COMMAND_TARGETS = {
    hit = {key="hitmarkerNormalColor", label="Hitmarker normal", fallback=1},
    normal = {key="hitmarkerNormalColor", label="Hitmarker normal", fallback=1},
    hit_normal = {key="hitmarkerNormalColor", label="Hitmarker normal", fallback=1},
    critical = {key="hitmarkerCriticalColor", label="Hitmarker critical", fallback=2},
    crit = {key="hitmarkerCriticalColor", label="Hitmarker critical", fallback=2},
    hit_critical = {key="hitmarkerCriticalColor", label="Hitmarker critical", fallback=2},
    damage = {key="damageNormalColor", label="Damage normal", fallback=1},
    dmg = {key="damageNormalColor", label="Damage normal", fallback=1},
    damage_normal = {key="damageNormalColor", label="Damage normal", fallback=1},
    damage_critical = {key="damageCriticalColor", label="Damage critical", fallback=2},
    dmgcrit = {key="damageCriticalColor", label="Damage critical", fallback=2},
    dmg_critical = {key="damageCriticalColor", label="Damage critical", fallback=2}
}

local function printOpticColorPalette()
    console_out("=== OpticCompat: 20 colores ===")
    for _, color in ipairs(OPTIC_COLOR_PALETTE) do
        console_out(string.format("%02d  %-10s %s  RGB(%d,%d,%d)",
            color.id, color.name, color.hex, color.r, color.g, color.b))
    end
    console_out("Usage: ocolor <hit-critical-damage-damage_critical> <1-20|nombre>")
end

local function printCurrentOpticColors()
    local entries = {
        {"Hitmarker normal", "hitmarkerNormalColor", 1},
        {"Hitmarker critical", "hitmarkerCriticalColor", 2},
        {"Damage normal", "damageNormalColor", 1},
        {"Damage critical", "damageCriticalColor", 2}
    }
    console_out("=== Colores actuales ===")
    for _, entry in ipairs(entries) do
        local c = configuredOpticColor(entry[2], entry[3])
        console_out(entry[1] .. ": " .. c.id .. " " .. c.name .. " " .. c.hex)
    end
end

local function handleOpticColorCommand(command)
    local params = util.split(command, " ")
    local targetName = tostring(params[2] or ""):lower()

    if targetName == "" or targetName == "show" then
        printCurrentOpticColors()
        return false
    end

    local target = COLOR_COMMAND_TARGETS[targetName]
    if not target then
        console_out("Target invalido. Usa: hit, critical, damage, damage_critical")
        return false
    end

    if not params[3] then
        local current = configuredOpticColor(target.key, target.fallback)
        console_out(target.label .. ": " .. current.id .. " " .. current.name .. " " .. current.hex)
        return false
    end

    local requested = tostring(params[3]):lower()
    local color = resolveOpticPaletteColor(requested, nil)
    local numeric = tonumber(requested)
    local named = OPTIC_COLOR_BY_NAME[requested:gsub("[%s%-]", "_")] or
                  OPTIC_COLOR_BY_NAME[requested:gsub("[%s_%-]", "")]

    if not ((numeric and numeric >= 1 and numeric <= #OPTIC_COLOR_PALETTE) or named) then
        console_out("Color invalido. Escribe ocolors para ver los 20 colores.")
        return false
    end

    configuration[target.key] = color.id
    saveOpticConfiguration()
    resetOpticColorCaches()
    console_out(target.label .. " -> " .. color.id .. " " .. color.name .. " " .. color.hex)
    return false
end

function OnCommand(command)
    if command == "optic_colors" or command == "ocolors" then
        printOpticColorPalette()
        return false
    elseif command == "optic_color" or command == "ocolor" or
           command:find("optic_color ", 1, true) == 1 or
           command:find("ocolor ", 1, true) == 1 then
        return handleOpticColorCommand(command)

    elseif (command == "optic_version" or command == "oversion") then
        console_out(opticVersion)
        return false
    elseif (command == "optic_reload" or command == "oreload") then
        loadOpticConfiguration()
        return false
    elseif (command:find "optic_style") then
        local params = util.split(command, " ")
        local style = params[2]
        if (style and directory_exists(style)) then
            configuration.style = style
            console_out("Success, optic style loaded")
            saveOpticConfiguration()
            loadOpticConfiguration()
            return false
        end
        console_out("Error at loading optic style")
        return false
    elseif command:find "optic_volume" or command:find "ovolume" then
        local params = util.split(command, " ")
        local volume = tonumber(params[2]) or 1
        configuration.volume = volume
        applyConfiguredVolume()
        console_out("Optic volume set to " .. volume)
        saveOpticConfiguration()
        return false
    elseif command == "optic_sound" or command == "osound" then
        configuration.enableSound = not configuration.enableSound
        console_out("Optic sound: " .. tostring(configuration.enableSound))
        saveOpticConfiguration()
        return false
    end
end

function OnMapLoad()
    weaponHudTagCache = {}
    haloReticleAlphaCache = {}
    hitModelNameCache = {}
    hitDamageEffectHeadshotCache = {}
    proceduralHitmarkerCache = util.newHitmarkerCache()
    proceduralHitFlashCache = util.newHitmarkerCache()
    proceduralHitmarkerFxCache = util.newHitmarkerCache()
    damageNumberSpriteCache = {}
    damageNumberNextQueue = 1
    nativeDamageTracker.available = nil
    nativeDamageTracker.recent = {}

    resetHitmarkerCombo()
    hitmarkerComboState.suppressWhiteUntilMs = nil
    pendingNormalHitmarkers = {}
    pendingCriticalFollowups = {}
    resetDamageTracker()

    loadOpticConfiguration()
    refreshNativeDamageProbeState(true)
    if (not medalsLoaded) then
        console_out("Error, medals were not loaded properly!")
    end

    playerData = deepcopy(defaultPlayerData)
end

set_callback("command", "OnCommand")
set_callback("map load", "OnMapLoad")
set_callback("tick", "OnTick")

OnScriptLoad()
