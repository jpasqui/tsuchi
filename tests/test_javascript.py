"""JavaScript bindings, limits, and language checks against the Lua tests' stand-in."""
import json
from pathlib import Path
import shutil
import sys
import unittest
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from gen import c_header, js_binding, load, service_glue, typescript
from test_all import CXX, EXE, run
from test_lua_u64 import precision_service
import quickjs_build


class JavaScript(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        schema = load(ROOT / 'schema')
        precision = precision_service()
        schema.services.append(precision)
        schema.host_order.append(precision)
        cls.out = ROOT / 'build/javascript-tests'
        cls.out.mkdir(parents=True, exist_ok=True)
        files = c_header.generate(schema) | service_glue.generate(schema) | js_binding.generate(schema)
        for name, text in files.items():
            path = cls.out / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_text(encoding='utf-8') != text:
                path.write_text(text, encoding='utf-8', newline='\n')
        cls.programs = {}
        for own in (False, True):
            program = cls.out / ('host-' + str(own) + EXE)
            sources = [ROOT / 'framework/core' / (n + '.cpp') for n in ('registry','ids','frame')]
            sources += sorted((cls.out / 'framework').glob('*.cpp')) + sorted((cls.out / 'javascript').glob('*.cpp'))
            sources += [p for p in sorted((ROOT / 'runtime/javascript').glob('*.cpp')) if p.name != 'script_mods.cpp']
            sources += [ROOT / 'tests/host/no_dynamic_hooks.cpp', ROOT / 'tests/javascript/fake_host.cpp', ROOT / 'framework/target/uint128.cpp']
            run([shutil.which('clang++'), *CXX, '-I', cls.out / 'include', '-I', cls.out / 'framework',
                 '-I', ROOT / 'framework/core', '-I', ROOT / 'runtime/javascript', '-isystem', quickjs_build.QUICKJS,
                 *sources, *quickjs_build.objects(own), '-o', program])
            cls.programs[own] = program

    def test_combat_events_errors_defaults_flags_and_cleanup(self):
        self.assertIn('javascript host passed', run([self.programs[False], ROOT / 'tests/javascript/combat.js']).stdout)

    def test_camera_world_and_actor_targets(self):
        for program in self.programs.values():
            self.assertIn('camera world targets passed', run([program, ROOT / 'tests/javascript/camera-look.js']).stdout)

    def test_bytes_come_back_whole_and_handles_are_bounded(self):
        # Every bytes return was empty while the typed array was made with one argument.
        for program in self.programs.values():
            self.assertIn('bytes and handles passed', run([program, ROOT / 'tests/javascript/bytes.js']).stdout)

    def test_exact_u64_structs_events_and_awaited_dialogs(self):
        for program in self.programs.values():
            self.assertIn('u64 exact native', run([program, ROOT / 'tests/javascript/u64.js']).stdout)

    def test_language_on_host_and_framework_libc(self):
        told = [run([p, ROOT / 'tests/javascript/language.js']).stdout for p in self.programs.values()]
        self.assertGreater(told[0].count('\n'), 40)
        self.assertEqual(told[0], told[1])

    def test_reflection_matches_the_lua_stand_in(self):
        from test_all import Generated, CORE, BASE, GEN
        import generate
        generate.write(GEN)
        fixture = Generated().fixture()
        program = self.out / ('reflection' + EXE)
        sources = [CORE / (n + '.cpp') for n in ('registry','ids','reflection','json','sha256','text','texts')]
        sources += sorted(BASE.glob('*.cpp')) + sorted((GEN / 'framework').glob('*.cpp'))
        sources += sorted((GEN / 'javascript').glob('*.cpp'))
        sources += [p for p in (ROOT / 'runtime/javascript').glob('*.cpp') if p.name != 'script_mods.cpp']
        sources += [ROOT / 'tests/host/no_dynamic_hooks.cpp', ROOT / 'tests/javascript/reflect_host.cpp', ROOT / 'framework/target/uint128.cpp']
        run([shutil.which('clang++'), *CXX, '-I', GEN / 'include', '-I', fixture / 'framework',
             '-I', GEN / 'framework', '-I', BASE, '-I', CORE, '-I', ROOT / 'tests/base',
             '-I', ROOT / 'runtime/javascript', '-isystem', quickjs_build.QUICKJS,
             *sources, *quickjs_build.objects(), '-o', program])
        run([program, ROOT / 'tests/javascript/reflect.js'])

    def script(self, source):
        path = self.out / 'limits.js'
        path.write_text(source, encoding='utf-8')
        return run([self.programs[True], path]).stdout

    def test_stack_overflow_is_distinct_from_memory_exhaustion(self):
        self.assertIn('deep stack overflow', self.script('''
let depth=0;
function deep(){ ++depth; return deep()+1; }
try {deep(); throw new Error("recursion returned");} catch(e) {
    if (!String(e).includes("stack overflow") || String(e).includes("memory") || depth<100) throw e;
    print("deep stack overflow");
}
'''))

    def test_instruction_budget_cannot_be_caught_or_reset_by_jobs(self):
        # The engine marks the interrupt exception uncatchable.
        import subprocess
        path = self.out / 'limits.js'
        for source in ('while(true){}', 'while(true){try{while(true){}}catch(e){}}',
                       'Promise.resolve().then(function spin(){Promise.resolve().then(spin);});'):
            path.write_text(source, encoding='utf-8')
            print('budget case:', source, flush=True)
            ran = subprocess.run([str(self.programs[True]), str(path)], capture_output=True, text=True, timeout=30)
            self.assertNotEqual(ran.returncode, 0)
            self.assertRegex(ran.stderr, 'interrupted|too long')

    def test_memory_cap_and_text_only_evaluation(self):
        self.assertIn('memory capped', self.script('''
try {const blocks=[]; while(true) blocks.push(new Uint8Array(1000000));}
catch(e) {if (!String(e).includes("out of memory")) throw e; print("memory capped");}
'''))
        self.assertIn('text only', self.script('''
try { eval("\\x02\\x00binary"); throw new Error("binary accepted"); }
catch(e) {if (e.name !== "SyntaxError") throw e;}
if (eval("2+3") !== 5) throw new Error("text failed"); print("text only");
'''))

    def test_schema_parity_and_documented_declarations(self):
        schema = load(ROOT / 'schema')
        files = js_binding.generate(schema)
        declarations = typescript.declaration(schema)
        for service in schema.services:
            if not service.has_bindings: continue
            binding = files[f'javascript/{service.name}_js.cpp']
            for fn in service.functions:
                if service.scripted(fn):
                    self.assertIn(f'"{fn.name}"', binding)
                    self.assertIn(f'    {fn.name}(', declarations)
                    for p in fn.params: self.assertIn(p.doc.replace('\n', ' '), declarations)
            for event in service.events:
                self.assertIn(f'"{event.name}"', binding)
                self.assertIn(f'on(event: "{event.name}"', declarations)
                for field in event.fields: self.assertIn(field.doc, declarations)
        self.assertIn('placement: bigint;', declarations)
        self.assertIn('actor?: ActorHandle | null;', declarations)
        self.assertIn('most: bigint', declarations)
        import package_sdk
        self.assertEqual(package_sdk.members()['sdk/tsuchi.d.ts'], declarations.encode('utf-8'))

if __name__ == '__main__': unittest.main()
