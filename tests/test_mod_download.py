"""Public-source discovery, bounded package extraction, receipts, and streaming hashes."""
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]

class ModDownload(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out=ROOT/'build/mod-download';cls.out.mkdir(parents=True,exist_ok=True)
        cls.binary=cls.out/('check.exe' if sys.platform=='win32' else 'check')
        core=ROOT/'framework/core'
        sources=[core/name for name in ('mod_catalog.cpp','mod_zip.cpp','sha256.cpp','json.cpp','registry.cpp')]
        command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror','-I',str(core),'-I',str(ROOT/'build/gen/include'),*map(str,sources),str(ROOT/'tests/core/mod_download_test.cpp'),'-o',str(cls.binary)]
        result=subprocess.run(command,capture_output=True,text=True)
        if result.returncode:raise AssertionError(result.stdout+result.stderr)

    def run_case(self,mode,data,*args,success=True):
        path=self.out/'input.bin';path.write_bytes(data if isinstance(data,bytes) else data.encode())
        result=subprocess.run([str(self.binary),mode,str(path),*args],capture_output=True,text=True)
        self.assertEqual(result.returncode,0 if success else 2,result.stdout+result.stderr)
        return result.stdout

    def archive(self,entries,method=zipfile.ZIP_DEFLATED):
        out=io.BytesIO()
        with zipfile.ZipFile(out,'w',compression=method) as archive:
            for name,body in entries:archive.writestr(name,body)
        return out.getvalue()

    def test_stored_deflated_wrapped_and_multi_mod_packages(self):
        for method in (zipfile.ZIP_STORED,zipfile.ZIP_DEFLATED):
            for wrapper in ('','Release/'):
                entries=[(wrapper+'romfs/tsuchi/mods/author.one/mod.json',b'{"id":"author.one"}'),(wrapper+'romfs/tsuchi/mods/author.one/main.lua',b'print("loaded")'),(wrapper+'romfs/tsuchi/mods/author.two/mod.json',b'{"id":"author.two"}'),(wrapper+'romfs/tsuchi/mods/author.two/data.bin',bytes(range(256))*1500),(wrapper+'README.md',b'Notes')]
                result=self.run_case('zip',self.archive(entries,method))
                for name,body in entries[:-1]:self.assertIn(name.split('/mods/')[1]+' '+hashlib.sha256(body).hexdigest(),result)

    def test_paths_aliases_links_and_wrong_package_types_are_refused(self):
        good=('romfs/tsuchi/mods/author.mod/mod.json',b'{}')
        for path in ('../escape','romfs/tsuchi/mods/author.mod/../escape','romfs/tsuchi/mods/author.mod/a:b','romfs/tsuchi/mods/author.mod/NUL.txt','romfs/tsuchi/mods/author.mod/trailing.','romfs/tsuchi/mods/tsuchi.self/mod.json','romfs/Carrot4/Content/Paks/mod.pak','exefs/main'):
            with self.subTest(path=path):self.run_case('zip',self.archive([good,(path,b'x')]),success=False)
        self.run_case('zip',self.archive([good,('romfs/tsuchi/mods/author.mod/MOD.JSON',b'{}')]),success=False)
        link=zipfile.ZipInfo('romfs/tsuchi/mods/author.mod/link');link.create_system=3;link.external_attr=(0o120777<<16)
        self.run_case('zip',self.archive([good,(link,b'target')]),success=False)
        link.external_attr=0x400
        self.run_case('zip',self.archive([good,(link,b'target')]),success=False)

    def test_corruption_truncation_bombs_and_cancel_preserve_publication(self):
        entries=[('romfs/tsuchi/mods/author.mod/mod.json',b'{}'),('romfs/tsuchi/mods/author.mod/main.lua',b'hello'*20000)]
        data=self.archive(entries)
        for how in ('cancel','midcancel','write'):self.run_case('zip',data,how,success=False)
        self.run_case('zip',data[:-3],success=False)
        broken=bytearray(self.archive(entries,zipfile.ZIP_STORED));name_len=struct.unpack_from('<H',broken,26)[0];broken[30+name_len]^=1
        self.run_case('zip',bytes(broken),success=False)
        self.run_case('zip',self.archive([entries[0],('romfs/tsuchi/mods/author.mod/bomb',bytes(2<<20))]),success=False)
        self.run_case('zip',self.archive([(f'romfs/tsuchi/mods/author.mod/f{i}',b'x') for i in range(257)]),success=False)

    def test_legacy_container_zip_preserves_bytes_and_stable_ids(self):
        triple=[('romfs/Carrot4/Content/Paks/Map_P.'+ext,bytes([i])*130) for i,ext in enumerate(('pak','utoc','ucas'),1)]
        first=self.run_case('zip',self.archive(triple),'legacy')
        second=self.run_case('zip',self.archive([('release/'+name,data) for name,data in triple]),'legacy')
        self.assertEqual(first,second)
        self.assertIn('external.0123456789abcdef01234567.',first)
        for _,data in triple:self.assertIn(hashlib.sha256(data).hexdigest(),first)
        for files in (triple[:-1],triple+[('exefs/main',b'x')],triple+[('cheats/build.txt',b'[code]')],triple+[('romfs/tsuchi/mods/author.mod/mod.json',b'{}')],triple+[('other/Map_P.pak',b'x')]):
            self.run_case('zip',self.archive(files),'legacy',success=False)

    def test_legacy_cheat_archives_keep_names_and_reject_mixed_or_wrong_game(self):
        body=b'[Original name]\n400F0000 00000000 00000001\n'
        bare=self.run_case('zip',self.archive([('060f827bd2b38703.txt',body)]),'legacy')
        wrapped=self.run_case('zip',self.archive([('Release/atmosphere/contents/0100B7C00933A000/cheats/060F827BD2B38703.txt',body)]),'legacy')
        self.assertEqual(bare,wrapped)
        self.assertIn('.cheats/cheats/060F827BD2B38703.txt '+hashlib.sha256(body).hexdigest(),bare)
        good=('cheats/060F827BD2B38703.txt',body)
        for extra in [('romfs/a.pak',b'x'),('romfs/tsuchi/mods/a/mod.json',b'{}'),('cheats/build.txt',body),('other/060f827bd2b38703.txt',body)]:
            self.run_case('zip',self.archive([good,extra]),'legacy',success=False)
        self.run_case('zip',self.archive([('atmosphere/contents/0100000000000000/cheats/060F827BD2B38703.txt',body)]),'legacy',success=False)
        self.run_case('zip',self.archive([(good[0],bytes(65536))]),'legacy',success=False)
        self.run_case('zip',self.archive([(good[0],b'')]),'legacy',success=False)

    def test_github_topic_query_pages_and_failure_retains_cache(self):
        query=self.run_case('url','cat OR topic:wrong &x=1')
        self.assertIn('topic%3Atsuchi-mod%20is%3Apublic',query);self.assertNotIn('&x=',query)
        page={'total_count':20,'incomplete_results':True,'items':[{'full_name':'author/mod','name':'<tag>[Mod]','owner':{'login':'author'},'private':False}]}
        result=self.run_case('page',json.dumps(page));self.assertIn('1 1 1',result);self.assertNotIn('<tag>',result)
        self.run_case('page','{"message":"rate limit"}',success=False)
        for key in ('author/mod/extra','../mod','author/..','/mod','author/','author//mod'):
            page['items'][0]['full_name']=key;self.run_case('page',json.dumps(page),success=False)

    def test_release_assets_preview_filter_and_source_identity(self):
        asset={'id':4,'name':'mod.zip','size':100,'state':'uploaded','browser_download_url':'https://github.com/author/mod/releases/download/v1/mod.zip','digest':'sha256:'+'a'*64}
        stable={'tag_name':'v1','draft':False,'prerelease':False,'assets':[asset]}
        preview={'tag_name':'v2','draft':False,'prerelease':True,'assets':[dict(asset,id=5)]}
        self.assertTrue(self.run_case('files',json.dumps([stable,preview])).startswith('1\n'))
        self.assertTrue(self.run_case('files',json.dumps([stable,preview]),'gh','preview').startswith('2\n'))
        for patch in ({'name':'source.tar.gz'},{'browser_download_url':'https://github.com/attacker/mod/releases/download/v1/mod.zip'},{'digest':'sha256:bad'},{'size':1<<30}):
            stable['assets']=[dict(asset,**patch)];self.assertEqual(self.run_case('files',json.dumps([stable])),'0\n')

    def test_gamebanana_game_and_download_identity(self):
        listing={'_idRow':12,'_sName':'Mod','_aGame':{'_idRow':18712},'_aSubmitter':{'_sName':'Author'}}
        self.assertIn('12 Mod',self.run_case('page',json.dumps({'_aRecords':[listing]}),'gb'))
        listing['_aFiles']=[{'_idRow':4,'_sFile':'mod.zip','_nFilesize':100,'_sDownloadUrl':'https://gamebanana.com/dl/4'}]
        self.assertTrue(self.run_case('files',json.dumps(listing),'gb').startswith('1\n'))
        listing['_aGame']['_idRow']=1;self.run_case('files',json.dumps(listing),'gb',success=False)

    def test_receipt_corruption_and_incremental_hash(self):
        self.run_case('receipt',b'')
        for size in (0,1,55,56,63,64,65,127,128,100003):self.run_case('hash',(bytes(range(256))*((size+255)//256))[:size])

    def test_manifest_admission_and_restart_discovery(self):
        binary=self.out/('install.exe' if sys.platform=='win32' else 'install')
        dirs=[ROOT/'build/gen/include',ROOT/'build/gen/framework',ROOT/'framework/core',ROOT/'framework/base',ROOT/'framework/game']
        sources=['framework/core/registry.cpp','framework/core/ids.cpp','framework/core/json.cpp','framework/core/text.cpp','framework/core/texts.cpp','framework/base/settings.cpp','framework/base/mod_choices.cpp','framework/base/mods.cpp','tests/base/mod_install_test.cpp']
        command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',*[x for d in dirs for x in ('-I',str(d))],*[str(ROOT/p) for p in sources],'-o',str(binary)]
        command+=['-Xlinker','/OPT:REF'] if sys.platform=='win32' else ['-Wl,--gc-sections']
        built=subprocess.run(command,capture_output=True,text=True);self.assertEqual(built.returncode,0,built.stdout+built.stderr)
        ran=subprocess.run([str(binary)],capture_output=True,text=True);self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)

    def test_browser_navigation_cache_cancel_and_install(self):
        binary=self.out/('browser.exe' if sys.platform=='win32' else 'browser')
        dirs=[ROOT/'build/gen/include',ROOT/'build/gen/framework',ROOT/'framework/core',ROOT/'framework/base',ROOT/'framework/game']
        sources=['framework/core/json.cpp','framework/core/mod_catalog.cpp','framework/game/browser/provider.cpp','framework/game/browser/github.cpp','framework/game/browser/json.cpp','framework/game/browser/images.cpp','framework/game/browser/controller.cpp','framework/game/browser/bridge.cpp','tests/game/mod_browser_test.cpp']
        command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror',*[x for d in dirs for x in ('-I',str(d))],*[str(ROOT/p) for p in sources],'-o',str(binary)]
        built=subprocess.run(command,capture_output=True,text=True);self.assertEqual(built.returncode,0,built.stdout+built.stderr)
        ran=subprocess.run([str(binary)],capture_output=True,text=True);self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)

    def test_streaming_coordinator_publication_and_failed_install(self):
        binary=self.out/('coordinator.exe' if sys.platform=='win32' else 'coordinator')
        dirs=[ROOT/'build/gen/include',ROOT/'build/gen/framework',ROOT/'framework/core',ROOT/'framework/base',ROOT/'framework/game']
        sources=['framework/core/json.cpp','framework/core/text.cpp','framework/core/texts.cpp','framework/core/sha256.cpp','framework/core/mod_catalog.cpp','framework/core/mod_zip.cpp','framework/core/cheat_vm.cpp','tests/game/mod_download_native_test.cpp']
        command=[shutil.which('clang++'),'-std=c++17','-Wall','-Wextra','-Werror',*[x for d in dirs for x in ('-I',str(d))],*[str(ROOT/p) for p in sources],'-o',str(binary)]
        built=subprocess.run(command,capture_output=True,text=True);self.assertEqual(built.returncode,0,built.stdout+built.stderr)
        archive=self.out/'coordinator.zip';archive.write_bytes(self.archive([('romfs/tsuchi/mods/author.mod/mod.json',b'{}'),('romfs/tsuchi/mods/author.mod/main.lua',b'print("loaded")')]))
        for mode in ('good','digest','cancel','manifest','receipt','queue','update','update-failure'):
            with self.subTest(mode=mode):
                ran=subprocess.run([str(binary),str(archive),mode],capture_output=True,text=True);self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
        for mode,body in [('cheat-good',b'[Original name]\n400F0000 00000000 00000001\n'),('cheat-bad',b'[Bad]\nnot cheat words\n')]:
            archive.write_bytes(self.archive([('cheats/060f827bd2b38703.txt',body)]))
            ran=subprocess.run([str(binary),str(archive),mode],capture_output=True,text=True);self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
        good=b'[Original name]\n400F0000 00000000 00000001\n'
        archive.write_bytes(self.archive([('cheats/060F827BD2B38703.txt',good)]))
        bad=self.out/'bad-cheat.zip';bad.write_bytes(self.archive([('cheats/060F827BD2B38703.txt',b'[Broken]\nbad words')]))
        for mode,args in [('cheat-update',[]),('cheat-update-invalid',[str(bad)])]:
            ran=subprocess.run([str(binary),str(archive),mode,*args],capture_output=True,text=True);self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)

if __name__=='__main__':unittest.main()
