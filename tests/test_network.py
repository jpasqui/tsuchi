"""Network service and wire tests against a made-up game."""
from pathlib import Path
import shutil
import subprocess
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import generate
class Network(unittest.TestCase):
 @classmethod
 def setUpClass(cls):generate.write(ROOT/'build/gen')
 def build_run(self,name,sources):
  exe=ROOT/'build'/f'{name}.exe'
  command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti']
  for folder in ('build/gen/include','build/gen/framework','framework/core','framework/base','framework/game'):command += ['-I',str(ROOT/folder)]
  command += [str(ROOT/p) for p in sources]+['-o',str(exe)]
  built=subprocess.run(command,capture_output=True,text=True)
  self.assertEqual(built.returncode,0,built.stdout+built.stderr)
  result=subprocess.run([str(exe)],check=True,capture_output=True,text=True)
  self.assertIn('PASS',result.stdout)
 def test_service_calls_and_restore(self):
  self.build_run('network_services_test',['framework/core/registry.cpp','framework/core/frame.cpp','framework/game/holds.cpp','framework/game/net_backend.cpp','framework/game/network_requests.cpp','framework/game/session_backend.cpp','framework/game/session_wire.cpp','build/gen/framework/net_service.cpp','build/gen/framework/session_service.cpp','build/gen/framework/time_service.cpp','tests/game/network_services_test.cpp'])
 def test_native_sdk_workarounds(self):
  self.build_run('network_native_test',['framework/game/network_native.cpp','tests/game/network_native_test.cpp'])
 def test_https_memory_budgets_and_cleanup(self):
  self.build_run('https_memory_test',['tests/game/https_memory_test.cpp'])
 def test_worker_wait_and_wakeup(self):
  self.build_run('https_work_signal_test',['tests/game/https_work_signal_test.cpp'])
 def test_targeted_events(self):
  self.build_run('targeted_events_test',['framework/core/registry.cpp','tests/game/targeted_events_test.cpp'])
 def test_loss_reordering_and_peer_contracts(self):
  self.build_run('session_wire_test',['framework/core/registry.cpp','framework/game/session_wire.cpp','tests/game/session_wire_test.cpp'])
if __name__=='__main__':unittest.main()
