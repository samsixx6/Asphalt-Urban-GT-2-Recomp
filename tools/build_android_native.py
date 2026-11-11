"""Compile/cache the recompiled game and native Android host; no emulator or JIT."""
import argparse,concurrent.futures,hashlib,json,os,re,subprocess,time
from pathlib import Path
from patch_display_hooks import prepare
from display_sources import DISPLAY_SOURCES
ROOT=Path(__file__).resolve().parents[1]
VENDOR=ROOT/'vendor/psprecomp';GEN=ROOT/'work/generated-ctors';BUILD=ROOT/'build/android'
NDK=ROOT/'toolchains/android/ndk';TC=NDK/'toolchains/llvm/prebuilt/windows-x86_64'
RUNTIME=['cpu','mem','dispatch','ctors','render','render_gl','vfpu']
HLE=['hle','sysmem','threadman','power','intrman','display','ge','sascore','iofilemgr','misc']
def main():
 parser=argparse.ArgumentParser();parser.add_argument('--game-only',action='store_true');parser.add_argument('--hud-diagnostics',action='store_true');opt=parser.parse_args()
 BUILD.mkdir(parents=True,exist_ok=True);objects=BUILD/'objects';objects.mkdir(exist_ok=True)
 logs=ROOT/'build/logs/android';logs.mkdir(parents=True,exist_ok=True)
 flags=['--target=aarch64-linux-android26','--sysroot='+str(TC/'sysroot'),'-std=c11','-O1','-foptimize-sibling-calls','-ffp-contract=off','-fPIC',
  '-ffunction-sections','-fdata-sections','-DPSPRECOMP_REAL_THREADS=1','-DPSPRECOMP_TRACE=1',
  '-I',str(VENDOR/'include'),'-I',str(GEN),'-I',str(ROOT/'build'),'-I',str(ROOT/'src')]
 common=hashlib.sha256(json.dumps(flags).encode()+b''.join(p.read_bytes() for p in sorted((VENDOR/'include').rglob('*.h')))+
  (GEN/'asphalt_funcs.h').read_bytes()).digest()
 env=dict(os.environ);env['PATH']=str(TC/'bin')+os.pathsep+env.get('PATH','')
 def compile_one(source,kind,extra=()):
  label=kind+'_'+source.stem;obj=objects/(label+'.o');stamp=obj.with_suffix('.sha256')
  local=b''.join(p.read_bytes() for p in [*sorted((ROOT/'src').glob('*.h')),*sorted((ROOT/'build').glob('*.h'))]) if kind=='host' else b''
  if kind=='core' and source.stem=='render_gl':local+=b''.join((VENDOR/p).read_bytes() for p in ['src/host_pacing.h','src/hle/host_thread.h','src/render_smooth.h'])
  digest=hashlib.sha256(common+source.read_bytes()+local+json.dumps(extra).encode()).hexdigest()
  if obj.exists() and stamp.exists() and stamp.read_text()==digest:return obj
  result=subprocess.run([str(TC/'bin/clang.exe'),*flags,*extra,'-c',str(source),'-o',str(obj)],env=env,capture_output=True,timeout=600)
  (logs/(label+'.log')).write_bytes(result.stdout+result.stderr)
  if result.returncode:raise RuntimeError(source.name+':\n'+result.stderr.decode(errors='replace')[-6000:])
  stamp.write_text(digest);return obj
 split=ROOT/'build/registration-split';assert(split/'asphalt_register.c').is_file()
 original=(GEN/'asphalt_register.c').read_text();pattern=r'psp_register(?:_label)?\(0x[0-9A-Fa-f]+u, psp_(?:func|at|import)_[0-9A-Fa-f]+\);'
 expected=re.findall(pattern,original);actual=[]
 for p in sorted(split.glob('asphalt_register_part_*.c')):actual.extend(re.findall(pattern,p.read_text()))
 verified=json.loads((ROOT/'build/registration-split-verification.json').read_text())
 assert actual==expected and len(actual)==verified['calls_preserved'],'Registration split differs'
 overrides=prepare()
 sources=[(overrides.get(p.name,p),'game') for p in sorted(GEN.glob('*.c')) if p.name!='asphalt_register.c']+[(p,'register') for p in sorted(split.glob('*.c'))]
 started=time.monotonic();output=[]
 with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
  futures=[pool.submit(compile_one,p,kind) for p,kind in sources]
  for future in concurrent.futures.as_completed(futures):
   output.append(future.result())
   if len(output)%50==0 or len(output)==len(sources):print(f'ARM64 game units: {len(output)}/{len(sources)}',flush=True)
 if opt.game_only:print(f'ARM64 game compilation ready ({time.monotonic()-started:.1f}s)',flush=True);return
 for name in RUNTIME:output.append(compile_one(VENDOR/f'src/{name}.c','core'))
 # GE-only O2 is measured on Vivo with exact transformed vertex/state output.
 # Keep original game units and FP contraction rules on their existing flags.
 unit_overrides={'hle_ge':['-O2']}
 if opt.hud_diagnostics:unit_overrides['hle_ge'].append('-DASPHALT_HUD_DIAGNOSTICS=1')
 for name in HLE:output.append(compile_one(VENDOR/f'src/hle/{name}.c','hle',unit_overrides.get('hle_'+name,())))
 for name in ['android_host','android_game','audio_mixer','performance_report','host_interrupts','startup_services','savedata',*DISPLAY_SOURCES]:
  extra=['-DASPHALT_HUD_DIAGNOSTICS=1'] if opt.hud_diagnostics and name in ('display_semantics','display_semantic_hooks','display_session') else []
  if extra:unit_overrides['host_'+name]=extra
  output.append(compile_one(ROOT/f'src/{name}.c','host',extra))
 for name in ['asphalt_mpeg','asphalt_atrac']:
  output.append(compile_one(ROOT/f'src/{name}.c','host',['-DASPHALT_FFMPEG=1','-I',str(ROOT/'toolchains/android/ffmpeg-install/include')]))
 libs=[ROOT/f'toolchains/android/ffmpeg-install/lib/lib{name}.a' for name in ['swscale','avcodec','avutil']]
 so=BUILD/'libasphalt.so';rsp=BUILD/'link.rsp'
 args=['--target=aarch64-linux-android26','--sysroot='+str(TC/'sysroot'),'-shared','-Wl,-z,max-page-size=16384',
  '-Wl,-z,common-page-size=16384','-Wl,--gc-sections','-Wl,--no-undefined','-Wl,-soname,libasphalt.so',
  *map(str,sorted(output)),*map(str,libs),'-laaudio','-lEGL','-lGLESv3','-landroid','-llog','-ldl','-lm','-o',str(so)]
 rsp.write_text('\n'.join('"'+a.replace('\\','/')+'"' for a in args))
 result=subprocess.run([str(TC/'bin/clang.exe'),'@'+str(rsp)],env=env,capture_output=True,timeout=180)
 (logs/'link.log').write_bytes(result.stdout+result.stderr)
 if result.returncode:raise RuntimeError(result.stderr.decode(errors='replace')[-8000:])
 details=subprocess.check_output([str(TC/'bin/llvm-readelf.exe'),'-h','-l','-d',str(so)],env=env,text=True)
 (BUILD/'elf-details.txt').write_text(details)
 assert 'AArch64' in details and '0x4000' in details
 report={'abi':'arm64-v8a','min_api':26,'library':str(so),'sha256':hashlib.sha256(so.read_bytes()).hexdigest(),
  'game_units':len(sources),'registration_calls':len(actual),'flags':flags,'unit_overrides':unit_overrides,
  'compiler':subprocess.check_output([str(TC/'bin/clang.exe'),'--version'],env=env,text=True).splitlines()[0],
  'ge_source_sha256':hashlib.sha256((VENDOR/'src/hle/ge.c').read_bytes()).hexdigest(),
  'ge_object_sha256':hashlib.sha256((objects/'hle_ge.o').read_bytes()).hexdigest(),
  'motion_source_sha256':{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in
   ['src/native_engine.c','src/native_camera.c','src/native_camera.h','src/display_hooks.c','src/display-hooks.json','src/display_semantic_hooks.c']},
  'seconds':round(time.monotonic()-started,2),'phone_runtime_tested':False,'hud_diagnostics':opt.hud_diagnostics}
 (ROOT/'build/android-native-results.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2),flush=True)
if __name__=='__main__':main()
