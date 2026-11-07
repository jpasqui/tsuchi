#include "mod_choices.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
using namespace tsuchi;
int main() {
    ModChoices choices;
    assert(choices.load("p4a1:bulbmin:absent.mod:", "p4o1:zoo:bulbmin:") == TSUCHI_OK);
    assert(!choices.enabled("bulbmin") && !choices.enabled("absent.mod") && choices.enabled("new.mod"));
    assert(choices.rank("zoo") == 0 && choices.rank("bulbmin") == 1 && choices.rank("new.mod") == UINT32_MAX);
    const char* invalid[] = {"p4a1|bulbmin|", "p4a1:bulbmin", "p4a1:bulbmin:bulbmin:", "p4a2:", "p4a1:bad/name:", "p4a1::"};
    for (const char* bad : invalid) {
        assert(choices.load(bad, "") == TSUCHI_INVALID);
        assert(!choices.enabled("bulbmin") && choices.rank("zoo") == 0);
    }
    assert(choices.load("p4a1:", "p4o1:zoo:zoo:") == TSUCHI_INVALID);
    assert(!choices.enabled("bulbmin"));
    assert(choices.move("bulbmin", "zoo") == TSUCHI_OK);
    assert(choices.rank("bulbmin") == 0 && choices.rank("zoo") == 1);
    assert(choices.move("bulbmin", "") == TSUCHI_OK);
    assert(choices.rank("zoo") == 0 && choices.rank("bulbmin") == 1);
    assert(choices.move("new.mod", "zoo") == TSUCHI_OK);
    assert(choices.rank("new.mod") == 0 && choices.rank("zoo") == 1 && choices.rank("bulbmin") == 2);
    assert(choices.move("new.mod", "new.mod") == TSUCHI_OK);
    assert(choices.move("zoo", "missing") == TSUCHI_NOT_FOUND);
    assert(choices.set_enabled("bulbmin", true) == TSUCHI_OK);
    Text activation, order;
    assert(choices.activation_text(activation) && !std::strcmp(activation.get(), "p4a1:absent.mod:"));
    assert(choices.order_text(order) && !std::strcmp(order.get(), "p4o1:new.mod:zoo:bulbmin:"));
    ModChoices restarted;
    assert(restarted.load(activation.get(), order.get()) == TSUCHI_OK);
    assert(restarted.enabled("bulbmin") && !restarted.enabled("absent.mod") && restarted.rank("new.mod") == 0);
    choices.clear(); restarted.clear(); activation.clear(); order.clear();
    // Empty and late arrivals can be moved without unsigned wraparound.
    assert(choices.move("first", "") == TSUCHI_OK && choices.rank("first") == 0);
    assert(choices.move("last", "") == TSUCHI_OK && choices.rank("last") == 1);
    assert(choices.move("last", "first") == TSUCHI_OK && choices.rank("last") == 0 && choices.rank("first") == 1);
    choices.clear();
    // Allocation failure preserves a complete earlier snapshot.
    assert(choices.load("p4a1:bulbmin:", "p4o1:zoo:") == TSUCHI_OK);
    set_allocator({[](size_t) -> void* { return nullptr; }, nullptr});
    assert(choices.load("p4a1:other:", "p4o1:") == TSUCHI_NO_MEMORY);
    assert(!choices.enabled("bulbmin") && choices.rank("zoo") == 0);
    set_allocator({std::malloc, std::free});
    choices.clear();
}
