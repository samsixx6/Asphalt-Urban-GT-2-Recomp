from pathlib import Path
import os,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless((ROOT/'toolchains/w64devkit/bin/gcc.exe').is_file(),'Install the pinned Windows compiler first')
class LauncherTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.out=ROOT/'build/tests/windows-launcher';cls.out.mkdir(parents=True,exist_ok=True);cls.exe=cls.out/'test.exe';gcc=ROOT/'toolchains/w64devkit/bin/gcc.exe'
  p=subprocess.run([str(gcc),'-mcrtdll=ucrt','-std=c11','-O1','-I',str(ROOT/'src'),str(ROOT/'src/test_windows_launcher.c'),str(ROOT/'src/windows_bootstrap.c'),'-lbcrypt','-lshell32','-lcomdlg32','-o',str(cls.exe)],capture_output=True);(cls.out/'compile.log').write_bytes(p.stdout+p.stderr)
  if p.returncode:raise RuntimeError(p.stderr.decode(errors='replace'))
 def test_process_lifetime_lock_and_different_folders(self):
  with tempfile.TemporaryDirectory(dir=self.out) as td:
   root=Path(td);other=root/'other';other.mkdir()
   p=subprocess.Popen([str(self.exe),'hold',str(root)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
   try:
    self.assertEqual(p.stdout.readline().strip(),b'ready')
    self.assertEqual(subprocess.run([str(self.exe),'probe',str(root)]).returncode,7)
    self.assertEqual(subprocess.run([str(self.exe),'probe',str(root/'.')]).returncode,7)
    self.assertEqual(subprocess.run([str(self.exe),'probe',str(other)]).returncode,0)
   finally:p.communicate(b'\n',timeout=10)
   self.assertEqual(p.returncode,0);self.assertEqual(subprocess.run([str(self.exe),'probe',str(root)]).returncode,0)
if __name__=='__main__':unittest.main()
