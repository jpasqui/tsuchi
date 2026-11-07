// The game side of the audio service.

#include "actors.h"
#include "audio_service.h"
#include "audio_parameters.h"
#include "game.h"
#include "objects.h"
#include "reflect.h"
#include "services.h"

namespace tsuchi {
namespace {

namespace t = game::table;

// True once the game's sound is running.
bool sound_ready() {
    const uint64_t sound = game::global<uint64_t>(t::data::sound_system);
    return sound && game::read_or<uint8_t>(sound + t::sound_system::started) && game::read_or<uint8_t>(sound + t::sound_system::ready);
}

TSUCHI_Result play_on(TSUCHI_Actor actor, const char* key) {
    const uint64_t source = game::from_handle(actor.id);
    if (!source || !game::is_game_actor(source)) return TSUCHI_NOT_FOUND;
    game::Name name{};
    const TSUCHI_Result named = game::name_from_utf8(key, name);
    if (named != TSUCHI_OK || !name.index) return named == TSUCHI_NOT_READY ? named : TSUCHI_INVALID;
    if (!sound_ready() || !t::bound(t::fn::play_sound_on)) return TSUCHI_NOT_READY;
    game::Name nowhere{}; // no part of the actor in particular
    return t::fn::play_sound_on(reinterpret_cast<void*>(source), &name, &nowhere, true) ? TSUCHI_OK : TSUCHI_NOT_FOUND;
}

const audio::Backend service{play_on, audio_parameter, audio_hold_parameter, audio_release_parameter};

} // namespace

void audio_start() { audio::set_backend(&service); }

} // namespace tsuchi
