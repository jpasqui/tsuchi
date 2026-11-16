"""Network service and wire tests against a made-up game."""
from pathlib import Path
import shutil
import subprocess
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import generate
TLS_FLAGS=['-I',str(ROOT/'third_party/mbedtls-3.6.7/include'),'-I',str(ROOT/'runtime/https'),'-DMBEDTLS_CONFIG_FILE="https_tls_config.h"']
class Network(unittest.TestCase):
 @classmethod
 def setUpClass(cls):generate.write(ROOT/'build/gen')
 def tls_objects(self):
  """Mbed TLS on this machine's C library, with the framework's configuration: what sealed sessions use."""
  out=ROOT/'build/session-tls';out.mkdir(parents=True,exist_ok=True);objects=[]
  config=ROOT/'runtime/https/https_tls_config.h'
  for source in sorted((ROOT/'third_party/mbedtls-3.6.7/library').glob('*.c')):
   obj=out/f'{source.stem}.o'
   if not obj.exists() or obj.stat().st_mtime<max(source.stat().st_mtime,config.stat().st_mtime):
    subprocess.run([shutil.which('clang'),*TLS_FLAGS,'-O1','-D_CRT_SECURE_NO_WARNINGS','-std=c99','-c',str(source),'-o',str(obj)],check=True,capture_output=True)
   objects.append(str(obj))
  # An archive, so the linker takes only what sealing uses and not the TLS client's platform needs.
  archive=out/'mbedtls.lib'
  if not archive.exists() or archive.stat().st_mtime<max(Path(o).stat().st_mtime for o in objects):
   if archive.exists():archive.unlink()
   subprocess.run([shutil.which('llvm-ar'),'rcs',str(archive),*objects],check=True,capture_output=True)
  return [str(archive)]
 def build_run(self,name,sources):
  exe=ROOT/'build'/f'{name}.exe'
  command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti',*TLS_FLAGS]
  for folder in ('build/gen/include','build/gen/framework','framework/core','framework/base','framework/game'):command += ['-I',str(ROOT/folder)]
  command += [str(ROOT/p) for p in sources]
  if any('session_secure' in p for p in sources):command += [str(ROOT/'tests/game/session_tls_support.cpp'),*self.tls_objects(),'-lbcrypt']
  command += ['-o',str(exe)]
  built=subprocess.run(command,capture_output=True,text=True)
  self.assertEqual(built.returncode,0,built.stdout+built.stderr)
  result=subprocess.run([str(exe)],check=True,capture_output=True,text=True)
  self.assertIn('PASS',result.stdout)
 def test_service_calls_and_restore(self):
  self.build_run('network_services_test',['framework/core/registry.cpp','framework/core/frame.cpp','framework/game/holds.cpp','framework/game/net_backend.cpp','framework/game/network_requests.cpp','framework/game/session_backend.cpp','framework/game/session_wire.cpp','framework/game/session_traversal.cpp','framework/game/session_secure.cpp','build/gen/framework/net_service.cpp','build/gen/framework/session_service.cpp','build/gen/framework/time_service.cpp','tests/game/network_services_test.cpp'])
 def test_native_sdk_workarounds(self):
  self.build_run('network_native_test',['framework/game/network_native.cpp','tests/game/network_native_test.cpp'])
 def test_https_memory_budgets_and_cleanup(self):
  self.build_run('https_memory_test',['tests/game/https_memory_test.cpp'])
 def test_worker_wait_and_wakeup(self):
  self.build_run('https_work_signal_test',['tests/game/https_work_signal_test.cpp'])
 def test_targeted_events(self):
  self.build_run('targeted_events_test',['framework/core/registry.cpp','tests/game/targeted_events_test.cpp'])
 def test_loss_reordering_and_peer_contracts(self):
  self.build_run('session_wire_test',['framework/core/registry.cpp','framework/game/session_wire.cpp','framework/game/session_traversal.cpp','framework/game/session_secure.cpp','tests/game/session_wire_test.cpp'])
if __name__=='__main__':unittest.main()
