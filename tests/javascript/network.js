function check(value) { if (!value) throw new Error("request binding check failed"); }
let finished = 0;
for (const method of ["post", "put", "patch", "remove"]) {
    const payload = new Uint8Array([123, 0, 195, 169, 125]);
    const pending = net.request(method, "https://example.org/runs",
        "Content-Type: application/json\r\nAuthorization: Bearer fixture\r\n", payload, 65536n, 1000);
    check(net.request_info(pending.request).state === "queued");
    pending.then(event => {
        check(event.request === pending.request && event.state === "succeeded" && event.http_status === 200);
        const info = net.request_info(event.request);
        check(info.received === 5n);
        const data = net.read(event.request, 0n, 16n);
        check(data.length === payload.length && data.every((n, i) => n === payload[i]));
        net.release_request(event.request); ++finished;
    });
    check(test.request_done(pending.request));
}
check(finished === 4);
print("HTTPS request bindings passed");
