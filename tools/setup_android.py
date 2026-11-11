"""Provision checksum-verified portable Android build tools inside this project."""
import concurrent.futures, hashlib, json, shutil, urllib.request, zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'toolchains/android'
PACKAGES=[
 ('ndk','https://dl.google.com/android/repository/android-ndk-r28c-windows.zip','sha1','086bba43ff2f5eb0e387b15c8278bb4e0d89ba1d',748118221),
 ('sdk/platforms/android-35','https://dl.google.com/android/repository/platform-35_r02.zip','sha1','0bb560a90a7a2cbd0dd8348224d518b638fe7949',64273788),
 ('sdk/build-tools/35.0.0','https://dl.google.com/android/repository/build-tools_r35_windows.zip','sha1','af059bb67cf7786f45ee0db85e2d24985df1b4b6',59878107),
 ('jdk','https://aka.ms/download-jdk/microsoft-jdk-21.0.13.0.2-windows-x64.zip','sha256','c5706a9cf482ef1a0622a86eea5b13d557b07f04554572af061e2d02b58c0f99',201450315)]
def digest(path,algorithm):
 with path.open('rb') as f:return hashlib.file_digest(f,algorithm).hexdigest()
def provision(item):
 target,url,algorithm,expected,size=item
 if expected is None:
  with urllib.request.urlopen(url+'.sha256sum.txt',timeout=60) as response:expected=response.read().decode().split()[0].lower()
  if len(expected)!=64 or any(c not in '0123456789abcdef' for c in expected):raise RuntimeError('Invalid JDK checksum')
 archive=BASE/'downloads'/url.rsplit('/',1)[1];archive.parent.mkdir(parents=True,exist_ok=True)
 if not archive.exists() or (size and archive.stat().st_size!=size) or digest(archive,algorithm)!=expected:
  partial=archive.with_suffix(archive.suffix+'.download');done=0;report=0
  with urllib.request.urlopen(url,timeout=120) as response,partial.open('wb') as output:
   while chunk:=response.read(1024*1024):
    output.write(chunk);done+=len(chunk)
    if done-report>=64*1024*1024:print(f'{target}: downloaded {done//1048576} MiB',flush=True);report=done
  if (size and done!=size) or digest(partial,algorithm)!=expected:raise RuntimeError(f'Checksum/size mismatch: {target}')
  partial.replace(archive)
 destination=(BASE/target).resolve();stamp=destination/'.archive-sha256';sha=digest(archive,'sha256')
 if not stamp.exists() or stamp.read_text()!=sha:
  with zipfile.ZipFile(archive) as z:
   for entry in z.infolist():
    parts=Path(entry.filename.replace('\\','/')).parts
    if len(parts)<2:continue
    path=(destination/Path(*parts[1:])).resolve()
    if not path.is_relative_to(destination):raise RuntimeError('Archive path escaped destination')
    if entry.is_dir():path.mkdir(parents=True,exist_ok=True);continue
    path.parent.mkdir(parents=True,exist_ok=True)
    with z.open(entry) as source,path.open('wb') as output:shutil.copyfileobj(source,output,1024*1024)
  stamp.write_text(sha)
 print(f'{target}: verified and extracted',flush=True)
 return {'target':target,'url':url,'official_checksum_algorithm':algorithm,'official_checksum':expected,'sha256':sha,'bytes':archive.stat().st_size,'path':str(destination)}
def main():
 BASE.mkdir(parents=True,exist_ok=True)
 with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:results=list(pool.map(provision,PACKAGES))
 (ROOT/'build/android-toolchain.json').write_text(json.dumps({'packages':results,'platform':35,'ndk':'28.2.13676358','min_api':26},indent=2))
 print('Android toolchain ready',flush=True)
if __name__=='__main__':main()
