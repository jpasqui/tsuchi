export function language() {
// JavaScript counterparts of the language features in tests/lua/language.lua.
function check(v) { if (!v) throw new Error("language check failed"); }
const lines = [];
const tell = (...v) => lines.push(v.map(String).join("\t"));
let sum=0; for (let i=1;i<=10;++i) sum+=i; tell("loop",sum);
let j=0; while(j<3) ++j; do { --j; } while(j>0); tell("while",j);
const closure = x => y => x+y; tell("closure",closure(20)(22));
function spread(...values) { return values.reduce((a,b)=>a+b,0); } tell("spread",spread(...[1,2,3]));
const record={x:1,y:2}; const {x,y}=record; tell("object",x,y,Object.keys(record).sort());
const array=[1,2,3]; tell("array",array.map(x=>x*2),array.filter(x=>x>1),array.reduce((a,b)=>a+b));
tell("sort",[9,2,7,1].sort((a,b)=>a-b));
class Leader { constructor(name) {this.name=name;} speak(){return this.name;} }
class Dog extends Leader { speak(){return super.speak()+"!";} } tell("class",new Dog("Oatchi").speak());
const proxy=new Proxy({x:2},{get:(o,k)=>o[k]*3}); tell("proxy",proxy.x);
const key=Symbol("leaf"); record[key]=3; tell("symbol",record[key],Object.getOwnPropertySymbols(record).length);
const map=new Map([[record,7]]); tell("map",map.get(record),new Set([1,1,2]).size);
function* steps(){yield 1;yield 2;return 3;} tell("generator",[...steps()]);
try {throw new Error("caught");} catch(e) {tell("error",e.name,e.message);} finally {tell("finally",true);}
tell("eval",eval("2+3"),new Function("x","return x*2")(21));
tell("text","Olimar é🌱".length,"Olimar é🌱".includes("🌱"),"ABC".toLowerCase(),"abc".toUpperCase());
tell("unicode","e\u0301".normalize(),[..."🌱"].length);
tell("regexp",/\p{Letter}+/u.exec("123éABC")[0],"abc123".replace(/\d+/g,"x"));
tell("json",JSON.stringify(JSON.parse('{"leaf":true,"x":[1,2]}')));
const bytes=new Uint8Array([0,255,128]); const view=new DataView(bytes.buffer); tell("bytes",[...bytes],view.getUint16(1));
const ints=new BigUint64Array([0n,9007199254740993n,18446744073709551615n]); tell("u64array",[...ints]);
tell("bigint",2n**100n,(2n**100n)/3n,(2n**100n)%3n,BigInt.asUintN(64,-1n));
tell("date",new Date(Date.UTC(2024,1,29)).toISOString());
for (const v of [0,-0,0.1,1e-200,1e200,Number.MIN_VALUE,Number.MAX_VALUE,NaN,Infinity,-Infinity]) {
    tell("number",v,String(v),Object.is(v,-0));
    if (Number.isFinite(v)) check(Object.is(Number(String(v)),v) || Object.is(v,-0));
}
for (const v of [0,0.125,0.5,1,2,10,-0.25]) {
    tell("math",v,...[Math.sin,Math.cos,Math.tan,Math.atan,Math.exp,Math.log,Math.sqrt,Math.cbrt,Math.sinh,Math.cosh,Math.tanh,Math.asinh,Math.expm1,Math.log1p].map(fn=>Number(fn(v).toPrecision(10))));
}
tell("round",Math.floor(-1.2),Math.ceil(-1.2),Math.trunc(-1.2),Math.round(-1.5),Math.hypot(3,4));
check(Math.hypot(3,4)===5 && Object.is(Math.trunc(-0.1),-0));
check(Number.isNaN(Math.acosh(0)) && Number.isNaN(Math.atanh(2)) && Math.atanh(1)===Infinity);
tell("promise",42);
return lines.join("\n");
}
