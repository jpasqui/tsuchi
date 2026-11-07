// Same stand-in and cases as tests/base/reflect.lua.
function check(v, what) { if (!v) throw new Error((what || "check failed") + " " + new Error().stack); }
function fails(code, fn) { try { fn(); } catch(e) { return e.name === code; } return false; }
const Creature=reflection.find_class("Creature"), Actor=reflection.find_class("Actor");
check(Creature && Actor && Creature !== Actor);
check(reflection.class_of(red) === Creature);
check(reflection.is_a(red,Actor) && !reflection.is_a(body,Actor));
check(reflection.name(red)==="Red" && reflection.name(Creature)==="Creature");
check(fails("not_found",()=>reflection.find_class("Onion")));
check(reflection.get(red,"Health")===12.5 && typeof reflection.get(red,"Health")==="number");
check(reflection.get(red,"Lives")===-3n);
check(reflection.get(red,"bCanBeDamaged") && !reflection.get(red,"bHidden"));
check(reflection.get(red,"Tag")==="Leader" && reflection.get(red,"Label")==="Olimar é🌱");
check(reflection.get(red,"Root")===body && reflection.get(red,"Root.Scale")===2);
let location=reflection.get(red,"Location"); check(location.X===0 && location.Y===7 && location.Z===0);
let scores=reflection.get(red,"Scores"); check(scores.length===3 && scores[0]===10n && scores[2]===30n);
let points=reflection.get(red,"Points"); check(points.length===2 && points[1].Z===9 && points[0].Z===0);
let tags=reflection.get(red,"Tags"); check(tags.length===2 && tags[0]==="Big" && tags[1]==="Night_2");
check(reflection.get(red,"Corners")[1].Y===5);
for (const [code,path] of [["not_found","Nope"],["invalid","Tags["],["unsupported","Lookup"],["unsupported","Odd"],["not_found","a".repeat(600)],["invalid","a".repeat(5000)]]) check(fails(code,()=>reflection.get(red,path)),path);
check(reflection.set(red,"Health",3) && reflection.get(red,"Health")===3);
check(reflection.set(red,"Health",40.25) && reflection.get(red,"Health")===40.25);
check(fails("invalid",()=>reflection.set(red,"Lives",2.5)) && reflection.get(red,"Lives")===-3n);
check(fails("invalid",()=>reflection.set(red,"Health","a lot")));
check(reflection.set(red,"bHidden",true) && reflection.get(red,"bHidden"));
check(reflection.set(red,"Tag","Chief") && reflection.get(red,"Tag")==="Chief");
check(reflection.set(red,"Label","Louie é") && reflection.get(red,"Label")==="Louie é");
// JS text is Unicode; the equivalent invalid UTF-8 input is a lone surrogate.
check(fails("invalid",()=>reflection.set(red,"Label","bad\ud800")) && reflection.get(red,"Label")==="Louie é");
check(fails("invalid",()=>reflection.set(red,"Label","cut\0short")));
check(reflection.set(red,"Root",spare) && reflection.get(red,"Root")===spare);
check(fails("invalid",()=>reflection.set(red,"Root",red)));
check(fails("invalid",()=>reflection.set(red,"Root",new Date())));
check(reflection.set(red,"Root",null) && reflection.get(red,"Root")===null);
check(reflection.set(red,"Root",body));
check(reflection.get(red,"Target")===null && reflection.set(red,"Target",spare) && reflection.get(red,"Target")===spare);
check(reflection.get(red,"Target.Scale")===reflection.get(spare,"Scale") && reflection.set(red,"Target",null) && reflection.get(red,"Target")===null);
check(reflection.set(red,"Location",{X:1,z:3})); location=reflection.get(red,"Location"); check(location.X===1 && location.Y===7 && location.Z===3);
check(fails("not_found",()=>reflection.set(red,"Location",{W:1})));
check(fails("invalid",()=>reflection.set(red,"Location",{1:1})));
check(fails("unsupported",()=>reflection.set(red,"Location",5)));
check(fails("invalid",()=>reflection.set(red,"Health",{X:1})));
check(reflection.set(red,"Tags",["A","B","C"])); tags=reflection.get(red,"Tags"); check(tags.length===3 && tags[0]==="A" && tags[2]==="C");
check(reflection.set(red,"Tags",[]) && reflection.get(red,"Tags").length===0);
check(reflection.set(red,"Points",[{X:1},{Y:2},{Z:3}])); points=reflection.get(red,"Points"); check(points.length===3 && points[0].X===1 && points[1].Y===2 && points[2].Z===3 && points[2].X===0);
check(reflection.set(red,"Notes",["one","two"]) && reflection.get(red,"Notes")[1]==="two");
check(reflection.set(red,"Notes",["only"]) && reflection.get(red,"Notes").length===1);
check(reflection.set(red,"Scores",[1,2,3]) && reflection.get(red,"Scores")[1]===2n);
check(fails("invalid",()=>reflection.set(red,"Scores",[1,2])) && fails("unsupported",()=>reflection.set(red,"Odd",[1])));
// Native validation and script conversion failures leave complete old values.
check(fails("invalid",()=>reflection.set(red,"Scores",[99,2147483648n,77])));
scores=reflection.get(red,"Scores"); check(scores.length===3 && scores[0]===1n && scores[1]===2n && scores[2]===3n);
check(fails("RangeError",()=>reflection.set(red,"Scores",[99,9223372036854775808n,77])));
check(reflection.get(red,"Scores")[0]===1n);
check(fails("invalid",()=>reflection.set(red,"Notes",["changed","bad\ud800","third"])));
let notes=reflection.get(red,"Notes"); check(notes.length===1 && notes[0]==="only");
check(fails("invalid",()=>reflection.set(red,"Points",[{X:99},{Y:88},{Z:"bad"},{X:77}])));
points=reflection.get(red,"Points"); check(points.length===3 && points[0].X===1 && points[1].Y===2 && points[2].Z===3);
check(fails("invalid",()=>reflection.set(red,"Notes",["changed","cut\0short"])));
check(reflection.get(red,"Notes").length===1 && reflection.get(red,"Notes")[0]==="only");
let firstReads=0, laterReads=0;
check(fails("Error",()=>reflection.set(red,"Location",{
    get X(){ ++firstReads; return 99; },
    get Z(){ ++laterReads; throw new Error("late getter"); }
})));
check(firstReads===1 && laterReads===1 && reflection.get(red,"Location.X")===1 && reflection.get(red,"Location.Z")===3);
let successfulReads=0;
check(reflection.set(red,"Location",{get X(){ ++successfulReads; return 1; }}));
check(successfulReads===1,"a getter is evaluated once, before native validation");
const cyclic={X:99}; cyclic.Y=cyclic;
check(fails("unsupported",()=>reflection.set(red,"Location",cyclic)) && reflection.get(red,"Location.X")===1);
// A thrown argument getter never invokes the function or leaves a call open.
check(fails("Error",()=>reflection.call(red,"Heal",{Amount:90,get bFull(){throw new Error("argument getter");}})));
check(reflection.get(red,"Health")===40.25);
check(fails("capacity",()=>reflection.set(red,"Notes",new Array(1000001))));
check(reflection.get(red,"Notes").length===1 && reflection.get(red,"Notes")[0]==="only");
check(fails("invalid",()=>reflection.set(red,"Location",{X:99,x:88})) && reflection.get(red,"Location.X")===1);
check(reflection.set(red,"Health",10));
let [result,written]=reflection.call(red,"Heal",{Amount:2.5}); check(result===12.5 && Object.keys(written).length===0 && reflection.get(red,"Health")===12.5);
check(reflection.call(red,"Heal",{bFull:true})[0]===100 && reflection.call(red,"Describe")[0]==="Red Bulborb");
let where; [where,written]=reflection.call(red,"Where"); check(where.X===1 && where.Y===2 && where.Z===3 && written.Found===red && written.ReturnValue===undefined);
const box=reflection.call(red,"Measure")[0]; check(box.Width===3 && box.Height===2);
check(reflection.call(Creature,"Spawn",{Count:21})[0]===42n && reflection.call(red,"Spawn",{Count:4})[0]===8n);
const anger=reflection.get(red,"Anger"); [result,written]=reflection.call(red,"Jump"); check(result===null && typeof written==="object" && reflection.get(red,"Anger")===anger+1n);
check(reflection.call(red,"Sum",{Values:[1,2,3]})[0]===6n && reflection.call(red,"Sum",{Values:[]})[0]===0n);
[result,written]=reflection.call(red,"Rename",{NewName:"Skipper",Title:"Captain"}); check(result===null && Object.keys(written).length===0 && reflection.get(red,"Tag")==="Skipper");
const long="L".repeat(300); check(reflection.get(wordy,long)===5n); const wide=reflection.get(wordy,"Shape"); check(wide[long]===7n && wide.After===9n);
check(reflection.set(wordy,"Shape",{[long]:8}) && reflection.get(wordy,"Shape."+long)===8n);
[result,written]=reflection.call(wordy,long); check(result===null && written[long]===41n && written.Tail===42n);
for (const [code,fn] of [["not_found",()=>reflection.call(red,"Fly")],["not_found",()=>reflection.call(red,"Heal",{Speed:1})],["invalid",()=>reflection.call(red,"Heal",{Amount:"plenty"})],["invalid",()=>reflection.call(red,"Heal",{1:2})],["not_found",()=>reflection.call(body,"Heal")]]) check(fails(code,fn));
for (const fn of [()=>reflection.call(red,"Heal",5),()=>reflection.call(red),()=>reflection.get(5,"Health"),()=>reflection.get(red),()=>reflection.set(red,"Health"),()=>combat.life(body)]) check(fails("TypeError",fn));
print("reflection same stand-in passed");