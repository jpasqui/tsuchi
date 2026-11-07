// A content path: how the engine names a package, and an object in one.
//
//   /Game/Mods/example_cave/Cave_Mine        a package: folders from the top, then the package's name
//   /Game/Mods/example_cave/Rock.Rock_C      an object: its package, a dot, and the object's name in it
//
// The engine takes no dot in the name of a folder or of a package, so a path has one dot at most: the one
// between a package and an object in it, which comes after the last `/`. A path with a dot before that, such
// as one through a folder named after a mod ID that has a dot, names nothing the engine can load. A mod's
// content folder is therefore named without a dot.
//
// The engine tells no two names apart by the case of their letters, and neither do two paths compare here.
//
// Every part of the framework that takes a content path splits it, judges it and compares it with these, and
// tells what is wrong with one in the words below. No library is called.
#pragma once
#include <stddef.h>

namespace tsuchi::content_path {

// A path's two parts. The package's path is the first `package` bytes of the path. `object` is the object's
// name, which follows the dot and is `object_bytes` long, or null for a path that names a package alone.
struct Parts {
    size_t package = 0;
    const char* object = nullptr;
    size_t object_bytes = 0;
};

// Splits `bytes` of text at the dot after the last `/`. False for text that is no content path: one that does
// not begin with `/`, has nothing between two `/` or after the last, has a dot before the last `/` or more
// than one after it, or has nothing before or after its dot.
inline bool split(const char* path, size_t bytes, Parts& out) {
    out = Parts{};
    if (!path || bytes < 2 || path[0] != '/') return false;
    size_t leaf = 0, dot = 0; // where the last part begins, and where its dot is
    for (size_t i = 0; i < bytes; ++i) {
        const char c = path[i];
        if (!c) return false;
        if (c == '/') {
            if (dot || i + 1 == bytes || path[i + 1] == '/') return false;
            leaf = i + 1;
        } else if (c == '.') {
            if (dot || i == leaf || i + 1 == bytes) return false;
            dot = i;
        }
    }
    out.package = dot ? dot : bytes;
    if (dot) out.object = path + dot + 1, out.object_bytes = bytes - dot - 1;
    return true;
}

// The same for text with a terminator.
inline bool split(const char* path, Parts& out) {
    size_t bytes = 0;
    while (path && path[bytes]) ++bytes;
    return split(path, bytes, out);
}

// What a part of the framework wants a path to name.
enum class Names { package, object, either };

// What is told of text that is no content path, after the text itself and whatever says where it stands.
inline constexpr char kNoPath[] =
    " is no content path: one begins with /, and its only dot is the one before an object's name, as in /Game/Mods/my_mod/Rock.Rock_C. The engine takes no dot in "
    "the name of a folder or of a package, so a mod's content folder is named without one";
// What is told of a content path that names the other of the two.
inline constexpr char kNoPackage[] = " names an object, and a package's path is wanted here: the path without its dot and the name after it";
inline constexpr char kNoObject[] = " names a package, and an object's path is wanted here: the package's path, a dot, and the object's name, as in /Game/Mods/my_mod/Rock.Rock_C";

// Judges `bytes` of text. Null when it is a content path that names what is wanted. Otherwise what to tell of
// it, which is one of the three texts above.
inline const char* wrong(const char* path, size_t bytes, Names wanted) {
    Parts parts;
    if (!split(path, bytes, parts)) return kNoPath;
    if (wanted == Names::package && parts.object) return kNoPackage;
    if (wanted == Names::object && !parts.object) return kNoObject;
    return nullptr;
}

// The same for text with a terminator.
inline const char* wrong(const char* path, Names wanted) {
    size_t bytes = 0;
    while (path && path[bytes]) ++bytes;
    return wrong(path, bytes, wanted);
}

// Whether two paths, or two names, are the same to the engine: letter for letter, whatever the case of each.
inline bool same(const char* a, size_t a_bytes, const char* b, size_t b_bytes) {
    if (a_bytes != b_bytes) return false;
    for (size_t i = 0; i < a_bytes; ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] + 32) : a[i], y = b[i] >= 'A' && b[i] <= 'Z' ? char(b[i] + 32) : b[i];
        if (x != y) return false;
    }
    return true;
}

// The same for two texts with terminators.
inline bool same(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char x = *a >= 'A' && *a <= 'Z' ? char(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? char(*b + 32) : *b;
        if (x != y) return false;
        if (!x) return true;
    }
}

} // namespace tsuchi::content_path
