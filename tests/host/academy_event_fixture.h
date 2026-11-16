#pragma once
#include "academy_service.h"

namespace academy_event_fixture {
inline void emit() {
    tsuchi::academy::emit_opened({{42}});
    tsuchi::academy::emit_period_delivered({11});
    tsuchi::academy::emit_period_unlocked({10});
    tsuchi::academy::emit_exam_started({71, true});
    tsuchi::academy::emit_exam_finished({71, 12, 12.75f, true, TSUCHI_ACADEMY_MEDAL_KING, false});
}
inline bool wanted() {
    return tsuchi::academy::opened_wanted() || tsuchi::academy::period_delivered_wanted() ||
        tsuchi::academy::period_unlocked_wanted() || tsuchi::academy::exam_started_wanted() ||
        tsuchi::academy::exam_finished_wanted();
}
}
