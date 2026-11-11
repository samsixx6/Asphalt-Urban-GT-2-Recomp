"""Verify platform-specific factories and real keyboard-to-controller mapping."""
from pathlib import Path
import json,os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def main():
 out=ROOT/'build/tests/windows-preferences';out.mkdir(parents=True,exist_ok=True);gcc=ROOT/'toolchains/w64devkit/bin/gcc.exe';env=dict(os.environ);env['PATH']=str(gcc.parent)+os.pathsep+env.get('PATH','');results=[]
 for name,defines in [('android-defaults',[]),('windows-defaults',['-DASPHALT_WINDOWS_RELEASE=1']),('windows-keys',['-DASPHALT_WINDOWS_RELEASE=1','-DTEST_WINDOWS_KEYMAP=1'])]:
  exe=out/(name+'.exe');p=subprocess.run([str(gcc),'-mcrtdll=ucrt','-std=c11','-O1','-Wall','-Wextra','-Werror',*defines,str(ROOT/'src/display_settings.c'),str(ROOT/'src/host_input.c'),str(ROOT/'src/test_windows_preferences.c'),'-o',str(exe)],env=env,capture_output=True);(out/(name+'.compile.log')).write_bytes(p.stdout+p.stderr)
  if p.returncode:print(p.stderr.decode(errors='replace'));return p.returncode
  with tempfile.TemporaryDirectory(dir=out) as td:p=subprocess.run([str(exe),td],env=env,capture_output=True,timeout=10)
  (out/(name+'.log')).write_bytes(p.stdout+p.stderr);print(name,p.returncode,(p.stdout+p.stderr).decode(errors='replace').strip(),flush=True);results.append({'case':name,'exit_code':p.returncode})
  if p.returncode:return p.returncode
 (out/'results.json').write_text(json.dumps({'passed':True,'cases':results},indent=2));return 0
if __name__=='__main__':raise SystemExit(main())
