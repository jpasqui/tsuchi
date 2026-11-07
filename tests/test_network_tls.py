"""Independent TLS certificate tests against local HTTPS servers."""
import http.server
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import threading
import time
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host_tools import find_openssl
OPENSSL=find_openssl()
class Handler(http.server.BaseHTTPRequestHandler):
 protocol_version='HTTP/1.1'
 def log_message(self,*args):pass
 def do_GET(self):
  try:
   if self.path=='/large':
    self.send_response(200);self.send_header('Content-Length',str(4*1024*1024));self.end_headers()
    data=bytes(range(256))*256
    for _ in range(64):self.wfile.write(data)
   elif self.path in ('/slow-body','/truncated'):
    self.send_response(200);self.send_header('Content-Length',str(1024*1024));self.send_header('Connection','close');self.end_headers()
    self.wfile.write(bytes(range(256))*256);self.wfile.flush()
    if self.path=='/slow-body':time.sleep(.5)
    self.close_connection=True
   elif self.path=='/chunked':
    self.send_response(200);self.send_header('Transfer-Encoding','chunked');self.end_headers()
    data=(bytes(range(256))*274)[:70000]
    for pos in range(0,len(data),12000):
     chunk=data[pos:pos+12000];self.wfile.write(f'{len(chunk):x}\r\n'.encode()+chunk+b'\r\n')
    self.wfile.write(b'0\r\n\r\n')
   else:
    self.send_response(200);self.send_header('Content-Length','7');self.end_headers();self.wfile.write(b'captain')
  except (BrokenPipeError,ConnectionResetError,ssl.SSLError):pass
class NetworkTLS(unittest.TestCase):
 @unittest.skipUnless(sys.platform == 'win32' and shutil.which('clang') and OPENSSL is not None,'needs clang and openssl')
 def test_certificates_limits_and_cancel(self):
  out=ROOT/'build/network-tls';out.mkdir(parents=True,exist_ok=True)
  openssl=OPENSSL
  def command(*parts):subprocess.run([str(openssl),*map(str,parts)],cwd=out,check=True,capture_output=True)
  command('req','-x509','-newkey','rsa:2048','-nodes','-subj','/CN=Tsuchi test root','-days','2','-keyout','root.key','-out','root.pem','-addext','basicConstraints=critical,CA:TRUE')
  servers=[]
  for name,hostname,days in [('trusted','localhost','2'),('wrong','wrong.example','2'),('expired','localhost','-1')]:
   command('req','-new','-newkey','rsa:2048','-nodes','-subj',f'/CN={hostname}','-keyout',f'{name}.key','-out',f'{name}.csr')
   (out/f'{name}.ext').write_text(f'subjectAltName=DNS:{hostname}\nbasicConstraints=critical,CA:FALSE\nextendedKeyUsage=serverAuth\nkeyUsage=digitalSignature,keyEncipherment\n',encoding='utf-8')
   if name=='expired':
    (out/'index.txt').write_text('',encoding='utf-8');(out/'serial').write_text('01\n',encoding='utf-8')
    (out/'expired-ca.cnf').write_text('[ca]\ndefault_ca=local\n[local]\ndatabase=index.txt\nnew_certs_dir=.\nserial=serial\ndefault_md=sha256\ndefault_days=1\npolicy=policy_any\nunique_subject=no\n[policy_any]\ncommonName=supplied\n[server]\nsubjectAltName=DNS:localhost\nbasicConstraints=critical,CA:FALSE\nextendedKeyUsage=serverAuth\nkeyUsage=digitalSignature,keyEncipherment\n',encoding='utf-8')
    command('ca','-config','expired-ca.cnf','-keyfile','root.key','-cert','root.pem','-in',f'{name}.csr','-out',f'{name}.pem','-startdate','20200101000000Z','-enddate','20210101000000Z','-extensions','server','-batch','-notext')
   else:
    command('x509','-req','-in',f'{name}.csr','-CA','root.pem','-CAkey','root.key','-CAcreateserial','-days',days,'-extfile',f'{name}.ext','-out',f'{name}.pem')
  command('req','-x509','-newkey','rsa:2048','-nodes','-subj','/CN=localhost','-days','2','-keyout','untrusted.key','-out','untrusted.pem','-addext','subjectAltName=DNS:localhost')
  for name in ('trusted','wrong','expired','untrusted'):
   server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);server.daemon_threads=True
   context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.minimum_version=ssl.TLSVersion.TLSv1_2;context.load_cert_chain(out/f'{name}.pem',out/f'{name}.key');server.socket=context.wrap_socket(server.socket,server_side=True)
   threading.Thread(target=server.serve_forever,daemon=True).start();servers.append(server)
  try:
   (out/'roots.pem').write_bytes((ROOT/'third_party/mbedtls-3.6.7/cacert.pem').read_bytes()+b'\n'+(out/'root.pem').read_bytes())
   clang=shutil.which('clang');objects=[]
   flags=['-O1','-D_CRT_SECURE_NO_WARNINGS','-I',str(ROOT/'third_party/mbedtls-3.6.7/include'),'-I',str(ROOT/'runtime/https'),'-DMBEDTLS_CONFIG_FILE="https_tls_config.h"']
   for source in sorted((ROOT/'third_party/mbedtls-3.6.7/library').glob('*.c')):
    obj=out/f'{source.stem}.o'
    if not obj.exists() or obj.stat().st_mtime<max(source.stat().st_mtime,(ROOT/'runtime/https/https_tls_config.h').stat().st_mtime):subprocess.run([clang,*flags,'-std=c99','-c',str(source),'-o',str(obj)],check=True,capture_output=True)
    objects.append(str(obj))
   exe=out/'network_tls_test.exe'
   result=subprocess.run([shutil.which('clang++'),'-std=c++17',*flags,'-I',str(ROOT/'framework/game'),str(ROOT/'runtime/https/https_tls.cpp'),str(ROOT/'tests/game/network_tls_test.cpp'),*objects,'-lws2_32','-lbcrypt','-o',str(exe)],capture_output=True,text=True)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   result=subprocess.run([str(exe),str(out/'roots.pem'),*[f'https://localhost:{server.server_port}' for server in servers]],capture_output=True,text=True,timeout=180)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertIn('PASS independent TLS',result.stdout)
   (out/'memory-results.txt').write_text(result.stdout,encoding='utf-8')
  finally:
   for server in servers:server.shutdown();server.server_close()
if __name__=='__main__':unittest.main()
