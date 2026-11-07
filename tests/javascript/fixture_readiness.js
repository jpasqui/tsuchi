// The Python selector inserts the actual fixture and its imports into this production QuickJS host.
const originalPrint = print;
const fixtureLines = [];
let fixtureStep = null, readiness = "unloaded", listCalls = 0;
const unexpected = new Error("unexpected fixture service failure");
unexpected.name = "invalid";
const encoded = new Uint8Array(encodeURIComponent(language()).match(/%[0-9A-F]{2}|[^%]/g)
    .map(piece => piece[0] === "%" ? parseInt(piece.slice(1), 16) : piece.charCodeAt(0)));
globalThis.print = (...values) => fixtureLines.push(values.join(" "));
globalThis.mod = { framework: () => 0, id: () => "check.javascript", version: () => "1.0.0", version_of: () => "1.4.2" };
globalThis.file = { size: () => BigInt(encoded.length), read: () => encoded };
globalThis.game = { world: () => "Area001_P" };
globalThis.time = { on: (name, handler) => {
    if (name !== "step") throw new Error("wrong fixture event");
    fixtureStep = handler;
} };
globalThis.entity = { list: () => {
    ++listCalls;
    if (readiness === "unloaded") {
        const error = new Error("No world is loaded.");
        error.name = "not_ready";
        throw error;
    }
    if (readiness === "invalid") throw unexpected;
    return new Array(readiness === "small" ? 99 : 120).fill({});
} };

// ACTUAL_FIXTURE_HERE

function fixtureCheck(value, message) { if (!value) throw new Error(message); }
function fixtureSteps(count) { for (let i = 0; i < count; ++i) fixtureStep(); }
fixtureCheck(typeof fixtureStep === "function", "actual fixture did not subscribe");
fixtureCheck(fixtureLines.includes("helper 42") && fixtureLines.includes("language same"), "actual fixture startup failed");
fixtureSteps(30);
fixtureCheck(listCalls === 1 && fixtureLines.includes("frames 30"), "unloaded world did not retry");
fixtureCheck(!fixtureLines.some(line => line.startsWith("actors ")), "unloaded world reported actors");
readiness = "invalid";
let observed = null;
try { fixtureSteps(30); } catch (error) { observed = error; }
fixtureCheck(observed === unexpected && listCalls === 2, "unexpected service error was swallowed");
readiness = "small";
fixtureSteps(30);
fixtureCheck(listCalls === 3 && !fixtureLines.some(line => line.startsWith("actors ")), "small world stopped readiness retry");
readiness = "ready";
fixtureSteps(30);
fixtureCheck(listCalls === 4 && fixtureLines.includes("actors 120 in Area001_P"), "ready world did not report actors");
fixtureSteps(60);
fixtureCheck(listCalls === 4 && fixtureLines.filter(line => line.startsWith("actors ")).length === 1, "fixture repeated its completed actor check");
originalPrint("actual SDK JavaScript readiness and unexpected-error visibility passed");
