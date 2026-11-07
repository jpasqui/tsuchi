import { double } from "./lib/helper.js";
import { language } from "./language.js";

if (!Number.isInteger(mod.framework()) || mod.framework() < 0) throw new Error("framework version");
print("started " + mod.id() + " " + mod.version() + " with " + mod.version_of("check.library"));
print("helper " + double(21));
const expectedBytes = file.read("language.txt", 0n, file.size("language.txt"));
const expected = decodeURIComponent(Array.from(expectedBytes, b => "%" + b.toString(16).padStart(2, "0")).join(""));
print(language() === expected ? "language same" : "language differs");
try { throw new Error("this"); } catch (error) { print("caught " + error.message); }

let frames = 0, toldActors = false;
time.on("step", () => {
    ++frames;
    if (frames === 30) print("frames 30");
    if (!toldActors && frames % 30 === 0) {
        let actors;
        try {
            actors = entity.list();
        } catch (error) {
            if (error.name === "not_ready") return;
            throw error;
        }
        if (actors.length >= 100) {
            toldActors = true;
            print("actors " + actors.length + " in " + game.world());
        }
    }
});
