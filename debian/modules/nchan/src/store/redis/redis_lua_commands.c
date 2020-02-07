#include "redis_lua_commands.h"

redis_lua_scripts_t redis_lua_scripts = {
  {"add_fakesub", "fd2ee193a2518982e607f99623c8200849b21098",
   "--input:  keys: [], values: [namespace, channel_id, number, time]\n"
   "--output: current_fake_subscribers\n"
   "  \n"
   "redis.call('echo', ' ####### FAKESUBS ####### ')\n"
   "local ns=ARGV[1]\n"
   "local id=ARGV[2]\n"
   "local num=tonumber(ARGV[3])\n"
   "local time = tonumber(ARGV[4])\n"
   "if num==nil then\n"
   "  return {err=\"fakesub number not given\"}\n"
   "end\n"
   "\n"
   "local chan_key = ('%s{channel:%s}'):format(ns, id)\n"
   "\n"
   "local res = redis.pcall('EXISTS', chan_key)\n"
   "if type(res) == \"table\" and res[\"err\"] then\n"
   "  return {err = (\"CLUSTER KEYSLOT ERROR. %i %s\"):format(num, id)}\n"
   "end\n"
   "\n"
   "local exists = res == 1\n"
   "\n"
   "local cur = 0\n"
   "\n"
   "if exists or (not exists and num > 0) then\n"
   "  cur = redis.call('HINCRBY', chan_key, 'fake_subscribers', num)\n"
   "  if time then\n"
   "    redis.call('HSET', chan_key, 'last_seen_fake_subscriber', time)\n"
   "  end\n"
   "  if not exists then\n"
   "    redis.call('EXPIRE', chan_key, 5) --something small\n"
   "  end\n"
   "end\n"
   "\n"
   "return cur\n"},

  {"channel_keepalive", "7e213d514486887425875dc9835564edfe14e677",
   "--input:  keys: [], values: [namespace, channel_id, ttl]\n"
   "-- ttl is for when there are no messages but at least 1 subscriber.\n"
   "--output: seconds until next keepalive is expected, or -1 for \"let it disappear\"\n"
   "redis.call('ECHO', ' ####### CHANNEL KEEPALIVE ####### ')\n"
   "local ns=ARGV[1]\n"
   "local id=ARGV[2]\n"
   "local ttl=tonumber(ARGV[3])\n"
   "if not ttl then\n"
   "  return {err=\"Invalid channel keepalive TTL (2nd arg)\"}\n"
   "end\n"
   "\n"
   "local random_safe_next_ttl = function(ttl)\n"
   "  return math.floor(ttl/2 + ttl/2.1 * math.random())\n"
   "end\n"
   "local ch = ('%s{channel:%s}'):format(ns, id)\n"
   "local key={\n"
   "  channel=   ch, --hash\n"
   "  messages=  ch..':messages', --list\n"
   "}\n"
   "  \n"
   "local subs_count = tonumber(redis.call('HGET', key.channel, \"subscribers\")) or 0\n"
   "local msgs_count = tonumber(redis.call('LLEN', key.messages)) or 0\n"
   "local actual_ttl = tonumber(redis.call('TTL',  key.channel))\n"
   "\n"
   "if subs_count > 0 then\n"
   "  if msgs_count > 0 and actual_ttl > ttl then\n"
   "    return random_safe_next_ttl(actual_ttl)\n"
   "  end\n"
   "  --refresh ttl\n"
   "  redis.call('expire', key.channel, ttl);\n"
   "  redis.call('expire', key.messages, ttl);\n"
   "  return random_safe_next_ttl(ttl)\n"
   "else\n"
   "  return -1\n"
   "end\n"},

  {"delete", "181906aceef3012cc1f94c272bd00d9050f2916f",
   "--input: keys: [],  values: [ namespace, channel_id ]\n"
   "--output: channel_hash {ttl, time_last_seen, subscribers, messages} or nil\n"
   "-- delete this channel and all its messages\n"
   "local ns = ARGV[1]\n"
   "local id = ARGV[2]\n"
   "local ch = ('%s{channel:%s}'):format(ns, id)\n"
   "local key_msg=    ch..':msg:%s' --not finished yet\n"
   "local key_channel=ch\n"
   "local messages=   ch..':messages'\n"
   "local subscribers=ch..':subscribers'\n"
   "local pubsub=     ch..':pubsub'\n"
   "\n"
   "redis.call('echo', ' ####### DELETE #######')\n"
   "local num_messages = 0\n"
   "--delete all the messages right now mister!\n"
   "local msg\n"
   "while true do\n"
   "  msg = redis.call('LPOP', messages)\n"
   "  if msg then\n"
   "    num_messages = num_messages + 1\n"
   "    redis.call('DEL', key_msg:format(msg))\n"
   "  else\n"
   "    break\n"
   "  end\n"
   "end\n"
   "\n"
   "local del_msgpack =cmsgpack.pack({\"alert\", \"delete channel\", id})\n"
   "for k,channel_key in pairs(redis.call('SMEMBERS', subscribers)) do\n"
   "  redis.call('PUBLISH', channel_key, del_msgpack)\n"
   "end\n"
   "\n"
   "local tohash=function(arr)\n"
   "  if type(arr)~=\"table\" then\n"
   "    return nil\n"
   "  end\n"
   "  local h = {}\n"
   "  local k=nil\n"
   "  for i, v in ipairs(arr) do\n"
   "    if k == nil then\n"
   "      k=v\n"
   "    else\n"
   "      h[k]=v; k=nil\n"
   "    end\n"
   "  end\n"
   "  return h\n"
   "end\n"
   "\n"
   "local channel = nil\n"
   "if redis.call('EXISTS', key_channel) ~= 0 then\n"
   "  channel = tohash(redis.call('hgetall', key_channel))\n"
   "  --leave some crumbs behind showing this channel was just deleted\n"
   "  redis.call('setex', ch..\":deleted\", 5, 1)  \n"
   "end\n"
   "\n"
   "redis.call('DEL', key_channel, messages, subscribers)\n"
   "\n"
   "if redis.call('PUBSUB','NUMSUB', pubsub)[2] > 0 then\n"
   "  redis.call('PUBLISH', pubsub, del_msgpack)\n"
   "end\n"
   "\n"
   "if channel then\n"
   "  return {\n"
   "    tonumber(channel.ttl) or 0,\n"
   "    tonumber(channel.last_seen_fake_subscriber) or 0,\n"
   "    tonumber(channel.fake_subscribers or channel.subscribers) or 0,\n"
   "    channel.current_message or \"\",\n"
   "    tonumber(num_messages)\n"
   "  }\n"
   "else\n"
   "  return nil\n"
   "end\n"},

  {"find_channel", "d027f1bc80eb1afc33f909759dfb7428cf1f9063",
   "--input: keys: [],  values: [ namespace, channel_id ]\n"
   "--output: channel_hash {ttl, time_last_seen, subscribers, last_channel_id, messages} or nil\n"
   "-- finds and return the info hash of a channel, or nil of channel not found\n"
   "local ns = ARGV[1]\n"
   "local id = ARGV[2]\n"
   "local channel_key = ('%s{channel:%s}'):format(ns, id)\n"
   "local messages_key = channel_key..':messages'\n"
   "\n"
   "redis.call('echo', ' #######  FIND_CHANNEL ######## ')\n"
   "\n"
   "--check old entries\n"
   "local oldestmsg=function(list_key, old_fmt)\n"
   "  local old, oldkey\n"
   "  local n, del=0,0\n"
   "  while true do\n"
   "    n=n+1\n"
   "    old=redis.call('lindex', list_key, -1)\n"
   "    if old then\n"
   "      oldkey=old_fmt:format(old)\n"
   "      local ex=redis.call('exists', oldkey)\n"
   "      if ex==1 then\n"
   "        return oldkey\n"
   "      else\n"
   "        redis.call('rpop', list_key)\n"
   "        del=del+1\n"
   "      end\n"
   "    else\n"
   "      break\n"
   "    end\n"
   "  end\n"
   "end\n"
   "\n"
   "local tohash=function(arr)\n"
   "  if type(arr)~=\"table\" then\n"
   "    return nil\n"
   "  end\n"
   "  local h = {}\n"
   "  local k=nil\n"
   "  for i, v in ipairs(arr) do\n"
   "    if k == nil then\n"
   "      k=v\n"
   "    else\n"
   "      --dbg(k..\"=\"..v)\n"
   "      h[k]=v; k=nil\n"
   "    end\n"
   "  end\n"
   "  return h\n"
   "end\n"
   "\n"
   "if redis.call('EXISTS', channel_key) ~= 0 then\n"
   "  local ch = tohash(redis.call('hgetall', channel_key))\n"
   "    \n"
   "  local msgs_count\n"
   "  if redis.call(\"TYPE\", messages_key)['ok'] == 'list' then\n"
   "    oldestmsg(messages_key, channel_key ..':msg:%s')\n"
   "    msgs_count = tonumber(redis.call('llen', messages_key))\n"
   "  else\n"
   "    msgs_count = 0\n"
   "  end\n"
   "  \n"
   "  return {\n"
   "    tonumber(ch.ttl) or 0,\n"
   "    tonumber(ch.last_seen_fake_subscriber) or 0,\n"
   "    tonumber(ch.fake_subscribers or ch.subscribers) or 0,\n"
   "    ch.current_message or \"\",\n"
   "    msgs_count\n"
   "  }\n"
   "else\n"
   "  return nil\n"
   "end\n"},

  {"get_message", "fb9c46d33b3798a11d4eca6e0f7a3f92beba8685",
   "--input:  keys: [], values: [namespace, channel_id, msg_time, msg_tag, no_msgid_order, create_channel_ttl]\n"
   "--output: result_code, msg_ttl, msg_time, msg_tag, prev_msg_time, prev_msg_tag, message, content_type, eventsource_event, compression_type, channel_subscriber_count\n"
   "-- no_msgid_order: 'FILO' for oldest message, 'FIFO' for most recent\n"
   "-- create_channel_ttl - make new channel if it's absent, with ttl set to this. 0 to disable.\n"
   "-- result_code can be: 200 - ok, 404 - not found, 410 - gone, 418 - not yet available\n"
   "local ns, id, time, tag, subscribe_if_current = ARGV[1], ARGV[2], tonumber(ARGV[3]), tonumber(ARGV[4])\n"
   "local no_msgid_order=ARGV[5]\n"
   "local create_channel_ttl=tonumber(ARGV[6]) or 0\n"
   "local msg_id\n"
   "if time and time ~= 0 and tag then\n"
   "  msg_id=(\"%s:%s\"):format(time, tag)\n"
   "end\n"
   "\n"
   "if redis.replicate_commands then\n"
   "  redis.replicate_commands()\n"
   "end\n"
   "\n"
   "-- This script has gotten big and ugly, but there are a few good reasons \n"
   "-- to keep it big and ugly. It needs to do a lot of stuff atomically, and \n"
   "-- redis doesn't do includes. It could be generated pre-insertion into redis, \n"
   "-- but then error messages become less useful, complicating debugging. If you \n"
   "-- have a solution to this, please help.\n"
   "local ch=('%s{channel:%s}'):format(ns, id)\n"
   "local msgkey_fmt=ch..':msg:%s'\n"
   "local key={\n"
   "  next_message= msgkey_fmt, --hash\n"
   "  message=      msgkey_fmt, --hash\n"
   "  channel=      ch, --hash\n"
   "  messages=     ch..':messages', --list\n"
   "--  pubsub=       ch..':subscribers:', --set\n"
   "}\n"
   "\n"
   "--local dbg = function(...) redis.call('echo', table.concat({...})); end\n"
   "\n"
   "redis.call('echo', ' #######  GET_MESSAGE ######## ')\n"
   "\n"
   "local oldestmsg=function(list_key, old_fmt)\n"
   "  local old, oldkey\n"
   "  local n, del=0,0\n"
   "  while true do\n"
   "    n=n+1\n"
   "    old=redis.call('lindex', list_key, -1)\n"
   "    if old then\n"
   "      oldkey=old_fmt:format(old)\n"
   "      local ex=redis.call('exists', oldkey)\n"
   "      if ex==1 then\n"
   "        return oldkey\n"
   "      else\n"
   "        redis.call('rpop', list_key)\n"
   "        del=del+1\n"
   "      end \n"
   "    else\n"
   "      --dbg(list_key, \" is empty\")\n"
   "      break\n"
   "    end\n"
   "  end\n"
   "end\n"
   "\n"
   "local tohash=function(arr)\n"
   "  if type(arr)~=\"table\" then\n"
   "    return nil\n"
   "  end\n"
   "  local h = {}\n"
   "  local k=nil\n"
   "  for i, v in ipairs(arr) do\n"
   "    if k == nil then\n"
   "      k=v\n"
   "    else\n"
   "      --dbg(k..\"=\"..v)\n"
   "      h[k]=v; k=nil\n"
   "    end\n"
   "  end\n"
   "  return h\n"
   "end\n"
   "\n"
   "if no_msgid_order ~= 'FIFO' then\n"
   "  no_msgid_order = 'FILO'\n"
   "end\n"
   "\n"
   "local channel = tohash(redis.call('HGETALL', key.channel))\n"
   "local new_channel = false\n"
   "if next(channel) == nil then\n"
   "  if create_channel_ttl==0 then\n"
   "    return {404, nil}\n"
   "  end\n"
   "  redis.call('HSET', key.channel, 'time', time)\n"
   "  redis.call('EXPIRE', key.channel, create_channel_ttl)\n"
   "  channel = {time=time}\n"
   "  new_channel = true\n"
   "end\n"
   "\n"
   "local subs_count = tonumber(channel.subscribers)\n"
   "\n"
   "if msg_id==nil then\n"
   "  local found_msg_key\n"
   "  if new_channel then\n"
   "    --dbg(\"new channel\")\n"
   "    return {418, \"\", \"\", \"\", \"\", subs_count}\n"
   "  else\n"
   "    --dbg(\"no msg id given, ord=\"..no_msgid_order)\n"
   "    \n"
   "    if no_msgid_order == 'FIFO' then --most recent message\n"
   "      --dbg(\"get most recent\")\n"
   "      found_msg_key=channel.current_message\n"
   "    elseif no_msgid_order == 'FILO' then --oldest message\n"
   "      --dbg(\"get oldest\")\n"
   "      found_msg_key=oldestmsg(key.messages, msgkey_fmt)\n"
   "    end\n"
   "    \n"
   "    if found_msg_key == nil then\n"
   "      --we await a message\n"
   "      return {418, \"\", \"\", \"\", \"\", subs_count}\n"
   "    else\n"
   "      local msg=tohash(redis.call('HGETALL', found_msg_key))\n"
   "      if not next(msg) then --empty\n"
   "        return {404, \"\", \"\", \"\", \"\", subs_count}\n"
   "      else\n"
   "        --dbg((\"found msg %s:%s  after %s:%s\"):format(tostring(msg.time), tostring(msg.tag), tostring(time), tostring(tag)))\n"
   "        local ttl = redis.call('TTL', found_msg_key)\n"
   "        return {200, ttl, tonumber(msg.time) or \"\", tonumber(msg.tag) or \"\", tonumber(msg.prev_time) or \"\", tonumber(msg.prev_tag) or \"\", msg.data or \"\", msg.content_type or \"\", msg.eventsource_event or \"\", tonumber(msg.compression or 0), subs_count}\n"
   "      end\n"
   "    end\n"
   "  end\n"
   "else\n"
   "  if msg_id and channel.current_message == msg_id\n"
   "   or not channel.current_message then\n"
   "    return {418, \"\", \"\", \"\", \"\", subs_count}\n"
   "  end\n"
   "\n"
   "  key.message=key.message:format(msg_id)\n"
   "  local msg=tohash(redis.call('HGETALL', key.message))\n"
   "\n"
   "  if next(msg) == nil then -- no such message. it might've expired, or maybe it was never there\n"
   "    --dbg(\"MESSAGE NOT FOUND\")\n"
   "    return {404, nil}\n"
   "  end\n"
   "\n"
   "  local next_msg, next_msgtime, next_msgtag\n"
   "  if not msg.next then --this should have been taken care of by the channel.current_message check\n"
   "    --dbg(\"NEXT MESSAGE KEY NOT PRESENT. ERROR, ERROR!\")\n"
   "    return {404, nil}\n"
   "  else\n"
   "    --dbg(\"NEXT MESSAGE KEY PRESENT: \" .. msg.next)\n"
   "    key.next_message=key.next_message:format(msg.next)\n"
   "    if redis.call('EXISTS', key.next_message)~=0 then\n"
   "      local ntime, ntag, prev_time, prev_tag, ndata, ncontenttype, neventsource_event, ncompression=unpack(redis.call('HMGET', key.next_message, 'time', 'tag', 'prev_time', 'prev_tag', 'data', 'content_type', 'eventsource_event', 'compression'))\n"
   "      local ttl = redis.call('TTL', key.next_message)\n"
   "      --dbg((\"found msg2 %i:%i  after %i:%i\"):format(ntime, ntag, time, tag))\n"
   "      return {200, ttl, tonumber(ntime) or \"\", tonumber(ntag) or \"\", tonumber(prev_time) or \"\", tonumber(prev_tag) or \"\", ndata or \"\", ncontenttype or \"\", neventsource_event or \"\", ncompression or 0, subs_count}\n"
   "    else\n"
   "      --dbg(\"NEXT MESSAGE NOT FOUND\")\n"
   "      return {404, nil}\n"
   "    end\n"
   "  end\n"
   "end\n"},

  {"get_message_from_key", "304efcd42590f99e0016686572530defd3de1383",
   "--input:  keys: [message_key], values: []\n"
   "--output: msg_ttl, msg_time, msg_tag, prev_msg_time, prev_msg_tag, message, content_type, eventsource_event, compression, channel_subscriber_count\n"
   "local key = KEYS[1]\n"
   "\n"
   "local ttl = redis.call('TTL', key)\n"
   "local time, tag, prev_time, prev_tag, data, content_type, es_event, compression = unpack(redis.call('HMGET', key, 'time', 'tag', 'prev_time', 'prev_tag', 'data', 'content_type', 'eventsource_event', 'compression'))\n"
   "\n"
   "return {ttl, time, tag, prev_time or 0, prev_tag or 0, data or \"\", content_type or \"\", es_event or \"\", tonumber(compression or 0)}\n"},

  {"publish", "be28281114137d8e85a25a913c50bed2e93d0c7d",
   "--input:  keys: [], values: [namespace, channel_id, time, message, content_type, eventsource_event, compression_setting, msg_ttl, max_msg_buf_size, pubsub_msgpacked_size_cutoff, optimize_target]\n"
   "--output: channel_hash {ttl, time_last_subscriber_seen, subscribers, last_message_id, messages}, channel_created_just_now?\n"
   "\n"
   "local ns, id=ARGV[1], ARGV[2]\n"
   "\n"
   "local msg = {}\n"
   "\n"
   "local store_at_most_n_messages = tonumber(ARGV[9])\n"
   "if store_at_most_n_messages == nil or store_at_most_n_messages == \"\" then\n"
   "  return {err=\"Argument 9, max_msg_buf_size, can't be empty\"}\n"
   "end\n"
   "if store_at_most_n_messages == 0 then\n"
   "  msg.unbuffered = 1\n"
   "end\n"
   "\n"
   "local msgpacked_pubsub_cutoff = tonumber(ARGV[10])\n"
   "\n"
   "local optimize_target = tonumber(ARGV[11]) == 2 and \"bandwidth\" or \"cpu\"\n"
   "\n"
   "local time\n"
   "if optimize_target == \"cpu\" and redis.replicate_commands then\n"
   "  -- we're on redis >= 3.2. We can use We can use 'script effects replication' to allow\n"
   "  -- writing nondeterministic command values like TIME.\n"
   "  -- That's exactly what we want to do, use Redis' TIME rather than the given time from Nginx\n"
   "  -- Also, it's more efficient to replicate just the commands in this case rather than run the whole script\n"
   "  redis.replicate_commands()\n"
   "  time = tonumber(redis.call('TIME')[1])\n"
   "else\n"
   "  --fallback to the provided time\n"
   "  time = tonumber(ARGV[3])\n"
   "end\n"
   "\n"
   "msg.id=nil\n"
   "msg.data= ARGV[4]\n"
   "msg.content_type=ARGV[5]\n"
   "msg.eventsource_event=ARGV[6]\n"
   "msg.compression=tonumber(ARGV[7])\n"
   "msg.ttl= tonumber(ARGV[8])\n"
   "msg.time= time\n"
   "msg.tag= 0\n"
   "\n"
   "if msg.ttl == 0 then\n"
   "  msg.ttl = 126144000 --4 years\n"
   "end\n"
   "\n"
   "--[[local dbg = function(...)\n"
   "  local arg = {...}\n"
   "  for i = 1, #arg do\n"
   "    arg[i]=tostring(arg[i])\n"
   "  end\n"
   "  redis.call('echo', table.concat(arg, \", \"))\n"
   "end]]\n"
   "\n"
   "if type(msg.content_type)=='string' and msg.content_type:find(':') then\n"
   "  return {err='Message content-type cannot contain \":\" character.'}\n"
   "end\n"
   "\n"
   "redis.call('echo', ' #######  PUBLISH   ######## ')\n"
   "\n"
   "-- sets all fields for a hash from a dictionary\n"
   "local hmset = function (key, dict)\n"
   "  if next(dict) == nil then return nil end\n"
   "  local bulk = {}\n"
   "  for k, v in pairs(dict) do\n"
   "    table.insert(bulk, k)\n"
   "    table.insert(bulk, v)\n"
   "  end\n"
   "  return redis.call('HMSET', key, unpack(bulk))\n"
   "end\n"
   "\n"
   "local tohash=function(arr)\n"
   "  if type(arr)~=\"table\" then\n"
   "    return nil\n"
   "  end\n"
   "  local h = {}\n"
   "  local k=nil\n"
   "  for i, v in ipairs(arr) do\n"
   "    if k == nil then\n"
   "      k=v\n"
   "    else\n"
   "      h[k]=v; k=nil\n"
   "    end\n"
   "  end\n"
   "  return h\n"
   "end\n"
   "\n"
   "local ch = ('%s{channel:%s}'):format(ns, id)\n"
   "local msg_fmt = ch..':msg:%s'\n"
   "local key={\n"
   "  last_message= msg_fmt, --not finished yet\n"
   "  message=      msg_fmt, --not finished yet\n"
   "  channel=      ch,\n"
   "  messages=     ch..':messages',\n"
   "  subscribers=  ch..':subscribers'\n"
   "}\n"
   "local channel_pubsub = ch..':pubsub'\n"
   "\n"
   "local new_channel\n"
   "local channel\n"
   "if redis.call('EXISTS', key.channel) ~= 0 then\n"
   "  channel=tohash(redis.call('HGETALL', key.channel))\n"
   "  channel.max_stored_messages = tonumber(channel.max_stored_messages)\n"
   "end\n"
   "\n"
   "if channel~=nil then\n"
   "  --dbg(\"channel present\")\n"
   "  if channel.current_message ~= nil then\n"
   "    --dbg(\"channel current_message present\")\n"
   "    key.last_message=key.last_message:format(channel.current_message, id)\n"
   "  else\n"
   "    --dbg(\"channel current_message absent\")\n"
   "    key.last_message=nil\n"
   "  end\n"
   "  new_channel=false\n"
   "else\n"
   "  --dbg(\"channel missing\")\n"
   "  channel={}\n"
   "  new_channel=true\n"
   "  key.last_message=nil\n"
   "end\n"
   "\n"
   "--set new message id\n"
   "local lastmsg, lasttime, lasttag\n"
   "if key.last_message then\n"
   "  lastmsg = redis.call('HMGET', key.last_message, 'time', 'tag')\n"
   "  lasttime, lasttag = tonumber(lastmsg[1]), tonumber(lastmsg[2])\n"
   "  --dbg(\"New message id: last_time \", lasttime, \" last_tag \", lasttag, \" msg_time \", msg.time)\n"
   "  if lasttime and tonumber(lasttime) > tonumber(msg.time) then\n"
   "    redis.log(redis.LOG_WARNING, \"Nchan: message for \" .. id .. \" arrived a little late and may be delivered out of order. Redis must be very busy, or the Nginx servers do not have their times synchronized.\")\n"
   "    msg.time = lasttime\n"
   "    time = lasttime\n"
   "  end\n"
   "  if lasttime and lasttime==msg.time then\n"
   "    msg.tag=lasttag+1\n"
   "  end\n"
   "  msg.prev_time = lasttime\n"
   "  msg.prev_tag = lasttag\n"
   "else\n"
   "  msg.prev_time = 0\n"
   "  msg.prev_tag = 0\n"
   "end\n"
   "msg.id=('%i:%i'):format(msg.time, msg.tag)\n"
   "\n"
   "key.message=key.message:format(msg.id)\n"
   "if redis.call('EXISTS', key.message) ~= 0 then\n"
   "  local hash_tostr=function(h)\n"
   "    local tt = {}\n"
   "    for k, v in pairs(h) do\n"
   "      table.insert(tt, (\"%s: %s\"):format(k, v))\n"
   "    end\n"
   "    return \"{\" .. table.concat(tt,\", \") .. \"}\"\n"
   "  end\n"
   "  local existing_msg = tohash(redis.call('HGETALL', key.message))\n"
   "  local errmsg = \"Message %s for channel %s id %s already exists. time: %s lasttime: %s lasttag: %s. dbg: channel: %s, messages_key: %s, msglist: %s, msg: %s, msg_expire: %s.\"\n"
   "  errmsg = errmsg:format(key.message, id, msg.id or \"-\", time or \"-\", lasttime or \"-\", lasttag or \"-\", hash_tostr(channel), key.messages, \"[\"..table.concat(redis.call('LRANGE', key.messages, 0, -1), \", \")..\"]\", hash_tostr(existing_msg), redis.call('TTL', key.message))\n"
   "  return {err=errmsg}\n"
   "end\n"
   "\n"
   "msg.prev=channel.current_message\n"
   "if key.last_message and redis.call('exists', key.last_message) == 1 then\n"
   "  redis.call('HSET', key.last_message, 'next', msg.id)\n"
   "end\n"
   "\n"
   "--update channel\n"
   "redis.call('HSET', key.channel, 'current_message', msg.id)\n"
   "if msg.prev then\n"
   "  redis.call('HSET', key.channel, 'prev_message', msg.prev)\n"
   "end\n"
   "if time then\n"
   "  redis.call('HSET', key.channel, 'time', time)\n"
   "end\n"
   "\n"
   "local message_len_changed = false\n"
   "if channel.max_stored_messages ~= store_at_most_n_messages then\n"
   "  channel.max_stored_messages = store_at_most_n_messages\n"
   "  message_len_changed = true\n"
   "  redis.call('HSET', key.channel, 'max_stored_messages', store_at_most_n_messages)\n"
   "  --dbg(\"channel.max_stored_messages was not set, but is now \", store_at_most_n_messages)\n"
   "end\n"
   "\n"
   "--write message\n"
   "hmset(key.message, msg)\n"
   "\n"
   "\n"
   "--check old entries\n"
   "local oldestmsg=function(list_key, old_fmt)\n"
   "  local old, oldkey\n"
   "  local n, del=0,0\n"
   "  while true do\n"
   "    n=n+1\n"
   "    old=redis.call('lindex', list_key, -1)\n"
   "    if old then\n"
   "      oldkey=old_fmt:format(old)\n"
   "      local ex=redis.call('exists', oldkey)\n"
   "      if ex==1 then\n"
   "        return oldkey\n"
   "      else\n"
   "        redis.call('rpop', list_key)\n"
   "        del=del+1\n"
   "      end\n"
   "    else\n"
   "      break\n"
   "    end\n"
   "  end\n"
   "end\n"
   "\n"
   "local max_stored_msgs = channel.max_stored_messages or -1\n"
   "\n"
   "if max_stored_msgs < 0 then --no limit\n"
   "  oldestmsg(key.messages, msg_fmt)\n"
   "  redis.call('LPUSH', key.messages, msg.id)\n"
   "elseif max_stored_msgs > 0 then\n"
   "  local stored_messages = tonumber(redis.call('LLEN', key.messages))\n"
   "  redis.call('LPUSH', key.messages, msg.id)\n"
   "  -- Reduce the message length if necessary\n"
   "  local dump_message_ids = redis.call('LRANGE', key.messages, max_stored_msgs, stored_messages);\n"
   "  if dump_message_ids then\n"
   "    for _, msgid in ipairs(dump_message_ids) do\n"
   "      redis.call('DEL', msg_fmt:format(msgid))\n"
   "    end\n"
   "  end\n"
   "  redis.call('LTRIM', key.messages, 0, max_stored_msgs - 1)\n"
   "  oldestmsg(key.messages, msg_fmt)\n"
   "end\n"
   "\n"
   "\n"
   "--set expiration times for all the things\n"
   "local channel_ttl = tonumber(redis.call('TTL',  key.channel))\n"
   "redis.call('EXPIRE', key.message, msg.ttl)\n"
   "if msg.ttl + 1 > channel_ttl then -- a little extra time for failover weirdness for 1-second TTL messages\n"
   "  redis.call('EXPIRE', key.channel, msg.ttl + 1)\n"
   "  redis.call('EXPIRE', key.messages, msg.ttl + 1)\n"
   "  redis.call('EXPIRE', key.subscribers, msg.ttl + 1)\n"
   "end\n"
   "\n"
   "--publish message\n"
   "local unpacked\n"
   "\n"
   "if msg.unbuffered or #msg.data < msgpacked_pubsub_cutoff then\n"
   "  unpacked= {\n"
   "    \"msg\",\n"
   "    msg.ttl or 0,\n"
   "    msg.time,\n"
   "    tonumber(msg.tag) or 0,\n"
   "    (msg.unbuffered and 0 or msg.prev_time) or 0,\n"
   "    (msg.unbuffered and 0 or msg.prev_tag) or 0,\n"
   "    msg.data or \"\",\n"
   "    msg.content_type or \"\",\n"
   "    msg.eventsource_event or \"\",\n"
   "    msg.compression or 0\n"
   "  }\n"
   "else\n"
   "  unpacked= {\n"
   "    \"msgkey\",\n"
   "    msg.time,\n"
   "    tonumber(msg.tag) or 0,\n"
   "    key.message\n"
   "  }\n"
   "end\n"
   "\n"
   "if message_len_changed then\n"
   "  unpacked[1] = \"max_msgs+\" .. unpacked[1]\n"
   "  table.insert(unpacked, 2, tonumber(channel.max_stored_messages))\n"
   "end\n"
   "\n"
   "local msgpacked\n"
   "\n"
   "--dbg((\"Stored message with id %i:%i => %s\"):format(msg.time, msg.tag, msg.data))\n"
   "\n"
   "--we used to publish conditionally on subscribers on the Redis pubsub channel\n"
   "--but now that we're subscribing to slaves this is not possible\n"
   "--so just PUBLISH always.\n"
   "msgpacked = cmsgpack.pack(unpacked)\n"
   "redis.call('PUBLISH', channel_pubsub, msgpacked)\n"
   "\n"
   "local num_messages = redis.call('llen', key.messages)\n"
   "\n"
   "--dbg(\"channel \", id, \" ttl: \",channel.ttl, \", subscribers: \", channel.subscribers, \"(fake: \", channel.fake_subscribers or \"nil\", \"), messages: \", num_messages)\n"
   "local ch = {\n"
   "  tonumber(channel.ttl or msg.ttl),\n"
   "  tonumber(channel.last_seen_fake_subscriber) or 0,\n"
   "  tonumber(channel.fake_subscribers or channel.subscribers) or 0,\n"
   "  msg.time and msg.time and (\"%i:%i\"):format(msg.time, msg.tag) or \"\",\n"
   "  tonumber(num_messages)\n"
   "}\n"
   "\n"
   "return {ch, new_channel}\n"},

  {"publish_status", "2f2ce1443b22c8c9cf069d5588bad4bab58d70aa",
   "--input:  keys: [], values: [namespace, channel_id, status_code]\n"
   "--output: current_subscribers\n"
   "\n"
   "redis.call('echo', ' ####### PUBLISH STATUS ####### ')\n"
   "--local dbg = function(...) redis.call('echo', table.concat({...})); end\n"
   "local ns, id=ARGV[1], ARGV[2]\n"
   "local code=tonumber(ARGV[3])\n"
   "if code==nil then\n"
   "  return {err=\"non-numeric status code given, bailing!\"}\n"
   "end\n"
   "\n"
   "local pubmsg = \"status:\"..code\n"
   "local ch = ('%s{channel:%s}'):format(ns, id)\n"
   "local subs_key = ch..':subscribers'\n"
   "local chan_key = ch\n"
   "--local channel_pubsub = ch..':pubsub'\n"
   "\n"
   "for k,channel_key in pairs(redis.call('SMEMBERS', subs_key)) do\n"
   "  --not efficient, but useful for a few short-term subscriptions\n"
   "  redis.call('PUBLISH', channel_key, pubmsg)\n"
   "end\n"
   "--clear short-term subscriber list\n"
   "redis.call('DEL', subs_key)\n"
   "--now publish to the efficient channel\n"
   "--what?... redis.call('PUBLISH', channel_pubsub, pubmsg)\n"
   "return redis.call('HGET', chan_key, 'subscribers') or 0\n"},

  {"rsck", "2fca046fa783d6cc25e493c993c407e59998e6e8",
   "--redis-store consistency check\n"
   "local ns = ARGV[1]\n"
   "if ns and #ns > 0 then\n"
   "  ns = ns..\":\"\n"
   "end\n"
   "\n"
   "local concat = function(...)\n"
   "  local arg = {...}\n"
   "  for i = 1, #arg do\n"
   "    arg[i]=tostring(arg[i])\n"
   "  end\n"
   "  return table.concat(arg, \" \")\n"
   "end\n"
   "local dbg =function(...) redis.call('echo', concat(...)); end\n"
   "local errors={}\n"
   "local err = function(...)\n"
   "  local msg = concat(...)\n"
   "  dbg(msg)\n"
   "  table.insert(errors, msg)\n"
   "end\n"
   "\n"
   "local tp=function(t, max_n)\n"
   "  local tt={}\n"
   "  for i, v in pairs(t) do\n"
   "    local val = tostring(v)\n"
   "    if max_n and #val > max_n then\n"
   "      val = val:sub(1, max_n) .. \"[...]\"\n"
   "    end\n"
   "    table.insert(tt, tostring(i) .. \": \" .. val)\n"
   "  end\n"
   "  return \"{\" .. table.concat(tt, \", \") .. \"}\"\n"
   "end\n"
   "\n"
   "local tohash=function(arr)\n"
   "  if type(arr)~=\"table\" then\n"
   "    return nil\n"
   "  end\n"
   "  local h = {}\n"
   "  local k=nil\n"
   "  for i, v in ipairs(arr) do\n"
   "    if k == nil then\n"
   "      k=v\n"
   "    else\n"
   "      h[k]=v; k=nil\n"
   "    end\n"
   "  end\n"
   "  return h\n"
   "end\n"
   "local type_is = function(key, _type, description)\n"
   "  local t = redis.call(\"TYPE\", key)['ok']\n"
   "  local type_ok = true\n"
   "  if type(_type) == \"table\" then\n"
   "    type_ok = false\n"
   "    for i, v in pairs(_type) do\n"
   "      if v == _type then\n"
   "        type_ok = true\n"
   "        break\n"
   "      end\n"
   "    end\n"
   "  elseif t ~= _type then\n"
   "    err((description or \"\"), key, \"should be type \" .. _type .. \", is\", t)\n"
   "    type_ok = false\n"
   "  end\n"
   "  return type_ok, t\n"
   "end\n"
   "\n"
   "local known_msgs_count=0\n"
   "local known_msgkeys = {}\n"
   "local known_channel_keys = {}\n"
   "\n"
   "\n"
   "local k = {\n"
   "  channel = function(chid)\n"
   "    return (\"%s{channel:%s}\"):format(chid)\n"
   "  end,\n"
   "  msg = function (chid, msgid)\n"
   "    return (\"%s:msg:%s\"):format(k.channel(chid), msgid)\n"
   "  end,\n"
   "  messages = function(chid)\n"
   "    return k.channel(chid) .. \":messages\"\n"
   "  end\n"
   "}\n"
   "\n"
   "local check_msg = function(chid, msgid, prev_msgid, next_msgid, description)\n"
   "  description = description and \"msg (\" .. description ..\")\" or \"msg\"\n"
   "  local msgkey = k.msg(chid, msgid)\n"
   "  if not known_msgkeys[msgkey] then\n"
   "    known_msgs_count = known_msgs_count + 1\n"
   "  end\n"
   "  known_msgkeys[msgkey]=true\n"
   "  local ok, t = type_is(msgkey, {\"hash\", \"none\"}, \"message hash\")\n"
   "  if t == \"none\" then\n"
   "    --message is missing, but maybe it expired under normal circumstances. \n"
   "    --check if any earlier messages are present\n"
   "    local msgids = redis.call('LRANGE', k.messages(chid), 0, -1)\n"
   "    local founds = 0\n"
   "    for i=#msgids, 1, -1 do\n"
   "      if msgids[i] == msgid then \n"
   "        break\n"
   "      end\n"
   "      local thismsgkey = k.msg(chid, msgids[i])\n"
   "      local ttt = redis.call('type', thismsgkey)['ok']\n"
   "      redis.breakpoint()\n"
   "      if ttt == \"hash\" then\n"
   "        founds = founds + 1\n"
   "      end\n"
   "    end\n"
   "    \n"
   "    if founds > 0 then\n"
   "      err(\"message\", msgkey, \"missing, with\", founds, \"prev. msgs in msg list\")\n"
   "    end\n"
   "    \n"
   "  end\n"
   "  local msg = tohash(redis.call('HGETALL', msgkey))\n"
   "  local ttl = tonumber(redis.call('TTL', msgkey))\n"
   "  local n = tonumber(redis.call(\"HLEN\", msgkey))\n"
   "  if n > 0 and (msg.data == nil or msg.id == nil or msg.time == nil or msg.tag == nil)then\n"
   "    err(\"incomplete \" .. description ..\"(ttl \"..ttl..\")\", msgkey, tp(msg))\n"
   "    return false\n"
   "  end\n"
   "  if t == \"hash\" and tonumber(ttl) < 0 then\n"
   "    err(\"message\", msgkey, \"ttl =\", ttl)\n"
   "  end\n"
   "  if ttl ~= -2 then\n"
   "    if prev_msgid ~= false and msg.prev ~= prev_msgid then\n"
   "      err(description, chid, msgid, \"prev_message wrong. expected\", prev_msgid, \"got\", msg.prev)\n"
   "    end\n"
   "    if next_msgid ~= false and msg.next ~= next_msgid then\n"
   "      err(description, chid, msgid, \"next_message wrong. expected\", next_msgid, \"got\", msg.next)\n"
   "    end\n"
   "  end\n"
   "end\n"
   "\n"
   "local check_channel = function(id)\n"
   "  local key={\n"
   "    ch = k.channel(id),\n"
   "    msgs = k.messages(id)\n"
   "  }\n"
   "  \n"
   "  local ok, chkey_type = type_is(key.ch, \"hash\", \"channel hash\")\n"
   "  if not ok then\n"
   "    if chkey_type ~= \"none\" then\n"
   "      err(\"unecpected channel key\", key.ch, \"type:\", chkey_type);\n"
   "    end\n"
   "    return false\n"
   "  end\n"
   "  local _, msgs_list_type = type_is(key.msgs, {\"list\", \"none\"}, \"channel messages list\")\n"
   "  \n"
   "  local ch = tohash(redis.call('HGETALL', key.ch))\n"
   "  local len = tonumber(redis.call(\"HLEN\", key.ch))\n"
   "  local ttl = tonumber(redis.call('TTL',  key.ch))\n"
   "  if not ch.current_message or not ch.time then\n"
   "    if msgs_list_type == \"list\" then\n"
   "      err(\"incomplete channel (ttl \" .. ttl ..\")\", key.ch, tp(ch))\n"
   "    end  \n"
   "  elseif (ch.current_message or ch.prev_message) and msgs_list_type ~= \"list\" then\n"
   "    err(\"channel\", key.ch, \"has a current_message but no message list\")\n"
   "  end\n"
   "  \n"
   "  local msgids = redis.call('LRANGE', key.msgs, 0, -1)\n"
   "  for i, msgid in ipairs(msgids) do\n"
   "    check_msg(id, msgid, msgids[i+1], msgids[i-1], \"msglist\")\n"
   "  end\n"
   "  \n"
   "  if ch.prev_message then\n"
   "    if redis.call('LINDEX', key.msgs, 1) ~= ch.prev_message then\n"
   "      err(\"channel\", key.ch, \"prev_message doesn't correspond to\", key.msgs, \"second list element\")\n"
   "    end\n"
   "    check_msg(id, ch.prev_message, false, ch.current_message, \"channel prev_message\")\n"
   "  end\n"
   "  if ch.current_message then\n"
   "    if redis.call('LINDEX', key.msgs, 0) ~= ch.current_message then\n"
   "      err(\"channel\", key.ch, \"current_message doesn't correspond to\", key.msgs, \"first list element\")\n"
   "    end\n"
   "    check_msg(id, ch.current_message, ch.prev_message, false, \"channel current_message\")\n"
   "  end\n"
   "  \n"
   "end\n"
   "\n"
   "local channel_ids = {}\n"
   "\n"
   "for i, chkey in ipairs(redis.call(\"KEYS\", k.channel(\"*\"))) do\n"
   "  local msgs_chid_match = chkey:match(\"^\"..k.messages(\"*\"))\n"
   "  if msgs_chid_match then\n"
   "    type_is(k.channel(msgs_chid_match), \"hash\", \"channel messages' corresponding hash key\")\n"
   "  elseif not chkey:match(\":msg$\") then\n"
   "    table.insert(channel_ids, chkey);\n"
   "    known_channel_keys[chkey] = true\n"
   "  end\n"
   "end\n"
   "\n"
   "dbg(\"found\", #channel_ids, \"channels\")\n"
   "for i, chkey in ipairs(channel_ids) do\n"
   "  local chid = chkey:match(\"^\" .. k.channel(\".*\"))\n"
   "  check_channel(chid)\n"
   "end\n"
   "\n"
   "for i, msgkey in ipairs(redis.call(\"KEYS\", k.channel(\"*\")..\":msg\")) do\n"
   "  if not known_msgkeys[msgkey] then\n"
   "    local ok, t = type_is(msgkey, \"hash\")\n"
   "    if ok then\n"
   "      if not redis.call('HGET', msgkey, 'unbuffered') then\n"
   "        err(\"orphan message\", msgkey, \"(ttl: \" .. redis.call('TTL', msgkey) .. \")\", tp(tohash(redis.call('HGETALL', msgkey)), 15))\n"
   "      end\n"
   "    else\n"
   "      err(\"orphan message\", msgkey, \"wrong type\", t) \n"
   "    end\n"
   "  end\n"
   "end\n"
   "\n"
   "if errors then\n"
   "  table.insert(errors, 1, concat(#channel_ids, \"channels,\",known_msgs_count,\"messages found\", #errors, \"problems\"))\n"
   "  return errors\n"
   "else\n"
   "  return concat(#channel_ids, \"channels,\", known_msgs_count, \"messages, all ok\")\n"
   "end\n"},

  {"subscriber_register", "0418d941e41ce9d8cb938860fd340d85c121d4cc",
   "--input: keys: [], values: [namespace, channel_id, subscriber_id, active_ttl, time, want_channel_settings]\n"
   "--  'subscriber_id' can be '-' for new id, or an existing id\n"
   "--  'active_ttl' is channel ttl with non-zero subscribers. -1 to persist, >0 ttl in sec\n"
   "--output: subscriber_id, num_current_subscribers, next_keepalive_time, channel_buffer_length\n"
   "--  'channel_buffer_length' is returned only if want_channel_settings is 1\n"
   "\n"
   "local ns, id, sub_id, active_ttl, time = ARGV[1], ARGV[2], ARGV[3], tonumber(ARGV[4]) or 20, tonumber(ARGV[5])\n"
   "local want_channel_settings = tonumber(ARGV[6]) == 1\n"
   "\n"
   "--local dbg = function(...) redis.call('echo', table.concat({...})); end\n"
   "\n"
   "redis.call('echo', ' ######## SUBSCRIBER REGISTER SCRIPT ####### ')\n"
   "local ch=(\"%s{channel:%s}\"):format(ns, id)\n"
   "local keys = {\n"
   "  channel =     ch,\n"
   "  messages =    ch..':messages',\n"
   "  subscribers = ch..':subscribers'\n"
   "}\n"
   "\n"
   "local setkeyttl=function(ttl)\n"
   "  for i,v in pairs(keys) do\n"
   "    if ttl > 0 then\n"
   "      redis.call('expire', v, ttl)\n"
   "    else\n"
   "      redis.call('persist', v)\n"
   "    end\n"
   "  end\n"
   "end\n"
   "\n"
   "local random_safe_next_ttl = function(ttl)\n"
   "  return math.floor(ttl/2 + ttl/2.1 * math.random())\n"
   "end\n"
   "\n"
   "local sub_count\n"
   "\n"
   "if sub_id == \"-\" then\n"
   "  sub_id = tonumber(redis.call('HINCRBY', keys.channel, \"last_subscriber_id\", 1))\n"
   "  sub_count=tonumber(redis.call('hincrby', keys.channel, 'subscribers', 1))\n"
   "else\n"
   "  sub_count=tonumber(redis.call('hget', keys.channel, 'subscribers'))\n"
   "end\n"
   "if time then\n"
   "  redis.call('hset', keys.channel, \"last_seen_subscriber\", time)\n"
   "end\n"
   "\n"
   "local next_keepalive \n"
   "local actual_ttl = tonumber(redis.call('ttl', keys.channel))\n"
   "if actual_ttl < active_ttl then\n"
   "  setkeyttl(active_ttl)\n"
   "  next_keepalive = random_safe_next_ttl(active_ttl)\n"
   "else\n"
   "  next_keepalive = random_safe_next_ttl(actual_ttl)\n"
   "end\n"
   "\n"
   "\n"
   "local ret = {sub_id, sub_count, next_keepalive}\n"
   "if want_channel_settings then\n"
   "  local max_messages = tonumber(redis.call('hget', keys.channel, 'max_stored_messages'))\n"
   "  table.insert(ret, max_messages)\n"
   "end\n"
   "\n"
   "return ret\n"},

  {"subscriber_unregister", "a98e07b21485951a7d34cf80736e53db1b6e87a6",
   "--input: keys: [], values: [namespace, channel_id, subscriber_id, empty_ttl]\n"
   "-- 'subscriber_id' is an existing id\n"
   "-- 'empty_ttl' is channel ttl when without subscribers. 0 to delete immediately, -1 to persist, >0 ttl in sec\n"
   "--output: subscriber_id, num_current_subscribers\n"
   "\n"
   "local ns, id, sub_id, empty_ttl = ARGV[1], ARGV[2], ARGV[3], tonumber(ARGV[4]) or 20\n"
   "\n"
   "--local dbg = function(...) redis.call('echo', table.concat({...})); end\n"
   "\n"
   "redis.call('echo', ' ######## SUBSCRIBER UNREGISTER SCRIPT ####### ')\n"
   "local ch=('%s{channel:%s}'):format(ns, id)\n"
   "local keys = {\n"
   "  channel =     ch,\n"
   "  messages =    ch..':messages',\n"
   "  subscribers = ch..':subscribers',\n"
   "}\n"
   "\n"
   "local setkeyttl=function(ttl)\n"
   "  for i,v in pairs(keys) do\n"
   "    if ttl > 0 then\n"
   "      redis.call('expire', v, ttl)\n"
   "    elseif ttl < 0 then\n"
   "      redis.call('persist', v)\n"
   "    else\n"
   "      redis.call('del', v)\n"
   "    end\n"
   "  end\n"
   "end\n"
   "\n"
   "local sub_count = 0\n"
   "\n"
   "local res = redis.pcall('EXISTS', keys.channel)\n"
   "if type(res) == \"table\" and res[\"err\"] then\n"
   "  return {err = (\"CLUSTER KEYSLOT ERROR. %i %s\"):format(empty_ttl, id)}\n"
   "end\n"
   "\n"
   "if res ~= 0 then\n"
   "   sub_count = redis.call('hincrby', keys.channel, 'subscribers', -1)\n"
   "\n"
   "  if sub_count == 0 and tonumber(redis.call('LLEN', keys.messages)) == 0 then\n"
   "    setkeyttl(empty_ttl)\n"
   "  elseif sub_count < 0 then\n"
   "    return {err=\"Subscriber count for channel \" .. id .. \" less than zero: \" .. sub_count}\n"
   "  end\n"
   "else\n"
   "  --dbg(\"channel \", id, \" already gone\")\n"
   "end\n"
   "\n"
   "return {sub_id, sub_count}\n"}
};
const int redis_lua_scripts_count=11;
