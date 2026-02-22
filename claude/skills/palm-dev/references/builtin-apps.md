# Palm OS Built-in App Workflows (Palm m500, ROM 4.1)

Learned interaction patterns for the stock ROM apps. These workflows
were discovered by the pose64-tester agent and verified against a live
emulator. Object IDs come from the ROM form resources and are stable
across sessions.

---

## To Do List

**Database name:** `To Do List`
**Main form:** `FORM id=1000 "To Do"`

### Key Objects

| ID | Type | Label | Notes |
|----|------|-------|-------|
| 1003 | BUTTON | "All" | Category filter |
| 1005 | BUTTON | "New" | Create new item / commit current |
| 1006 | BUTTON | "Details..." | Item details dialog |
| 1007 | BUTTON | "Show..." | Filter/sort options |

### Creating Items

The To Do list uses **inline TABLE editing** — no separate form opens.
The `ui` command shows `TABLE` but no FIELD element during editing;
the table widget manages its own text cursor internally.

**Workflow — N items:**

```
# For each item 1..N:
tap-id 1005          # New: commits previous item (if any), opens blank row
sleep 0.5
type <item text>     # text goes directly into inline editor
sleep 0.3

# After the last item, commit without starting another:
tap 80 5             # tap title bar area (any x, y~5) to deselect
sleep 0.3
```

**Key facts:**
- `tap-id 1005` is atomic: commits current edit AND opens next blank row
- Text is typed immediately after New — no extra tap to focus
- `key 10` (Enter) does NOT commit; it's ignored in the inline editor
- To commit the final item: tap the title bar area (`tap <any-x> 5`)
- Priority column is to the left; default is unfiled priority
- Items appear with checkboxes in the list

### Priority

Priority is set via a popup trigger in the table row. The priority
list (`LIST id=1011`) has items `" 1"` through `" 5"`. To change
priority you would tap the priority column of the row, but the default
(no priority indicator) is fine for most automation.

---

## Date Book

**Database name:** `Date Book`
**Main form:** `FORM id=1000` (title is current time, e.g. `"5:06 am"`)

### Key Objects — Day View

| ID | Type | Label | Notes |
|----|------|-------|-------|
| 1004 | BUTTON | "All" | Category filter |
| 1015 | BUTTON | "New" | Create event / commit current |
| 1016 | BUTTON | "Details..." | Event details dialog |
| 1017 | BUTTON | "Go To" | Date picker |

### Key Objects — Set Time Dialog

When "New" is tapped, a **Set Time** dialog always appears first:

`FORM id=10200 "Set Time"`

| ID | Type | Label | Purpose |
|----|------|-------|---------|
| 10208 | BUTTON | "OK" | Accept displayed start/end time (timed event) |
| 10209 | BUTTON | "Cancel" | Abort — do not create event |
| 10210 | BUTTON | "No Time" | Create untimed/floating event (shown at top) |
| 10212 | BUTTON | "All Day" | Create all-day event |

The dialog also shows hour buttons (1-12), minute columns, and AM/PM
selectors for choosing specific times, but for automation the four
named buttons above are sufficient.

### Creating Events

**Workflow — N untimed events:**

```
# For each event 1..N:
tap-id 1015          # New: commits previous (if any), opens Set Time
sleep 0.5
tap-id 10210         # "No Time" — or 10208 for timed, 10212 for all-day
sleep 0.5
type <event text>    # text goes into inline TABLE editor
sleep 0.3

# After the last event, commit without starting another:
tap 80 15            # tap an existing event row area to deselect
sleep 0.3
```

**Workflow — N timed events (accept default time):**

```
tap-id 1015          # New
sleep 0.5
tap-id 10208         # OK — accept displayed start/end time
sleep 0.5
type <event text>
sleep 0.3
```

**Key facts:**
- `tap-id 1015` is atomic: commits current edit AND opens Set Time for next
- The Set Time dialog ALWAYS appears — you cannot skip it
- After dismissing Set Time, text is typed immediately — no extra focus tap
- `key 10` (Enter) does NOT commit inline edits
- DO NOT tap the title bar (y~5) to deselect — it opens the Record menu
- Instead, tap an existing event row (`tap 80 15`) to commit the last item
- Untimed events appear at the top of the day with a diamond marker
- Timed events appear in their time slot
- All-day events appear with a special indicator

### Day Navigation

- `button up tap` / `button down tap` — previous/next day
- `tap-id 1017` ("Go To") — opens date picker

---

## Memo Pad

**Database name:** `Memo Pad`

(Not yet explored — to be documented by future agent runs.)

---

## Address Book

**Database name:** `Address Book`

(Not yet explored — to be documented by future agent runs.)

---

## General Palm OS Patterns

These apply across all built-in apps:

- **TABLE inline editing**: Most list-based apps (To Do, Date Book) use
  TABLE widgets with inline text editing. The `ui` command shows `TABLE`
  but not the text being edited. No FIELD element appears.
- **New button dual purpose**: The "New" button typically commits the
  current inline edit AND creates the next item in one atomic action.
- **No Enter to commit**: `key 10` (Enter/newline) does not commit
  inline table edits in any tested app.
- **0.5s settle time**: Allow 500ms between actions for the emulated
  CPU to process events and update the display.
- **Latin-1 encoding**: All text is Latin-1, not UTF-8. The `type`
  command handles conversion, but keep text to ASCII/Latin-1 characters.
- **Hardware buttons for app switching**: `app1`=Date Book, `app2`=Address,
  `app3`=To Do, `app4`=Memo Pad (default mapping, user-configurable).
