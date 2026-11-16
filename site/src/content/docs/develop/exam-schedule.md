---
title: Set the Academy's exam schedule
description: Open Dandori Academy exams on a schedule your mod supplies, without the online service.
---

:::caution[WIP]
Exam schedules work in the Switch 2 Edition only.
:::

The Switch 2 Edition's Dandori Academy opens its exams in periods. The game downloads
the schedule of those periods from Nintendo's service, so without it (offline, or once the
service has ended) no exam opens. A mod can supply that schedule instead. While the mod
runs, the Academy opens, lists, and announces exams the way it does online, and keeps them
in the player's save as it does online. The original game code handles delivery dates, period lists, notices, Redo Pass
processing, and save changes after it receives the local schedule. Tsuchi doesn't
send anything or connect to the service.

Before you start, [create your mod](/develop/first-mod/).

## Manifest

Put the schedule file in your mod's folder and give its relative path in
`academy_schedule`. Set `game` to the Switch 2 Edition, as no other build has the Academy:

```json
"game": ["sw2"],
"academy_schedule": "schedule.json"
```

Only one mod can supply the schedule at a time. A second one that tries won't start.

## Schedule file

The file holds the two settings the game downloads, `DDKStages` and `DDKHeldInfo`, in
the same shape:

```json
{
  "DDKStages": { "Set": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11] },
  "DDKHeldInfo": {
    "Latest": {
      "Stage": 12,
      "DeliveryDate": "2026-11-01T00:00:00Z",
      "TestDates": [{ "Start": "2026-11-01T00:00:00Z", "End": "2099-12-31T23:59:59Z" }],
      "Ticket": 6,
      "UseTicketLimit": false
    },
    "Prev": {
      "Stage": 11,
      "DeliveryDate": "2026-10-01T00:00:00Z",
      "TestDates": [{ "Start": "2026-10-01T00:00:00Z", "End": "2026-11-01T00:00:00Z" }],
      "Ticket": 6
    }
  }
}
```

| Key | Description |
| --- | --- |
| `DDKStages.Set` | The exam periods in the order they come out, by their number in the game's exam course table, `0` to `11`. Up to 64 entries. |
| `DDKHeldInfo.Latest` | The current period. |
| `DDKHeldInfo.Prev` | The period before it. The game uses it until `Latest`'s `DeliveryDate`. Required when `Latest.Stage` is more than 1. |

Each period has these keys:

| Key | Required | Description |
| --- | --- | --- |
| `Stage` | Yes | How many periods of `Set` are out, counting from 1. The Academy lists the first `Stage` periods of `Set`. |
| `DeliveryDate` | Yes | When the period comes out. |
| `TestDates` | No | The windows when exams can be taken, as a list of `{"Start": date, "End": date}`. Up to 64. |
| `Ticket` | No | How many Redo Passes the period hands out, `0` to `99`. Defaults to `1`. |
| `UseTicketLimit` | No | `true` limits retries to the Redo Passes. Defaults to `true`. |
| `Close` | No | Can only be `false`. The game marks the save as past the end of the service for good when the current period is closed. |

Of the periods that are out, the Exams tab shows the first one right away and each later
one once the player has played a stage of the period before it. The game does the same
for a player who starts late online.

Dates are ISO 8601: `2026-11-01`, `2026-11-01T00:00:00Z`, or a time with an offset such
as `2026-11-01T09:00:00+09:00`. The game compares them with the console's network clock.

Tsuchi checks the file when the mod starts and logs what's wrong with it if it doesn't
load.

## Courses

Each exam period has six courses. They are rows `Test_00` to `Test_11` of the Academy's
course table, and the courses are the entries of `CourseData`. Change them with
[`tables`](/develop/change-tables/), like any other table:

```json
"tables": {
  "/Game/Carrot4/UI/OutGame/Dandori/DataTable/DT_DDKTestCourseInfo.DT_DDKTestCourseInfo": {
    "Test_00": {
      "CourseData[0].LevelName": "DDKCave039_Mode0_F00",
      "CourseData[0].CollectRank.Copper": 50
    }
  }
}
```

`Test_00` is Exam 1, and `CourseData[0]` to `CourseData[5]` are Elementary A, Advanced A,
Graduate A, Elementary B, Advanced B and Graduate B. Some of the fields:

| Field | What it is |
| --- | --- |
| `LevelName` | The level the course plays. The course takes the level's name and description from it too. |
| `CollectRank.Copper` to `CollectRank.Platinum`, `CollectRank.KingScore` | The scores for each medal. |
| `Timer` | The time limit, in seconds, that the course screen and the level's title card show. |
| `Difficulty` | 1, 2 or 3: Elementary, Advanced or Graduate. |
| `PassingScore`, `MaxScore` | Score limits. |

The Academy copies the table as the game starts. Tsuchi writes each change into that copy too,
so your changes show up in the Academy. Exam periods keep six courses: `CourseData` is an
array of structs and cannot be resized. Primitive arrays inside a course, including
`TargetPikminColor` and `CourseRewardInfo.RewardRowNameList`, can be resized with
`datatable.resize`. Element overrides apply to the live copies as well. For example,
resize `CourseData[0].TargetPikminColor`, then override its indexed colors. Restore the
length and element overrides separately when you finish with them.

Each native copy has its own array storage. A failed allocation leaves the table and all
copies as they were. Release or mod stop restores their original arrays. A copy made later
is reconciled on an engine frame. If another writer replaces a supplied array's header,
Tsuchi leaves that header alone and refuses a conflicting resize.

Training uses rows `Course_00` to `Course_02` of the training course table, whose row type
is `DDKTrainingCourseRow`. Each row has one `CourseData` struct, so use paths such as
`CourseData.LevelName`, `CourseData.Timer`, and `CourseData.TargetPikminColor[0]`, without
an index after `CourseData`. Training edits propagate to its live copies too. The result
API calls these training courses `0` to `2`; the game's internal IDs are `72` to `74`.

To route a course to your custom level, ship that level with your mod and set `LevelName`
to its world name. The result lookup follows the course's current `LevelName`, so two
courses pointing at the same world share its native saved result key. Give different
course revisions separate community board identities when using an online board.

The clock in the level comes from the level, not from `Timer`: each level has a game rule
actor (`CarrotGameRule`), and the `Minutes` and `Seconds` of its `GameSettings` are how
long the level runs. The game's own courses give `Timer` the same time as their level. When
you point a course at another level, set `Timer` to that level's time, and when you make a
level for a course, set its game rule's time to the course's `Timer`.

## Save data

A delivery goes into the player's Academy save data, as it does online: which periods came
out and when.

## Schedules from code

[`academy.hold_schedule`](/reference/academy/#hold_schedule) accepts the same JSON as the
manifest file. In Lua, call `academy.hold_schedule(json)`; in JavaScript, call
`academy.hold_schedule(JSON.stringify(schedule))`; in C, call
`host->academy->hold_schedule(self, json)`. It applies on an engine frame once the game
has its schedule manager. The same mod can replace its schedule. Invalid input preserves
the previous request; a different mod receives `busy` while the schedule is owned.

Call `academy.release_schedule()` to release it. Mod stop also releases ownership.
Previously delivered periods stay in the save. Release does not undo a delivery.

[`academy.hold_unlocked_periods`](/reference/academy/#hold_unlocked_periods) optionally
lists all delivered periods without requiring the previous period's clear. Reopen the
Exams tab after taking the hold. Release its returned field hold with
`reflection.release_field`; mod stop restores it too. It changes the configuration's
menu check and does not fabricate saved results or unlocked-period entries.

Schedule, course, unlock, and local board changes block Nintendo score uploads before
they apply. That block lasts until the game closes, including after the mod stops.
Read [`academy.state`](/develop/academy-progress/) to inspect it and the current progress.
