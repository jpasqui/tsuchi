const point={x:1,y:2,z:3};
if(camera.hold_look(0,{point})!==true) throw new Error("world target with omitted actor");
if(camera.hold_look(0,{actor:null,point})!==true) throw new Error("world target with null actor");
if(camera.hold_look(0,{actor:test.actor(1n),point})!==true) throw new Error("attached target");
try {
    camera.hold_look(0,{actor:1,point});
    throw new Error("numeric actor accepted");
} catch(e) {
    if(e.name!=="TypeError") throw e;
}
print("camera world targets passed");
