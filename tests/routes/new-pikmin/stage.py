"""Stage the isolated native save acceptance route into one reused lane output."""
import argparse, json, re, shutil, uuid
from pathlib import Path
root=Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('phase',choices=['spawn','reload','missing'])
parser.add_argument('--prekill-nonce',default=None)
args=parser.parse_args()
nonce=args.prekill_nonce or uuid.uuid4().hex
if not re.fullmatch('[0-9a-f]{32}',nonce): parser.error('prekill nonce must contain exactly32 lowercase hexadecimal digits')
source=Path(__file__).resolve().parent
output=root/'build/new-pikmin'
mods=output/'romfs/tsuchi/mods'
provider=mods/'route.new-pikmin'
observer=mods/'route.new-pikmin-proof'
observer.mkdir(parents=True,exist_ok=True)
if args.phase=='missing':
    target=provider.resolve()
    if target.parent!=mods.resolve() or target.name!='route.new-pikmin': raise ValueError('provider target escaped owned mod folder')
    if target.exists(): shutil.rmtree(target)
else:
    provider.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(source/'mod.json',provider/'mod.json')
    for name in ('main.lua','phase.txt','check.lua'):
        path=provider/name
        if path.exists(): path.unlink()
shutil.copyfile(source/'observer.json',observer/'mod.json')
(observer/'phase.txt').write_text(args.phase+'\n',encoding='utf-8')
shutil.copyfile(source/'main.lua',observer/'main.lua')
shutil.copyfile(source/'prekill_gate.lua',observer/'prekill_gate.lua')
(observer/'prekill-nonce.txt').write_text(nonce+'\n',encoding='utf-8')
shutil.copyfile(root/'sdk/check/check.lua',observer/'check.lua')
checks=['admitted','spawned','parameters','stock','squad','asked_amber','throw_amber','asked_azure','throw_azure','killed','death','survivors','departure','transition'] if args.phase=='spawn' else ['restored','stock','survivors']
(output/'checks.json').write_text(json.dumps({'check_mods':{'route.new-pikmin-proof':'new-pikmin'},'checks':{'route.new-pikmin-proof':checks}},indent=2)+'\n',encoding='utf-8')
print(output)
