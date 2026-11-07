// Bytes come back whole, and handles stay one object each while held and are let go after.
function check(ok, what) { if (!ok) throw new Error("failed: " + what); }
const whole = file.read("known.bin", 0n, 64n);
check(whole instanceof Uint8Array, "a Uint8Array");
check(whole.length === 9 && whole.byteLength === 9, "all nine bytes, not " + whole.length);
check([...whole].join(",") === "84,115,117,0,255,128,10,106,115", "the file's bytes: " + [...whole].join(","));
const part = file.read("known.bin", 3n, 3n);
check(part.length === 3 && part[0] === 0 && part[1] === 255 && part[2] === 128, "three bytes from the fourth");
check(file.read("known.bin", 9n, 4n).length === 0, "nothing past the end");
check(file.read("known.bin", 0n, 0n).length === 0, "nothing when nothing is asked for");

const kept = test.actor(1n);
check(kept === test.actor(1n), "the same handle is the same object");
const before = test.handles();
// In a function, so the script's completion value does not keep the last one.
(() => { for (let i = 0; i < 100000; ++i) test.actor(BigInt(i + 10)); })();
check(test.handles() === before, "dropped handles are let go: " + test.handles() + " held, " + before + " before");
const many = [];
for (let i = 0; i < 1000; ++i) many.push(test.actor(BigInt(i + 10)));
check(test.handles() === before + 1000, "held handles are kept");
check(many[500] === test.actor(510n), "identity holds for a handle in a list");
many.length = 0;
check(test.handles() === before, "handles go when the list does");
print("bytes and handles passed");
