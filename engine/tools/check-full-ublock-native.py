#!/usr/bin/env python3
"""Check official full uBlock in sandboxed Brave via inherited private IPC."""
import argparse
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--browser',type=Path,required=True)
parser.add_argument('--ublock',type=Path,required=True)
parser.add_argument('--log',type=Path,default=Path('/tmp/gnosis-full-ublock-browser.log'))
args=parser.parse_args()
fixture=Path(__file__).resolve().parents[1]/'tests/fixtures'
integration=Path(__file__).resolve().parents[1]/'data/native-integration'
import os,subprocess,tempfile,json,select,time,resource,signal,socket
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
a,b=os.pipe();c,d=os.pipe();native_parent,native_child=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
def child_setup():
 os.dup2(a,3);os.dup2(d,4);os.dup2(native_child.fileno(),5)
with tempfile.TemporaryDirectory(prefix='gnosis-extension-probe-') as profile:
 os.makedirs(profile+'/Default')
 Path(profile+'/Default/Preferences').write_text(json.dumps({'extensions':{'ui':{'developer_mode':True}}}))
 p=subprocess.Popen([str(args.browser.resolve()),'--ozone-platform=wayland','--no-first-run','--enable-blink-features=WebBluetooth','--no-default-browser-check','--enable-unsafe-extension-debugging','--remote-debugging-pipe','--user-data-dir='+profile,'--headless=new','about:blank'],env=dict(os.environ,GNOSIS_NATIVE_UI_FD='5'),pass_fds=tuple(set((a,b,c,d,3,4,5,native_child.fileno()))),preexec_fn=child_setup,start_new_session=True,stdout=subprocess.DEVNULL,stderr=args.log.open('w'))
 os.close(a);os.close(d)
 buf=b'';seq=0;events=[];replies={}
 def receive(timeout=10):
  global buf
  until=time.monotonic()+timeout
  while b'\0' not in buf:
   remaining=until-time.monotonic()
   if remaining<=0 or not select.select([c],[],[],remaining)[0]:raise TimeoutError('protocol timeout')
   chunk=os.read(c,65536)
   if not chunk:raise EOFError('browser closed protocol')
   buf+=chunk
  raw,buf=buf.split(b'\0',1);value=json.loads(raw)
  if 'id' in value:replies[value['id']]=value
  else:events.append(value)
  return value
 def call(method,params={},session=None):
  global seq
  seq+=1;ident=seq;message={'id':ident,'method':method,'params':params}
  if session:message['sessionId']=session
  os.write(b,json.dumps(message).encode()+b'\0')
  while ident not in replies:receive()
  result=replies.pop(ident)
  if 'error' in result:raise RuntimeError(result['error'])
  return result.get('result',{})
 try:
  print('browser',call('Browser.getVersion')['product'],flush=True)
  helper=call('Extensions.loadUnpacked',{'path':str(integration)})['id']
  deadline=time.monotonic()+10
  while True:
   workers=[t for t in call('Target.getTargets')['targetInfos'] if t['type']=='service_worker' and t['url']==f'chrome-extension://{helper}/background.js']
   if workers:break
   if time.monotonic()>deadline:raise TimeoutError('native helper worker')
   time.sleep(.1)
  helper_session=call('Target.attachToTarget',{'targetId':workers[0]['targetId'],'flatten':True})['sessionId']
  call('Runtime.enable',{},helper_session)
  call('Runtime.runIfWaitingForDebugger',{},helper_session)
  call('Runtime.addBinding',{'name':'__gnosisExtensionsChanged'},helper_session)
  def management(expression):
   reply=call('Runtime.evaluate',{'expression':expression,'awaitPromise':True,'returnByValue':True,'userGesture':True},helper_session)
   if 'exceptionDetails' in reply:raise RuntimeError(reply)
   return reply['result'].get('value')
  deadline=time.monotonic()+10
  while management('typeof globalThis.gnosisExtensions?.inventory')!='function':
   if time.monotonic()>deadline:raise TimeoutError('manager worker startup')
   time.sleep(.1)
  assert helper not in [x['id'] for x in management('gnosisExtensions.inventory()')]

  deadline=time.monotonic()+10
  while True:
   pages=[t['targetId'] for t in call('Target.getTargets')['targetInfos'] if t['type']=='page']
   if pages:target=pages[0];break
   if time.monotonic()>deadline:raise TimeoutError('page target')
   time.sleep(.1)
  downloads=profile+'/downloads';os.mkdir(downloads)
  call('Browser.setDownloadBehavior',{'behavior':'allowAndName','downloadPath':downloads,'eventsEnabled':True})
  session=call('Target.attachToTarget',{'targetId':target,'flatten':True})['sessionId']
  call('Page.enable',{},session);call('Runtime.enable',{},session)

  import http.server,threading,functools
  requests=[]
  class Handler(http.server.SimpleHTTPRequestHandler):
   def log_message(self,*args):pass
   def do_GET(self):
    requests.append(self.path);super().do_GET()
  server=http.server.ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Handler,directory=str(fixture)))
  threading.Thread(target=server.serve_forever,daemon=True).start();port=server.server_port
  ubo=call('Extensions.loadUnpacked',{'path':str(args.ublock.resolve())})['id']
  actual=management('gnosisExtensions.inventory()');entry=next(x for x in actual if x['id']==ubo)
  assert entry['enabled'] and entry['name']=='uBlock Origin',entry
  manifest=json.loads((args.ublock/'manifest.json').read_text())
  assert manifest['manifest_version']==2 and entry['version']==manifest['version'],entry
  print('FULL_UBLOCK_VERSION',entry['version'],flush=True)
  call('Page.navigate',{'url':f'chrome-extension://{ubo}/dashboard.html'},session)
  deadline=time.monotonic()+20
  while time.monotonic()<deadline:
   ready=call('Runtime.evaluate',{'expression':"Boolean(globalThis.vAPI?.messaging)",'returnByValue':True},session)['result'].get('value')
   if ready:break
   time.sleep(.1)
  setup=(fixture/'configure-ublock.js').read_text().replace('GNOSIS_NETWORK_CONFIG','null').replace('GNOSIS_FIXTURE_PORT',str(port))
  call('Runtime.evaluate',{'expression':setup},session)
  deadline=time.monotonic()+30
  while time.monotonic()<deadline:
   result=call('Runtime.evaluate',{'expression':"({url:location.href,cosmetic:document.querySelector('.gnosis-ad')?getComputedStyle(document.querySelector('.gnosis-ad')).display:null,scriptlet:globalThis.gnosisSentinel})",'returnByValue':True},session)['result'].get('value',{})
   if result.get('url','').startswith(f'http://127.0.0.1:{port}/ublock.html') and result.get('cosmetic')=='none' and result.get('scriptlet')==True:break
   time.sleep(.2)
  assert result.get('cosmetic')=='none' and result.get('scriptlet')==True,result
  time.sleep(5)
  assert '/blocked.gif' not in requests,requests
  frame=call('Runtime.evaluate',{'expression':"getComputedStyle(document.querySelector('iframe').contentDocument.querySelector('.gnosis-frame-ad')).display",'returnByValue':True},session)['result'].get('value')
  assert frame=='none',frame
  print('FULL_UBLOCK_PASS native MV2, network blocked before server, cosmetic main/frame, real scriptlet',flush=True)
  # GHSA-cmqw-9r2w-6wv2: exercise the real picker page and transferred port.
  # The valid-token control ensures rejection is not just a broken/unloaded UI.
  background=next(t for t in call('Target.getTargets')['targetInfos'] if t['type']=='background_page' and t['url'].startswith(f'chrome-extension://{ubo}/'))
  background_session=call('Target.attachToTarget',{'targetId':background['targetId'],'flatten':True})['sessionId']
  for password in ['missing','wrong','valid']:
   # Grant a valid resource URL to test the picker-token defense independently.
   secret=call('Runtime.evaluate',{'expression':'vAPI.warSecret.short()','returnByValue':True},background_session)['result'].get('value')
   assert isinstance(secret,str) and secret
   picker=call('Target.createTarget',{'url':f'chrome-extension://{ubo}/web_accessible_resources/epicker-ui.html?secret={secret}'})['targetId']
   picker_session=call('Target.attachToTarget',{'targetId':picker,'flatten':True})['sessionId']
   call('Runtime.enable',{},picker_session)
   deadline=time.monotonic()+10
   ready=False
   while time.monotonic()<deadline:
    ready=call('Runtime.evaluate',{'expression':"typeof globalThis.vAPI?.messaging?.send==='function' && document.readyState==='complete'",'returnByValue':True},picker_session)['result'].get('value')
    if ready:break
    time.sleep(.1)
   assert ready,'element picker did not finish loading'
   expression="""(async()=>{
    const message={what:'epickerStart'}, mode=MODE;
    if(mode==='wrong')message.password='gnosis-invalid-picker-token';
    if(mode==='valid')message.password=await vAPI.messaging.send('elementPicker',{what:'elementPickerPassword'});
    return new Promise(resolve=>{
     const channel=new MessageChannel(), replies=[];
     channel.port1.onmessage=event=>replies.push(event.data.what);
     window.postMessage(message,'*',[channel.port2]);
     setTimeout(()=>{channel.port1.close();resolve(replies)},700);
    });
   })()""".replace('MODE',json.dumps(password))
   result=call('Runtime.evaluate',{'expression':expression,'awaitPromise':True,'returnByValue':True},picker_session)
   assert 'exceptionDetails' not in result,result
   messages=result['result'].get('value')
   assert isinstance(messages,list) and ('start' in messages if password=='valid' else not messages),(password,messages)
   call('Target.closeTarget',{'targetId':picker})
  print('FULL_UBLOCK_PICKER_AUTH_PASS missing/wrong tokens rejected; valid token accepted',flush=True)
  for enabled in [False,True]:
   actual=management('gnosisExtensions.setEnabled('+json.dumps(ubo)+','+str(enabled).lower()+')')
   assert next(x for x in actual if x['id']==ubo)['enabled']==enabled
  print('FULL_UBLOCK_TOGGLE_PASS',flush=True)
  server.shutdown()

 finally:
  try:call('Browser.close')
  except Exception:pass
  try:p.wait(timeout=5)
  except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
  os.close(b);os.close(c);native_parent.close();native_child.close()
