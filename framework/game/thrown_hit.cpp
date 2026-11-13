// A thrown Pikmin's landing on a target, as the throw's contact handler (0x1735D50) delivers one when the Pikmin
// doesn't latch on: a damage message made with the Pikmin as its attacker, so its type is recorded (0x1B4B370
// sets "a Pikmin", Rock and Purple), with attack 1 (a direct hit), the contact point, whether the landing presses,
// no collision part (the message's own default), and the frozen-target scaling on, posted to the target's queue
// (0x1736608 and the post that follows it).
#include "families.h"
#include "game.h"
#include "life.h"
#include "reflect.h"

namespace tsuchi::game {
namespace {
namespace t = table;
} // namespace

TSUCHI_Result send_thrown_hit(uint64_t target, uint64_t pikmin, const float point[3], bool press) {
    namespace h = t::pikmin_hit;
    if (family_absent(t::family::thrown_hit)) return TSUCHI_UNSUPPORTED; // a build without the fields above
    if (!life_component(target)) return TSUCHI_NOT_FOUND;
    if (!t::bound(t::fn::damage_message) || !t::bound(t::fn::post_message)) return TSUCHI_NOT_READY;
    void* message = engine_memory(t::damage_message::size);
    if (!message) return TSUCHI_NO_MEMORY;
    t::fn::damage_message(message, reinterpret_cast<void*>(pikmin));
    const uint64_t at = reinterpret_cast<uint64_t>(message);
    write(at + t::damage_message::attack, uint8_t(1));
    for (int i = 0; i < 3; ++i) write(at + h::point + 4 * i, point[i]);
    write(at + h::press, uint8_t(press ? 1 : 0));
    write(at + h::part, uint64_t(0));
    write(at + h::freeze_scaled, uint8_t(1));
    // The game's post frees a message its target won't take: while the game is paused, or the target rests.
    return t::fn::post_message(reinterpret_cast<void*>(target), message) ? TSUCHI_OK : TSUCHI_NOT_READY;
}

} // namespace tsuchi::game
