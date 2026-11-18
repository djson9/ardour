local _,path=reaper.get_action_context()
local out=path:match('^(.*)/[^/]+$')
assert(reaper.get_ini_file()==out..'/config/reaper.ini','Wrong config')
assert(reaper.CountTracks(0)==0,'Expected empty project')
local function write(name,txt) local f=assert(io.open(out..'/'..name,'w'));f:write(txt);f:close() end
local function read(name) local f=assert(io.open(out..'/'..name));local s=f:read('*a');f:close();return s end
local stall_ms=tonumber(read('stall-ms.txt'))
local force_live=tonumber(read('force-live.txt'))
local tracks={}
local events={}
local action_file=io.open(out..'/action.txt','r')
local action=action_file and action_file:read('*a') or 'monitor';if action_file then action_file:close() end
local function event(label)
  events[#events+1]=string.format('%.9f,%.9f,%s',reaper.time_precise(),reaper.GetPlayPosition(),label)
end
local function run()
 reaper.gmem_attach('ardour_anticipation_handoff')
 for i=0,1 do
  reaper.InsertTrackAtIndex(i,false)
  local t=reaper.GetTrack(0,i);tracks[i+1]=t
  reaper.GetSetMediaTrackInfo_String(t,'P_NAME',i==0 and 'Transitions' or 'Always live',true)
  reaper.SetOnlyTrackSelected(t)
  reaper.SetEditCurPos(0,false,false)
  reaper.InsertMedia(out..'/source.wav',0)
  local fx=reaper.TrackFX_AddByName(t,'JS: trace',false,-1)
  assert(fx>=0,'Trace effect missing: '..reaper.GetResourcePath())
  reaper.TrackFX_SetParam(t,fx,0,i)
  reaper.SetMediaTrackInfo_Value(t,'I_PERFFLAGS',(i==1 or force_live==1) and 2 or 0)
  reaper.SetMediaTrackInfo_Value(t,'I_RECINPUT',0)
  reaper.SetMediaTrackInfo_Value(t,'I_RECMON',1)
 end
 reaper.SetEditCurPos(0,false,false);reaper.GetSetRepeat(0)
 local ready=reaper.time_precise()+1
 local started=nil;local armed=false;local disarmed=false;local monitor=nil
 local function tick()
  if not started then
   if reaper.time_precise()<ready then reaper.defer(tick);return end
   reaper.gmem_write(0,0);reaper.gmem_write(1000000,0)
   reaper.Audio_Init();reaper.OnPlayButton();started=reaper.time_precise();event('play')
  end
  local elapsed=reaper.time_precise()-started
  local m=reaper.GetMediaTrackInfo_Value(tracks[1],'B_RECMON_IN_EFFECT')
  if m~=monitor then event('monitor_'..tostring(m));monitor=m end
  if elapsed>=1 and not armed then
   if action=='seek' then event('seek_back_before');reaper.SetEditCurPos(1,false,true);event('seek_back_after')
   else event('arm_before');reaper.SetMediaTrackInfo_Value(tracks[1],'I_RECARM',1);event('arm_after') end
   armed=true
  end
  if elapsed>=2 and not disarmed then
   if action=='seek' then event('seek_forward_before');reaper.SetEditCurPos(5,false,true);event('seek_forward_after')
   else event('disarm_before');reaper.gmem_write(2000000,stall_ms);reaper.SetMediaTrackInfo_Value(tracks[1],'I_RECARM',0);event('disarm_after') end
   disarmed=true
  end
  if elapsed<3.5 then reaper.defer(tick);return end
  event('stop_before');reaper.OnStopButton()
  for slot=0,1 do
   local rows={'clock,position,frames,samples_seen,history,init_count,play_state,sample_rate'}
   local base=slot*1000000;local count=math.min(16000,reaper.gmem_read(base))
   for n=0,count-1 do
    local vals={};for c=0,7 do vals[#vals+1]=string.format('%.17g',reaper.gmem_read(base+16+n*8+c)) end
    rows[#rows+1]=table.concat(vals,',')
   end
   write('trace-'..slot..'.csv',table.concat(rows,'\n')..'\n')
  end
  write('fault.csv',string.format('start,end,position,live_position\n%.9f,%.9f,%.9f,%.9f\n',reaper.gmem_read(2000001),reaper.gmem_read(2000002),reaper.gmem_read(2000003),reaper.gmem_read(2000004)))
  write('events.csv','clock,position,event\n'..table.concat(events,'\n')..'\n')
  write('complete.txt',reaper.GetAppVersion()..'\n'..reaper.GetResourcePath()..'\n')
  -- Leave the isolated saved test project available; launcher terminates only its PID.
  reaper.Main_SaveProjectEx(0,out..'/trace.rpp',8)
 end
 reaper.defer(function() local ok,err=xpcall(tick,debug.traceback);if not ok then write('error.txt',err) end end)
end
local ok,err=xpcall(run,debug.traceback);if not ok then write('error.txt',err) end
