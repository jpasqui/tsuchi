"""Stage the networking proof mod for an isolated game run."""
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def stage():
 out=ROOT/'build/net-proof';mod=out/'romfs/tsuchi/mods/proof.network';mod.mkdir(parents=True,exist_ok=True)
 (mod/'mod.json').write_text(json.dumps({'id':'proof.network','name':'Network proof','version':'1.0.0','framework':0},indent=2)+'\n',encoding='utf-8')
 (mod/'main.lua').write_text((ROOT/'tests/game/network_proof.lua').read_text(encoding='utf-8'),encoding='utf-8')
 return out
if __name__=='__main__':print(stage())
