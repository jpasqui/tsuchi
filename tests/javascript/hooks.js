function assert(value) { if (!value) throw new Error('hook assertion failed'); }
const target = {entry_at:entry, stock:0n, signature:'u64(u64,u64)'};
function remove(h) { hook.remove(h); frame(); }
let saved;
let h = hook.wrap(target, (original,a,b) => {
    saved = original;
    assert(a === 0xfedcba9876543210n && b === 2n);
    let refused = false;
    try { hook.wrap(target, () => 0n); } catch(e) { refused = e.name === 'invalid'; }
    assert(refused);
    return original(a,b) + 10n;
});
assert(call_game(0xfedcba9876543210n,2n) === 0xfedcba987654321cn);
let expired = false; try { saved(1n,2n); } catch(e) { expired = true; } assert(expired);
assert(call_game(1n,2n,true) === 3n); remove(h);
let before = calls(), told = errors();
h = hook.wrap(target, () => { throw 'before original'; });
assert(call_game(2n,3n) === 5n && calls() === before+1 && errors() === told);
frame(); assert(errors() === told+1); remove(h);
h = hook.wrap(target, (original,a,b) => { original(a,b); original(8n,9n); throw 'after original'; });
before = calls(); assert(call_game(2n,3n) === 17n && calls() === before+2); remove(h);
h = hook.wrap(target, original => ({get then(){original(8n,9n);}})); assert(call_game(2n,3n) === 5n); remove(h);
h = hook.wrap(target, () => 'wrong type'); assert(call_game(2n,3n) === 5n); remove(h);
let ranJob = false;
h = hook.wrap(target, original => { Promise.resolve().then(() => {ranJob = true;}); return 99n; });
assert(call_game(2n,3n) === 99n && !ranJob); remove(h);
for (const fn of [async () => 9n, () => ({then(){}}), () => ({get then(){throw 'getter';}})]) {
    h = hook.wrap(target, fn); assert(call_game(2n,3n) === 5n); remove(h);
}
// Even a void hook must refuse asynchronous results and run its original once.
h = hook.wrap({...target,signature:'void(u64,u64)'}, async () => {});
before = calls(); call_game(2n,3n); assert(calls() === before+1); remove(h);

let records = [];
h = hook.watch(target, r => {records.push(r);});
assert(call_game(4n,5n) === 9n && call_game(6n,7n,true) === 13n && records.length === 0);
frame(); assert(records.length === 2 && records[0].args[0] === 4n && records[0].result === 9n && records[1].thread === 'other');
for (let i=0;i<260;i++) call_game(BigInt(i),1n);
frame(); assert(records.length === 258 && records[2].dropped === 4); remove(h);
// Plain code checks and patches go to the same backend as a compiled mod's.
assert(hook.check_code(0x900n, new Uint8Array([0xe8,0x97,0x9f,0x1a])) === true);
let refused = null; try { hook.patch_instruction(0x900n, 0x1a9f97e8, 0xd503201f); } catch (e) { refused = e.name; }
assert(refused === 'not_ready');
for (const [code, t] of [['not_found', {slot:'no_such_slot', signature:'u64(u64)'}], ['invalid', {slot:'engine_tick', entry_at:0x10n, signature:'u64(u64)'}]]) {
    let name = null; try { hook.wrap(t, () => 0n); } catch (e) { name = e.name; }
    assert(name === code);
}
let replacement;
h = hook.watch(target, () => {hook.remove(h); replacement = hook.wrap(target, () => 77n);});
call_game(1n,2n); frame(); assert(call_game(1n,2n) === 77n); remove(replacement);

const text_target = {entry_at:text_entry,stock:0n,signature:'u64(text16)'};
h = hook.wrap(text_target,(original,text)=>{assert(text === null || text === 'A🚀'); return original(text);});
assert(call_text(false) === 65n && call_text(true) === 0n); remove(h);
let text_record;
h = hook.watch(text_target,r=>{text_record=r;});
assert(call_text(false) === 65n); frame(); assert(text_record.args[0] === 'A🚀' && text_record.result === 65n); remove(h);

let kept;
h = hook.instruction(0x900n,0x1a9f97e8, r => {kept=r; assert(r.x[0] === 0xfedcba9876543210n); r.x[0]=0xffffffffffffffffn; r.z=true; r.c=false;});
assert(call_instruction() === 0x40000000n); kept.z=false; assert(call_instruction() === 0x40000000n); remove(h);
for (const fn of [r => {r.z=true; r.x[30]='bad';}, r => {r.z=true; throw 'discard';}, async r => {r.z=true;}, r => {r.z=true; Object.defineProperty(r,'x',{get(){throw 'getter';}});}]) {
    h = hook.instruction(0x900n,0x1a9f97e8,fn); assert(call_instruction() === 0x20000000n); remove(h);
}
// Text passed on unchanged reaches the game as its own pointer; changed text is a copy. `stock` can be left out.
const pointer_target = {entry_at:pointer_entry, signature:'u64(text16)'};
h = hook.wrap(pointer_target,(original,text)=>original(text)); assert(same_pointer() === 1n); remove(h);
h = hook.wrap(pointer_target,(original,text)=>original(text+'!')); assert(same_pointer() === 0n); remove(h);
// An Error's own message reaches the log; a message behind a getter is never read.
h = hook.wrap(target, () => { throw new Error('kept message'); });
call_game(2n,3n); frame(); assert(last_error().includes('kept message')); remove(h);
let getter_ran = false;
h = hook.wrap(target, () => { const e = new Error('x'); Object.defineProperty(e,'message',{get(){getter_ran=true; return 'from getter';}}); throw e; });
call_game(2n,3n); frame(); assert(!getter_ran && !last_error().includes('from getter')); remove(h);
let unsupported=false;
try { hook.wrap({...target,signature:'u64(u64,u64,u64,u64,u64,u64,u64,u64,u64)'},()=>0n); } catch(e) {unsupported=e.name==='unsupported';}
assert(unsupported);
hook.wrap(target, () => {while(true){try{while(true){}}catch(e){}}});
