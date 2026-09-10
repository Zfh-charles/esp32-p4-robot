#include "../../main/domain/visual_commit_scheduler.h"

#include <cassert>

using Scheduler = domain::VisualCommitScheduler;

namespace {

Scheduler::Intent Intent(uint32_t sequence, Scheduler::IntentKind kind,
                         uint32_t generation, uint64_t created_at_ms = 0,
                         uint32_t deadline_ms = 120,
                         uint16_t max_rows = Scheduler::kMaxRows) {
    return {kind, sequence, generation, created_at_ms, deadline_ms, max_rows,
            Scheduler::kMaxBurstBytes};
}

}  // namespace

int main() {
    Scheduler scheduler;
    assert(scheduler.SetGeneration(1));
    assert(scheduler.Submit(Intent(1, Scheduler::IntentKind::kMouthBand, 1)).action ==
           Scheduler::Action::kQueued);
    const auto replaced = scheduler.Submit(
        Intent(2, Scheduler::IntentKind::kMouthBand, 1));
    assert(replaced.superseded_sequence == 1);
    auto decision = scheduler.Evaluate(0, Scheduler::Budget::kMouthOnly);
    assert(decision.action == Scheduler::Action::kCommit && decision.sequence == 2);
    assert(scheduler.Evaluate(0, Scheduler::Budget::kMouthOnly).action ==
           Scheduler::Action::kNone);

    scheduler.Submit(Intent(3, Scheduler::IntentKind::kMouthBand, 1, 0, 119));
    assert(scheduler.Evaluate(120, Scheduler::Budget::kMouthOnly).reason ==
           Scheduler::Reason::kExpired);
    assert(!scheduler.has_pending());

    scheduler.Submit(Intent(4, Scheduler::IntentKind::kIdleLifeBand, 1, 120, 7000));
    assert(scheduler.Evaluate(120, Scheduler::Budget::kTransition, true).reason ==
           Scheduler::Reason::kResourceBusy);
    assert(scheduler.Evaluate(120, Scheduler::Budget::kTransition).action ==
           Scheduler::Action::kNone);

    assert(scheduler.SetGeneration(2));
    scheduler.Submit(Intent(5, Scheduler::IntentKind::kMouthBand, 1, 120));
    assert(scheduler.Evaluate(120, Scheduler::Budget::kMouthOnly).reason ==
           Scheduler::Reason::kStaleGeneration);
    assert(!scheduler.SetGeneration(1));

    scheduler.Submit(Intent(6, Scheduler::IntentKind::kMouthBand, 2, 120, 120,
                            Scheduler::kMaxRows + 1));
    assert(scheduler.Evaluate(120, Scheduler::Budget::kMouthOnly).reason ==
           Scheduler::Reason::kClaimCap);
    assert(scheduler.Submit(Intent(5, Scheduler::IntentKind::kMouthBand, 2)).reason ==
           Scheduler::Reason::kOutOfOrder);

    scheduler.Submit(Intent(7, Scheduler::IntentKind::kMouthBand, 2, 120));
    assert(scheduler.Evaluate(120, Scheduler::Budget::kStaticOnly).reason ==
           Scheduler::Reason::kMouthBudget);
    scheduler.Submit(Intent(8, Scheduler::IntentKind::kIdleLifeBand, 2, 120, 7000));
    decision = scheduler.Evaluate(120, Scheduler::Budget::kTransition);
    assert(decision.action == Scheduler::Action::kCommit &&
           decision.backend == Scheduler::Backend::kPrecomposedBand);
    return 0;
}
