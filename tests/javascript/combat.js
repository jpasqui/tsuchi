// Same cases as tests/host/combat.lua, using JavaScript values and exceptions.
function check(value) { if (!value) throw new Error("check failed " + new Error().stack); }
function throws(code, fn) { try { fn(); } catch (e) { check(e.name === code); return e; } throw new Error("expected " + code); }
const olimar = test.actor(1n), louie = test.actor(2n), bulborb = test.actor(3n);
check(olimar === test.actor(1n) && olimar !== louie && test.actor(0n) === null);
const seen = new Map([[olimar,true]]); check(seen.get(test.actor(1n)) && !seen.has(louie));
check(typeof olimar === "object" && Object.keys(olimar).length === 0);
const life = combat.life(louie); check(life.life === 100 && life.max_life === 100 && life.extra === 20 && life.max_extra === 20);
const missing = throws("not_found", () => combat.life(test.actor(99n)));
check(missing instanceof Error && missing.message === "The actor no longer exists or has no health. Pikmin don't have health.");
throws("invalid", () => combat.set_life(olimar,-1));
throws("unsupported", () => combat.knockback(bulborb,{x:0,y:0,z:100}));
for (const fn of [() => combat.life(1), () => combat.life(null), () => combat.damage(olimar,null,10,"lava"),
    () => combat.set_life(olimar,50,["gradual"]), () => combat.knockback(olimar,null,[1,2,3])]) throws("TypeError",fn);
check(combat.set_life(olimar,80,["smooth_gauge"]) && test.last().flags === 1 && combat.life(olimar).life === 80);
check(combat.set_life(olimar,100) && test.last().flags === 0);
check(combat.knockback(olimar,{x:1,y:2,z:300},5,bulborb));
const velocity = test.last().velocity; check(velocity.x === 1 && velocity.y === 2 && velocity.z === 300);
const hits = [], downs = [];
const on_hit = combat.on("damaged", e => hits.push(e)), on_down = combat.on("defeated", e => downs.push(e));
check(test.subscriptions() === 2);
check(combat.damage(olimar,30,"fire",bulborb) && test.last().kind === 3 && hits.length === 1 && downs.length === 0);
check(hits[0].actor === olimar && hits[0].attacker === bulborb && hits[0].previous === 100 && hits[0].life === 70);
check(combat.damage(olimar,70) && test.last().kind === 0 && hits.length === 2 && hits[1].attacker === null);
check(downs.length === 1 && downs[0].actor === olimar && downs[0].life === 0);
check(on_hit.cancel() && test.subscriptions() === 1); throws("not_found", () => on_hit.cancel());
check(combat.damage(louie,1) && hits.length === 2); on_down.cancel();
let calls = 0;
combat.on("damaged", () => { ++calls; throw {}; }); combat.on("damaged", () => { ++calls; throw new Error("plain"); });
check(combat.damage(louie,1) && calls === 2 && test.errors() === 2);
let once; once = combat.on("damaged", () => once.cancel());
const before = test.subscriptions(); check(combat.damage(louie,1) && test.subscriptions() === before-1);
throws("invalid", () => combat.set_life(louie,NaN)); throws("invalid", () => combat.knockback(louie,{x:0,y:Infinity,z:0}));
check(test.logged() === 0); test.deprecated("combat.old_call",3,"combat.new_call");
check(test.logged() === 1); test.deprecated("combat.old_call",3,"combat.new_call"); check(test.logged() === 1);
test.deprecated("combat.on_old_event",4,"combat.new_event"); check(test.logged() === 2);
globalThis.handler_calls = 0; combat.on("damaged", () => ++handler_calls);
check(combat.damage(louie,1) && handler_calls === 1);
globalThis.frames_seen = 0; globalThis.pictures_seen = 0; globalThis.steps_at_last_frame = 0; globalThis.last_frame_number = 0n;
time.on("step", step => { check(pictures_seen === frames_seen && step.seconds === undefined); ++frames_seen; steps_at_last_frame = test.steps(); last_frame_number = step.number; });
time.on("presented", picture => { ++pictures_seen; check(pictures_seen === frames_seen && picture.seconds === 0.5 && picture.fraction === 1); });
// A handler exhausted in its own call is reported once and disabled on later frames.
let slowCalls = 0;
const errorsBeforeSlow = test.errors();
time.on("step", () => { ++slowCalls; while (true) {} });
globalThis.verifySlow = () => check(slowCalls === 1 && test.errors() === errorsBeforeSlow + 1);
