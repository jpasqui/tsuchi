// Creator pixel programs in a UE4 version 2 archive, with material metadata in
// mod.json. Converts metadata into the existing shader transaction's in-memory
// records. No shader set is written to disk and no stock shader is imported.
#pragma once
#include "game_table.h"
#include "shader_file.h"
#include "json.h"
#include "registry.h"
#include "sha1.h"
#include <string.h>

namespace tsuchi::shaders::archive {
constexpr uint32_t limit = 256u << 20;
using tsuchi::sha1;
inline int nibble(unsigned char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
inline bool text(const Json* j, uint32_t maximum = 511) {
    return j && j->kind == Json::Kind::text && j->bytes && j->bytes <= maximum && strlen(j->text) == j->bytes;
}
inline bool whole(const Json* j, uint32_t maximum, uint32_t& out) {
    if (!j || j->kind != Json::Kind::number || !j->whole || j->integer < 0 || uint64_t(j->integer) > maximum) return false;
    out = uint32_t(j->integer);
    return true;
}
inline bool hex(const Json* j, uint8_t* out, uint32_t bytes) {
    if (!j || j->kind != Json::Kind::text || j->bytes != uint64_t(bytes) * 2) return false;
    for (uint32_t i = 0; i < bytes; ++i) {
        const int a = nibble(j->text[2 * i]), b = nibble(j->text[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        if (out) out[i] = uint8_t(16 * a + b);
    }
    return true;
}
struct Writer {
    uint8_t* data = nullptr;
    uint32_t capacity = limit, at = 0;
    bool ok = true, no_memory = false;
    void append(const void* source, uint32_t bytes) {
        if (!ok || bytes > capacity - at) {
            ok = false;
            return;
        }
        if (data && bytes) memcpy(data + at, source, bytes);
        at += bytes;
    }
    void byte(uint8_t value) { append(&value, 1); }
    void word(uint32_t value) {
        uint8_t b[4];
        for (unsigned i = 0; i < 4; ++i)
            b[i] = uint8_t(value >> (8 * i));
        append(b, 4);
    }
    bool string(const Json* j) {
        if (!text(j)) return false;
        byte(uint8_t(j->bytes));
        byte(uint8_t(j->bytes >> 8));
        append(j->text, uint32_t(j->bytes));
        return ok;
    }
    bool hex_bytes(const Json* j, uint32_t n) {
        if (!hex(j, nullptr, n) || n > capacity - at) return false;
        if (data && !hex(j, data + at, n)) return false;
        at += n;
        return true;
    }
};
struct View {
    const uint8_t* data = nullptr;
    uint32_t size = 0, starts[6]{}, counts[6]{}, payload = 0;
    bool valid = false;
    bool open(const uint8_t* input, uint32_t bytes) {
        data = input;
        size = bytes;
        valid = false;
        if (!data || size < 28 || size > limit || detail::u32_at(data) != 2) return false;
        uint32_t at = 4;
        const unsigned strides[] = {20, 20, 16, 17, 16, 4};
        for (unsigned i = 0; i < 6; ++i) {
            if (size - at < 4) return false;
            counts[i] = detail::u32_at(data + at);
            at += 4;
            starts[i] = at;
            if (counts[i] > 65536 || counts[i] > (size - at) / strides[i]) return false;
            at += counts[i] * strides[i];
        }
        payload = at;
        if (!counts[0] || !counts[1] || counts[0] != counts[2] || counts[1] != counts[3]) return false;
        for (uint32_t i = 0; i < counts[2]; ++i) {
            const auto* e = data + starts[2] + 16 * i;
            const uint32_t first = detail::u32_at(e), n = detail::u32_at(e + 4), p = detail::u32_at(e + 8),
                           pn = detail::u32_at(e + 12);
            if (!n || n > 4096 || first > counts[5] || n > counts[5] - first || p > counts[4] || pn > counts[4] - p) return false;
        }
        for (uint32_t i = 0; i < counts[5]; ++i)
            if (detail::u32_at(data + starts[5] + 4 * i) >= counts[3]) return false;
        for (uint32_t i = 0; i < counts[4]; ++i) {
            const auto* p = data + starts[4] + 16 * i;
            const uint64_t off = uint64_t(detail::u32_at(p)) | (uint64_t(detail::u32_at(p + 4)) << 32),
                           n = uint64_t(detail::u32_at(p + 8)) | (uint64_t(detail::u32_at(p + 12)) << 32);
            if (off > size - payload || n > size - payload - off) return false;
        }
        for (uint32_t i = 0; i < counts[3]; ++i) {
            const auto* e = data + starts[3] + 17 * i;
            const uint64_t off = uint64_t(detail::u32_at(e)) | (uint64_t(detail::u32_at(e + 4)) << 32);
            const uint32_t n = detail::u32_at(e + 8), full = detail::u32_at(e + 12);
            // This reader serves creator pixel archives. A stock vertex program
            // or any other stage in a mod archive is refused before publication.
            if (e[16] != kPixel || !n || n > kShaderLimit || !full || full > kShaderLimit || off > size - payload ||
                n > size - payload - off)
                return false;
        }
        valid = true;
        return true;
    }
    bool compressed(const uint8_t* raw, uint32_t count, uint8_t* out, uint32_t full) const {
        uint32_t at = 0, made = 0;
        const auto length = [&](uint32_t initial, uint32_t& n) {
            n = initial;
            if (initial == 15) {
                uint8_t v;
                do {
                    if (at == count) return false;
                    v = raw[at++];
                    if (n > full || v > full - n) return false;
                    n += v;
                } while (v == 255);
            }
            return true;
        };
        while (at < count) {
            const uint8_t token = raw[at++];
            uint32_t literals = 0;
            if (!length(token >> 4, literals) || literals > count - at || literals > full - made) return false;
            memcpy(out + made, raw + at, literals);
            at += literals;
            made += literals;
            if (at == count) break;
            if (count - at < 2) return false;
            const unsigned offset = unsigned(raw[at]) | (unsigned(raw[at + 1]) << 8);
            at += 2;
            uint32_t match = 0;
            if (!length(token & 15, match) || !offset || offset > made || match > full - made || full - made - match < 4)
                return false;
            match += 4;
            for (uint32_t i = 0; i < match; ++i) {
                out[made] = out[made - offset];
                ++made;
            }
        }
        return made == full;
    }
    bool append_payload(const uint8_t hash[20], Writer& out) const {
        if (!valid) return false;
        uint32_t found = counts[1];
        for (uint32_t i = 0; i < counts[1]; ++i)
            if (!memcmp(data + starts[1] + 20 * i, hash, 20)) {
                if (found != counts[1]) return false;
                found = i;
            }
        if (found == counts[1]) return false;
        const auto* entry = data + starts[3] + 17 * found;
        const uint64_t offset = uint64_t(detail::u32_at(entry)) | (uint64_t(detail::u32_at(entry + 4)) << 32);
        const uint32_t n = detail::u32_at(entry + 8), full = detail::u32_at(entry + 12);
        const uint8_t* bytes = data + payload + offset;
        uint8_t* owned = nullptr;
        if (n != full) {
            owned = static_cast<uint8_t*>(allocate(full));
            if (!owned) {
                out.no_memory = true;
                return false;
            }
            if (!compressed(bytes, n, owned, full)) {
                release(owned);
                return false;
            }
            bytes = owned;
        }
        uint8_t actual[20];
        sha1(bytes, full, actual);
        const bool same = !memcmp(actual, hash, 20);
        if (same) {
            out.word(full);
            out.append(bytes, full);
        }
        release(owned);
        return same && out.ok;
    }
    bool matches_collection(const Json& row, const Json& programs) const {
        uint8_t hash[20];
        if (!valid || !hex(row.find("archive_map"), hash, 20)) return false;
        uint32_t found = counts[0];
        for (uint32_t i = 0; i < counts[0]; ++i)
            if (!memcmp(data + starts[0] + 20 * i, hash, 20)) {
                if (found != counts[0]) return false;
                found = i;
            }
        if (found == counts[0]) return false;
        const auto* map = data + starts[2] + 16 * found;
        const uint32_t first = detail::u32_at(map), n = detail::u32_at(map + 4);
        if (n != programs.count) return false;
        uint32_t at = 0;
        for (const Json* program = programs.first; program; program = program->next, ++at) {
            if (!hex(program->find("hash"), hash, 20)) return false;
            const uint32_t index = detail::u32_at(data + starts[5] + 4 * (first + at));
            if (memcmp(data + starts[1] + 20 * index, hash, 20)) return false;
        }
        return at == n;
    }
};
inline bool shape(const Json& metadata) {
    const auto* library = metadata.find("library");
    const auto* file = metadata.find("archive");
    const auto* rows = metadata.find("materials");
    if (metadata.kind != Json::Kind::object || !text(library, 63) || !text(file, 127) || !rows ||
        rows->kind != Json::Kind::list || !rows->count || rows->count > kMaterialLimit)
        return false;
    for (size_t i = 0; i < library->bytes; ++i) {
        const unsigned char c = library->text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    const char prefix[] = "ShaderArchive-", suffix[] = ".ushaderbytecode";
    const char* const format = game::table::name::shader_format;
    const size_t before = sizeof(prefix) - 1 + library->bytes, format_bytes = strlen(format);
    return file->bytes == before + 1 + format_bytes + sizeof(suffix) - 1 &&
           !memcmp(file->text, prefix, sizeof(prefix) - 1) &&
           !memcmp(file->text + sizeof(prefix) - 1, library->text, library->bytes) &&
           file->text[before] == '-' &&
           !memcmp(file->text + before + 1, format, format_bytes) &&
           !memcmp(file->text + before + 1 + format_bytes, suffix, sizeof(suffix) - 1);
}
inline bool content(const Json* edits, Writer& out) {
    if (!edits) {
        out.word(0);
        return out.ok;
    }
    uint32_t old = 0, now = 0;
    const auto* operations = edits->find("operations");
    const auto* names = edits->find("names");
    if (edits->kind != Json::Kind::object || !whole(edits->find("old_size"), kContentLimit, old) || !old ||
        !whole(edits->find("new_size"), kContentLimit, now) || !now || !operations || operations->kind != Json::Kind::list ||
        operations->count > now || !names || names->kind != Json::Kind::list || names->count > now)
        return false;
    out.word(1);
    out.word(old);
    out.word(now);
    out.word(operations->count);
    uint32_t made = 0;
    for (const Json* op = operations->first; op; op = op->next) {
        if (op->kind != Json::Kind::object) return false;
        const auto* bytes = op->find("bytes");
        if (bytes) {
            if (op->find("offset") || op->find("length") || bytes->kind != Json::Kind::text || bytes->bytes % 2 ||
                bytes->bytes / 2 > now - made)
                return false;
            const uint32_t n = uint32_t(bytes->bytes / 2);
            if (!n) return false;
            out.byte(1);
            out.word(n);
            if (!out.hex_bytes(bytes, n)) return false;
            made += n;
        } else {
            uint32_t offset = 0, n = 0;
            if (!whole(op->find("offset"), old, offset) || !whole(op->find("length"), old - offset, n) || !n || n > now - made)
                return false;
            out.byte(0);
            out.word(offset);
            out.word(n);
            made += n;
        }
    }
    if (made != now) return false;
    out.word(names->count);
    for (const Json* name = names->first; name; name = name->next) {
        uint32_t number = 0;
        const auto* textValue = name->find("name");
        const auto* offsets = name->find("offsets");
        if (name->kind != Json::Kind::object || !text(textValue, kNameLimit) ||
            !whole(name->find("number"), UINT32_MAX, number) || !offsets || offsets->kind != Json::Kind::list ||
            offsets->count > now / kNameBytes || !out.string(textValue))
            return false;
        out.word(number);
        out.word(offsets->count);
        for (const Json* place = offsets->first; place; place = place->next) {
            uint32_t offset = 0;
            if (now < kNameBytes || !whole(place, now - kNameBytes, offset)) return false;
            out.word(offset);
        }
    }
    return out.ok;
}
inline bool write(const Json& metadata, const View& archive, Writer& out) {
    if (!shape(metadata)) return false;
    const auto* rows = metadata.find("materials");
    out.append("P4MSHDR3", 8);
    out.word(rows->count);
    for (const Json* row = rows->first; row; row = row->next) {
        if (row->kind != Json::Kind::object || !out.string(row->find("material")) || !out.string(row->find("game_material")) ||
            !out.hex_bytes(row->find("map_id"), 20))
            return false;
        uint32_t total = 0;
        const auto* programs = row->find("shaders");
        if (!whole(row->find("map_shaders"), kMapShaderLimit, total) || !total || !programs ||
            programs->kind != Json::Kind::list || !programs->count || programs->count > total ||
            !archive.matches_collection(*row, *programs))
            return false;
        out.word(total);
        out.word(programs->count);
        for (const Json* program = programs->first; program; program = program->next) {
            uint32_t place = 0;
            uint8_t hash[20];
            if (program->kind != Json::Kind::object || !whole(program->find("place"), total - 1, place) ||
                !hex(program->find("hash"), hash, 20))
                return false;
            out.word(place);
            out.byte(kPixel);
            out.append(hash, 20);
            if (!archive.append_payload(hash, out)) return false;
        }
        if (!content(row->find("content"), out)) return false;
    }
    return out.ok;
}
inline Read read(const Json& metadata, const uint8_t* data, uint32_t size, uint8_t*& result, uint32_t& resultSize) {
    result = nullptr;
    resultSize = 0;
    View archive;
    if (!archive.open(data, size)) return Read::bad_value;
    Writer measured;
    if (!write(metadata, archive, measured)) return measured.no_memory ? Read::full : Read::bad_value;
    auto* bytes = static_cast<uint8_t*>(allocate(measured.at));
    if (!bytes) return Read::full;
    Writer output{bytes, measured.at};
    Counts counts{};
    const bool ok = write(metadata, archive, output) && output.at == measured.at && measure(bytes, output.at, counts) == Read::ok;
    if (!ok) {
        release(bytes);
        return output.no_memory ? Read::full : Read::bad_value;
    }
    result = bytes;
    resultSize = output.at;
    return Read::ok;
}
} // namespace tsuchi::shaders::archive
