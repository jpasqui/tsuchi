// A mod's cooked Unreal content: one container in the mod's folder, `<name>.pak` with
// `<name>.utoc` and `<name>.ucas` beside it, as the engine's own tools write them.
//
// The framework mounts the container of each mod that can run, once, before the game
// loads anything from it: on the engine's first frame, before the title screen and
// before any save is read. Mods are mounted in the order they start, dependencies
// first, each with a higher order than the last and all of them above the game's own
// containers, so a mod's package with a game package's path takes the game's place and
// the mod that starts later wins over the one before it. Every mounted container is
// read first: two mods with a package in common are told so in both their logs.
//
// A blocked mod's container is not mounted. A container stays mounted until the game
// is closed, since the engine cannot take one back.
//
// A mod's shader library, `ShaderArchive-<name>-<format>.ushaderbytecode` in its
// container's `.pak`, is opened by the engine right after the container is mounted, and stays
// open until the game is closed. The engine finds the shader maps of the mod's materials in it.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "list.h"
#include "texts.h"
#include "text.h"

namespace tsuchi {

// What the framework needs of a container's table of contents, its `.utoc`.
struct ContainerToc {
    uint64_t id = 0;              // the container's ID, which the engine makes from its name
    uint64_t header_at = 0;       // where the container's header is in the `.ucas`,
    uint64_t header_bytes = 0;    // and how long it is: 0 when it is not stored whole
    uint64_t data_bytes = 0;      // the end of its last stored block in the `.ucas`
    List<uint64_t> packages;      // the packages whose data it holds,
    List<uint32_t> entries;       // and each one's place in the table, for its path
    // The table of contents itself, for the paths of its files. Borrowed: it is the caller's.
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    size_t index_at = 0, index_bytes = 0; // its index of files, when it has one
};

// Reads a table of contents. False, with `wrong` saying why in words for the mod list, when it
// is no table of contents the game reads: not one at all, of another version, or encrypted.
bool read_container_toc(const uint8_t* bytes, size_t size, ContainerToc& toc, const char*& wrong);
void forget_container_toc(ContainerToc& toc);

// A package the container puts in place of another: the engine looks up `from` and finds `to`.
struct ContainerRedirect {
    uint64_t from, to;
};
// Reads the redirects out of a container's header. Those that lead to the same package are left
// out. False when the header is not shaped as the engine writes one.
bool read_container_redirects(const uint8_t* header, size_t size, List<ContainerRedirect>& redirects);

// The path of a package of the container as the engine names it, such as `/Game/Mods/bulbmin/T_Leaf`,
// from the container's index of files. False when the index has no file for it or there is no memory.
bool container_package_path(const ContainerToc& toc, uint64_t package, Texts& out);
// The identity the engine gives a package path such as /Game/Mods/T_Leaf: CityHash64 of the
// lowercase path as UTF-16. Package paths are ASCII. 0 for a path too long to be one.
uint64_t container_package_id(const char* path);

// The order the engine gives a container mounted with `order`: names that end in `_P` are raised.
uint32_t container_order_raise(const char* pak_name);

// Mounts the containers of the mods that can run, in the order they start. A mod whose container
// cannot be mounted is blocked, and so is every mod that needs it. Notes a container a mod left in
// the game's own folder of containers, which the game mounts by itself. Called once, after the mods
// are found and before any of them starts.
void mount_mod_containers();
// Framework-only late activation of a validated code-free downloaded container.
// Makes packages available to subsequent loads; it does not replace live UObjects
// or promise unmount. Same container identities are refused while retained.
TSUCHI_Result validate_downloaded_assets(const char* id,const char* folder,Text& problem);
TSUCHI_Result mount_downloaded_assets(const char* id,const char* folder,Text& problem);

// Called under the engine's package-store lock before its first-entry check. Only entries
// belonging to the container currently mounted by Tsuchi may replace existing metadata.
void refresh_mounted_package_entry(uint64_t slot, uint64_t entry);

// Reads startup package identities and aliases through bounded header spans. Plain and Zlib
// blocks are accepted; a header is at most 64 MiB, and each stored/decoded block at most 1 MiB.
// A metadata-only container has no package claims, including the stock global sentinel ID.
// Startup headers may have exact zero padding to the TOC's block size or 16-byte alignment.
// Appends claims only on success. Does not mount anything or read the rest of the UCAS.
// An optional 32-byte output receives the reconstructed header's SHA-256, or zero for no header.
TSUCHI_Result read_startup_container_claims(const ContainerToc&, const char* native_ucas_path,
                                          List<uint64_t>& claims, uint8_t* header_sha256 = nullptr);

// Mounts a new immutable asset bundle after the startup census. Paths come from the framework's
// bundle spool, not from a mod. The native paths name sibling `.utoc` and `.ucas` files; the
// engine path names their `.pak`. Every package must be below the canonical `/Game/...`
// root, outside `/Game/Carrot4`, with no redirect to another identity or identity already mounted.
// A same-package identity mapping is allowed only for a package in this TOC. Headers may have
// the exact zero padding needed to align to 16 bytes. The TOC and header are at most 16 MiB
// each. The pak is at most 16 MiB, with empty indexes of at most 64 KiB each, no encryption,
// and verified index hashes. BUSY means a container or package identity is already mounted. UNSUPPORTED means the
// startup census or an uncompressed header is unavailable. Errors never block the calling mod.
// Successful mounts, their identities, and their files remain until the process exits.
TSUCHI_Result mount_asset_container(const char* id, const char* engine_pak_path,
                                   const char* native_toc_path, const char* native_ucas_path,
                                   const char* package_root);

// How many containers the framework has mounted, and whether it mounted one for the mod of this ID.
uint32_t containers_mounted();
bool container_mounted_for(const char* id);
// Writes to the log of each mod that was blocked as it started, with its container mounted, that the
// container stays mounted. Called once, after the mods have started.
void tell_containers_left_mounted();

} // namespace tsuchi
