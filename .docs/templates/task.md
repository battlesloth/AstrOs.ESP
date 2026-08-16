# T-NNN: <imperative title — what this task accomplishes>

<!-- File: .docs/tasks/T-NNN-<slug>.md. Branch: feature/T-NNN-<slug>. PR title: "T-NNN: <title>".
     Everything above "Implementation checklist" is written and committed BEFORE implementation code.
     Sizing rules and rationale: .docs/agentic-workflow.md §1. -->

## Context

<!-- Why this work exists. Link specs, .docs/protocol.md, code-review findings
     (.docs/code-review/code-review.md), bench observations. -->

## Contract (pinned — do not change)

<!-- Interfaces this task touches but must not alter: serial/ESP-NOW wire formats (shared with
     AstrOs.Server), NVS layouts (espnow_peer_t — already flashed in the field), queue-message
     ownership (producer mallocs, consumer frees), the 250 B ESP-NOW payload limit, PURE-lib
     purity (no ESP-IDF/FreeRTOS includes in lib_native/), master/padawan role gating.
     If a contract this task needs doesn't exist yet, STOP — designing it is its own task.
     If a pinned contract seems wrong mid-task, stop and raise it; never adapt it silently. -->

## Task

<!-- What to do, stated tightly. One session's worth, zero architectural decisions. -->

## Acceptance criteria

<!-- Agent ticks each box as it is verified, before the PR opens. Human-gated criteria
     (bench sign-off on real hardware) are ticked at post-merge upkeep. -->

- [ ]
- [ ]

## Out of scope

<!-- The fence. Adjacent improvements — including tempting fixes from the code-review catalog in
     files this task touches — go in a note or the PLAN.md Backlog, not the diff, unless the task
     is about that finding. Name the tasks that own the excluded work if they exist. -->

## Verification

<!-- Concrete, runnable proof of completion, written before work starts. Typical shape:
     `pio test -e test` green; `pio run -e lolin_d32_pro` and `pio run -e metro_s3` both build;
     bench: <action> → <observable result on device/monitor>. -->

## Implementation checklist

<!-- Added when work STARTS, not at authoring time. Check off + commit as work proceeds. -->

- [ ]
