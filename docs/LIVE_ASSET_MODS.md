# Live downloaded asset choices

Saving an asset-only download's On/Off choice can change its archive in the running
game on Pikmin 4 1.1.1. Removing that download also retires its archive and removes
its Mods row immediately. Scripts, compiled code and cheats retain their existing
restart rules.

The switch shows Reload while an affected package is still loaded. Close the
relevant map or menu; once its package objects unload, Tsuchi completes the
unmount automatically. Turning it on again cancels a waiting unmount. Assets
already held by the game are never destroyed by a toggle. Some globally cached
assets can remain loaded for the rest of the session.

The initial backend supports IoStore containers with an empty legacy pak, no
shader library, and no localization or cross-package redirects. Other formats
keep the Restart state. Other game builds explicitly lack this backend until
their native layouts are qualified.

Unmounting detaches the reader's immutable chunk lookup map under the engine's
reader lock, and changes package metadata under its package-store lock. Tsuchi
retains the original lookup map, reader indices, compression data, cache identities
and file handles. Pending reads can finish; re-enabling restores the same map.
Removed files that remain held are cleaned up on the next process start.

Each mod layer records the stock metadata pointer and its own override. Turning
off an earlier layer leaves a later active override in place; turning off the last
active override restores stock. Current metadata slots are looked up by package
identity because another mount can relocate the engine's map. All writes are
preflighted together. Unexpected metadata ownership refuses the change.

Pending changes use at most one cached package-name hash lookup per frame. They
perform no filesystem scans. Native qualification is opt-in through the existing
self-check build with the check.live-assets fixture; production builds omit it.
