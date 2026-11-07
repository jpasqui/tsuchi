// Internal script write trees. Conversion finishes before a native writer sees a tree.
#pragma once
#include "list.h"
#include "tsuchi/core.h"

namespace tsuchi {
inline constexpr uint32_t kMaxScriptArrayCount = 1000000;
enum class ScriptWriteKind { value, text, structure, array, empty };
// Script runtimes also build against hosts that do not link the native text library.
struct ScriptText {
    char* data = nullptr;
    const char* get() const { return data ? data : ""; }
    bool set(const char* text) {
        const size_t bytes = strlen(text) + 1;
        auto* copy = static_cast<char*>(allocate(bytes));
        if (!copy) return false;
        memcpy(copy, text, bytes); release(data); data = copy; return true;
    }
    void clear() { release(data); data = nullptr; }
};
struct ScriptWrite {
    ScriptWriteKind kind = ScriptWriteKind::value;
    TSUCHI_Value value{};
    ScriptText name, text;
    List<ScriptWrite*> children;
};
struct ScriptWriteTree {
    ScriptWrite* root = nullptr;
    List<ScriptWrite*> nodes;
    ScriptWriteTree() = default;
    ScriptWriteTree(const ScriptWriteTree&) = delete;
    ScriptWriteTree& operator=(const ScriptWriteTree&) = delete;
    ScriptWrite* add(ScriptWriteKind kind) {
        auto* node = static_cast<ScriptWrite*>(allocate(sizeof(ScriptWrite)));
        if (!node) return nullptr;
        *node = ScriptWrite{};
        node->kind = kind;
        if (!nodes.add(node)) { release(node); return nullptr; }
        return node;
    }
    ~ScriptWriteTree() {
        for (uint32_t i = 0; i < nodes.count; ++i) {
            nodes[i]->name.clear(), nodes[i]->text.clear(), nodes[i]->children.clear();
            release(nodes[i]);
        }
        nodes.clear();
    }
};
struct ScriptTarget {
    TSUCHI_Object object{};
    TSUCHI_PreparedCall call{};
    const char* row = nullptr;
};
using ScriptWriter = TSUCHI_Result (*)(TSUCHI_Mod*, const ScriptTarget&, const char*, const ScriptWrite&);
inline ScriptWriter object_script_writer = nullptr, table_script_writer = nullptr;
inline TSUCHI_Result apply_script_write(TSUCHI_Mod* mod, const ScriptTarget& target, const char* path, const ScriptWrite& value) {
    const ScriptWriter writer = target.row ? table_script_writer : object_script_writer;
    return writer ? writer(mod, target, path, value) : TSUCHI_UNSUPPORTED;
}
} // namespace tsuchi
