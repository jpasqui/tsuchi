function check(value) { if (!value) throw new Error("check failed"); }
function refuses(fn) { try { fn(); } catch (e) { check(e instanceof Error); return; } throw new Error("accepted bad input"); }
const ids = [0n,1n,9007199254740991n,9007199254740993n,9223372036854775807n,9223372036854775808n,18446744073709551615n];
for (const id of ids) {
    check(precision.capture(id) === id && test.captured() === id);
    check(precision.roundtrip({placement:id}).placement === id && test.captured() === id);
    let event_id;
    const sub = entity.on("spawned", e => { event_id = e.placement; check(precision.capture(e.placement) === id); });
    test.emit(id); check(event_id === id && test.captured() === id); sub.cancel();
}
check(precision.with_default() === 18446744073709551615n);
for (const value of [-1n,18446744073709551616n,0,9007199254740993,1.5,"9007199254740993",null,{},true]) refuses(() => precision.capture(value));
refuses(() => precision.bounded(4294967296)); refuses(() => precision.bounded(-1));
refuses(() => precision.bounded(1.5)); check(precision.bounded(4294967295) === 4294967295);
// An answered dialog resumes the await continuation on the same runtime.
let answer;
const promise = dialog.confirm("Shall we?");
check(promise instanceof Promise && typeof promise.dialog === "object");
(async () => { answer = await promise; })();
check(answer === undefined); test.answer(promise.dialog,true);
check(answer.accepted === true && answer.dialog === promise.dialog);
const canceled = dialog.message("Done."); let second;
(async () => { second = await canceled; })(); test.answer(canceled.dialog,false); check(second.accepted === false);
// Several dialogs in one turn each keep their own resolver, and completed waits don't add another listener.
const first = dialog.confirm("First"), next = dialog.confirm("Next");
let a,b; (async () => { a=await first; })(); (async () => { b=await next; })();
test.answer(next.dialog,true); test.answer(first.dialog,false); check(a.accepted === false && b.accepted === true);
print("u64 exact native calls, results, structs, events, refusals, and awaits passed");