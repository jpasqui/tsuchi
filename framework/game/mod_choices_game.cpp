#include "mod_choices_game.h"
#include "game.h"
#include "json.h"
#include "registry.h"
#include "save_session.h"
#include "services.h"
#include "texts.h"
#include <string.h>
namespace tsuchi {
namespace {
namespace t = game::table;
constexpr const char* path = "save:/tsuchi/mods.json";
constexpr uint64_t maximum_bytes = 256 * 1024;
TSUCHI_Result load(ModChoices& choices) {
    if (!on_game_thread()) return TSUCHI_INVALID;
    if (!t::bound(t::fn::save_begin_session) || !t::bound(t::fn::save_end_session) || !game::files_known()) return TSUCHI_NOT_READY;
    // Scene loading needs the chosen containers. Wait only for existing save IO.
    if (game::global<uint8_t>(t::data::save_async_state) ||
        game::global<uint8_t>(t::data::save_mounted) ||
        game::global<uint8_t>(t::data::engine_save_mounted)) return TSUCHI_NOT_READY;
    game::SaveSession session;
    if (!session.open) return TSUCHI_NOT_READY;
    uint64_t bytes = 0;
    auto result = game::file_size(path, bytes);
    if (result == TSUCHI_NOT_FOUND) return choices.load("", "");
    if (result != TSUCHI_OK) return result;
    if (bytes > maximum_bytes) return TSUCHI_CAPACITY;
    char* text = nullptr;
    result = game::read_whole_file(path, text, bytes);
    if (result != TSUCHI_OK) return result;
    JsonError error{};
    const Json* root = read_json(text, size_t(bytes), &error);
    const Json* format = root ? root->find("format") : nullptr;
    const Json* activation = root ? root->find("activation") : nullptr;
    const Json* order = root ? root->find("order") : nullptr;
    const bool valid = root && root->kind == Json::Kind::object && root->count == 3 &&
        format && format->kind == Json::Kind::number && format->whole && format->integer == 1 &&
        activation && activation->kind == Json::Kind::text && strlen(activation->text) == activation->bytes &&
        order && order->kind == Json::Kind::text && strlen(order->text) == order->bytes;
    result = valid ? choices.load(activation->text, order->text) :
        error.allocation_failure ? TSUCHI_NO_MEMORY : TSUCHI_INVALID;
    if (root) release_json(root);
    release(text);
    return result;
}
} // namespace
void mod_choices_start() { set_mod_choices_loader(load); }
TSUCHI_Result save_mod_choices(const ModChoices& choices) {
    if (!on_game_thread()) return TSUCHI_INVALID;
    Text activation, order;
    if (!choices.activation_text(activation) || !choices.order_text(order)) {
        activation.clear(); order.clear(); return TSUCHI_NO_MEMORY;
    }
    ModChoices staged;
    auto result = staged.load(activation.get(), order.get());
    Texts body, json;
    // Encoded choices contain only validated IDs and ':'; no JSON escapes.
    if (result == TSUCHI_OK && (!body.add("{\"format\":1,\"activation\":\"", activation.get(), "\",\"order\":\"", order.get()) ||
        !json.add(body.data, "\"}\n"))) result = TSUCHI_NO_MEMORY;
    body.clear();
    activation.clear(); order.clear();
    if (result == TSUCHI_OK && strlen(json.data) > maximum_bytes) result = TSUCHI_CAPACITY;
    if (result == TSUCHI_OK) result = write_save_file(path, json.data, strlen(json.data));
    if (result == TSUCHI_OK) swap_mod_choices(staged);
    staged.clear(); json.clear();
    return result;
}
} // namespace tsuchi
