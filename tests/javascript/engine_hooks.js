// hook.engine_function against the made-up engine (tests/core/engine_hooks_test.cpp), the same as
// tests/lua/engine_hooks.lua, from JavaScript.
function check(condition, what) { if (!condition) throw new Error('failed: ' + what); }
function refused(fn, name) {
    try { fn(); } catch (error) { check(error.name === name, name + ', got ' + error.name); return; }
    throw new Error('expected ' + name);
}

refused(() => hook.engine_function('/Script/Test.Actor:Nope', {before() {}}), 'not_found');
refused(() => hook.engine_function('/Script/Test.Actor:Heal', {}), 'invalid');
refused(() => hook.engine_function('/Script/Test.Actor:Heal', {after: 3}), 'invalid');

// `before` changes an argument, `after` replaces the result.
const seen = {};
const healHook = hook.engine_function('/Script/Test.Actor:Heal', {
    before(call) {
        seen.object = call.object; seen.amount = call.args.Amount; seen.full = call.args.bFull;
        try { hook.remove(healHook); } catch (error) { seen.remove = error.name; }
        call.args.Amount = call.args.Amount * 2;
    },
    after(call) {
        seen.result = call.result;
        return call.result + 1;
    },
});
check(heal(5) === 23.5, 'heal ' + seen.result);
check(health() === 22.5, 'health');
check(seen.amount === 5 && seen.full === false && seen.remove === 'invalid', 'before saw the call');
check(seen.object !== null && seen.result === 22.5, 'after saw the result');

// A Promise is no result: the handler fails, its change is dropped, and the call goes on.
const later = hook.engine_function('/Script/Test.Actor:Heal', {
    before(call) { call.args.Amount = 1000; return Promise.resolve(); },
});
const errorsBefore = errors();
check(heal(1) === 25.5, 'a Promise dropped its change');
frame();
check(errors() === errorsBefore + 1 && last_error().includes('synchronously'), last_error());
hook.remove(later);
hook.remove(healHook);
check(heal(1) === 25.5, 'removed');

// Calls that skip the handlers.
let skipped = 0;
const counting = hook.engine_function('/Script/Test.Actor:Heal', {before() { skipped++; }});
const stats0 = stats();
threaded_heal();
scripted_heal();
const stats1 = stats();
check(skipped === 0 && stats1.off_thread === stats0.off_thread + 1 && stats1.script_arguments === stats0.script_arguments + 1, 'skipped');
hook.remove(counting);

// A struct result, and an argument the function writes.
const whereHook = hook.engine_function('/Script/Test.Actor:Where', {
    after(call) {
        seen.where = call.result; seen.found = call.args.Found;
        call.args.Found = null; // the function writes it, so the caller gets this
        return {X: 7, Y: 8, Z: 9};
    },
});
const [x, y, z, foundRed] = where();
check(x === 7 && y === 8 && z === 9 && !foundRed, 'where ' + [x, y, z, foundRed]);
check(seen.where.X === 1 && seen.where.Z === 3 && seen.found !== null, 'where seen');
hook.remove(whereHook);

// Text both ways.
const renameHook = hook.engine_function('/Script/Test.Actor:Rename', {
    before(call) { seen.title = call.args.Title; call.args.NewName = call.args.NewName + ' Changed'; },
});
check(rename('Captain', 'Sir') === 'Captain Changed|Sir' && seen.title === 'Sir', 'rename');
hook.remove(renameHook);

// A Blueprint's calls, with their arguments as script code.
const scriptCalls = stats().script_calls;
const bpHeal = hook.engine_function('/Script/Test.Actor:Heal', {
    before(call) { call.args.Amount = call.args.Amount * 2; },
    after(call) { return call.result + 1; },
});
const bpHealth = health();
check(blueprint_heal(5) === bpHealth + 11 && health() === bpHealth + 10, 'blueprint heal');
hook.remove(bpHeal);
const bpWhere = hook.engine_function('/Script/Test.Actor:Where', {after(call) { call.args.Found = null; }});
const [bx, , , bpFoundRed] = blueprint_where();
check(bx === 1 && !bpFoundRed, 'blueprint where');
hook.remove(bpWhere);
check(stats().script_calls === scriptCalls + 2, 'script calls counted');

// An event that runs as an entry point of its class's event graph.
let pokes = 0;
hook.engine_function('/Script/Test.Creature:OnPoke', {after(call) { check(Object.keys(call.args).length === 0 && call.result === null, 'event call'); pokes++; }});
check(event(true) % 1000 === 42 && pokes === 1, 'poke');
check(event(false) % 1000 === 7 && pokes === 1, 'other event');
