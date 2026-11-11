"""Exact relocation oracle and malformed-input checks for the Windows ISO importer."""
from pathlib import Path
import json,os,subprocess,sys,tempfile,unittest
from prx_image import rebase_prx
from test_prx_image import fixture
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless((ROOT/'toolchains/w64devkit/bin/gcc.exe').is_file(),'Install the pinned Windows compiler first')
class BootstrapTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.out=ROOT/'build/tests/windows-bootstrap';cls.out.mkdir(parents=True,exist_ok=True)
  cls.exe=cls.out/'test.exe';gcc=ROOT/'toolchains/w64devkit/bin/gcc.exe'
  p=subprocess.run([str(gcc),'-mcrtdll=ucrt','-std=c11','-O1','-I',str(ROOT/'src'),str(ROOT/'src/test_windows_bootstrap.c'),str(ROOT/'src/windows_bootstrap.c'),'-lbcrypt','-o',str(cls.exe)],capture_output=True)
  (cls.out/'compile.log').write_bytes(p.stdout+p.stderr)
  if p.returncode:raise RuntimeError(p.stderr.decode(errors='replace'))
 def run_image(self,data,expected=None):
  with tempfile.TemporaryDirectory(dir=self.out) as td:
   td=Path(td);src=td/'source.bin';dst=td/'result.elf';src.write_bytes(data)
   p=subprocess.run([str(self.exe),'rebase',str(src),str(dst)],capture_output=True)
   self.assertEqual(src.read_bytes(),data)
   if expected is None:self.assertNotEqual(p.returncode,0);self.assertFalse(dst.exists())
   else:self.assertEqual(p.returncode,0,p.stderr.decode(errors='replace'));self.assertEqual(dst.read_bytes(),expected)
 def test_synthetic_matches_python_oracle(self):
  data=fixture();self.run_image(data,rebase_prx(data,0x08804000)[0])
 def test_multiple_hi_share_original_lo(self):
  import struct
  data=bytearray(fixture([(0,5),(4,5),(8,6)]));struct.pack_into('<III',data,0x100,0x3c080001,0x3c090001,0x25088004)
  self.run_image(bytes(data),rebase_prx(bytes(data),0x08804000)[0])
 def test_malformed_tables_and_relocations_refuse(self):
  import struct
  values=[b'',fixture()[:100],fixture([(0,7)]),fixture([(0,5)]),fixture([(0,0x102)]),fixture([(0xc0,2)]),fixture([(0,2),(0,2)])]
  for data in values:
   with self.subTest(length=len(data)):self.run_image(data)
 @unittest.skipUnless((ROOT/'work/input/BOOT.BIN').is_file() and (ROOT/'work/input/ASPHALT_REBASED.ELF').is_file(),'Prepare your owned ISO for original-image checks')
 def test_actual_boot_matches_existing_image(self):
  data=(ROOT/'work/input/BOOT.BIN').read_bytes();self.run_image(data,(ROOT/'work/input/ASPHALT_REBASED.ELF').read_bytes())
 @unittest.skipUnless((ROOT.parent/'Asphalt - Urban GT 2 (Europe) (En,Fr,De,Es,It).iso').is_file() and (ROOT/'work/input/ASPHALT_REBASED.ELF').is_file(),'Original ISO fixture is local only')
 def test_actual_iso_extracts_same_image(self):
  iso=ROOT.parent/'Asphalt - Urban GT 2 (Europe) (En,Fr,De,Es,It).iso'
  with tempfile.TemporaryDirectory(dir=self.out) as td:
   target=Path(td)/'from-iso.elf';p=subprocess.run([str(self.exe),'iso',str(iso),str(target)],capture_output=True)
   self.assertEqual(p.returncode,0,p.stderr.decode(errors='replace'));self.assertEqual(target.read_bytes(),(ROOT/'work/input/ASPHALT_REBASED.ELF').read_bytes())
 def test_wrong_iso_is_rejected(self):
  with tempfile.TemporaryDirectory(dir=self.out) as td:
   src=Path(td)/'not game.iso';dst=Path(td)/'result';src.write_bytes(bytes(65536));p=subprocess.run([str(self.exe),'iso',str(src),str(dst)],capture_output=True);self.assertNotEqual(p.returncode,0);self.assertFalse(dst.exists())
if __name__=='__main__':unittest.main()
