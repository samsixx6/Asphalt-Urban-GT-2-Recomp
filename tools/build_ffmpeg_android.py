"""Cross-build the already verified minimal FFmpeg source for Android ARM64."""
import hashlib,json,os,subprocess,tarfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'toolchains/android'
def main():
 archive=ROOT/'toolchains/ffmpeg-7.1.4.tar.xz'
 assert hashlib.sha256(archive.read_bytes()).hexdigest()=='71f4aac3573ed9060489cb62526a6c7dda815ae10993789611acd7be9fa9fbf4'
 source=BASE/'ffmpeg-7.1.4'
 if not source.exists():
  with tarfile.open(archive) as tar:tar.extractall(BASE,filter='data')
 # FFmpeg rejects out-of-tree builds when its source path contains spaces.
 # This is a separate Android-only source extraction, so configure in-tree.
 build=source;(build/'tmp').mkdir(exist_ok=True)
 logdir=ROOT/'build/logs/ffmpeg-android';logdir.mkdir(parents=True,exist_ok=True)
 env=dict(os.environ);bin=ROOT/'toolchains/w64devkit/bin';env['PATH']=str(bin)+os.pathsep+env.get('PATH','');env['TMPDIR']='./tmp'
 tc='../ndk/toolchains/llvm/prebuilt/windows-x86_64'
 options=['--prefix=../ffmpeg-install','--target-os=android','--arch=aarch64','--cpu=armv8-a','--enable-cross-compile',
  f'--cc={tc}/bin/clang.exe --target=aarch64-linux-android26',f'--cxx={tc}/bin/clang++.exe --target=aarch64-linux-android26',
  f'--ar={tc}/bin/llvm-ar.exe',f'--nm={tc}/bin/llvm-nm.exe',f'--ranlib={tc}/bin/llvm-ranlib.exe',f'--strip={tc}/bin/llvm-strip.exe',
  f'--sysroot={tc}/sysroot','--host-cc=../../w64devkit/bin/gcc.exe','--host-cflags=-mcrtdll=ucrt','--host-ldflags=-mcrtdll=ucrt',
  '--extra-cflags=-fPIC','--disable-everything','--disable-autodetect','--disable-asm','--disable-doc','--disable-programs',
  '--disable-network','--disable-avdevice','--disable-avfilter','--disable-avformat','--disable-postproc','--disable-debug',
  '--disable-shared','--enable-static','--enable-pic','--enable-decoder=h264,atrac3,atrac3p','--enable-parser=h264','--enable-swscale']
 started=time.monotonic()
 for name,command in [('configure',[str(bin/'sh.exe'),'./configure',*options]),
                      ('make',[str(bin/'make.exe'),'-j2','V=1']),('install',[str(bin/'make.exe'),'install','V=1'])]:
  with (logdir/(name+'.log')).open('wb') as log:result=subprocess.run(command,cwd=build,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=900)
  if result.returncode:
   print((logdir/(name+'.log')).read_text(errors='replace')[-6000:]);raise SystemExit(result.returncode)
  print('Android FFmpeg '+name+': passed',flush=True)
 libs=BASE/'ffmpeg-install/lib'
 report={'version':'7.1.4','abi':'arm64-v8a','api':26,'options':options,'seconds':round(time.monotonic()-started,2),
  'libraries':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in libs.glob('*.a')}}
 (ROOT/'build/ffmpeg-android-build.json').write_text(json.dumps(report,indent=2));print(json.dumps(report),flush=True)
if __name__=='__main__':main()
