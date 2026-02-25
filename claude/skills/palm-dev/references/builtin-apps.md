# Palm OS Built-in App Workflows (Palm m500, ROM 4.1)

Learned interaction patterns for the stock ROM apps. These workflows
were discovered by the pose64-tester agent and verified against a live
emulator. Object IDs come from the ROM form resources and are stable
across sessions.

All examples use MCP tool calls (`palm_tap_id`, `palm_type`, etc.).

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

The To Do list uses **inline TABLE editing** -- no separate form opens.
The `palm_ui` tool shows `TABLE` but no FIELD element during editing;
the table widget manages its own text cursor internally.

**Workflow -- N items:**

```
# For each item 1..N:
palm_tap_id id=1005       # New: commits previous item (if any), opens blank row
palm_sleep ms=500
palm_type text="..."      # text goes directly into inline editor
palm_sleep ms=300

# After the last item, commit without starting another:
palm_tap x=80 y=5         # tap title bar area (any x, y~5) to deselect
palm_sleep ms=300
```

**Key facts:**
- `palm_tap_id id=1005` is atomic: commits current edit AND opens next blank row
- Text is typed immediately after New -- no extra tap to focus
- `palm_key code=10` (Enter) does NOT commit; it's ignored in the inline editor
- To commit the final item: tap the title bar area (`palm_tap x=80 y=5`)
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

### Key Objects -- Day View

| ID | Type | Label | Notes |
|----|------|-------|-------|
| 1004 | BUTTON | "All" | Category filter |
| 1015 | BUTTON | "New" | Create event / commit current |
| 1016 | BUTTON | "Details..." | Event details dialog |
| 1017 | BUTTON | "Go To" | Date picker |

### Key Objects -- Set Time Dialog

When "New" is tapped, a **Set Time** dialog always appears first:

`FORM id=10200 "Set Time"`

| ID | Type | Label | Purpose |
|----|------|-------|---------|
| 10208 | BUTTON | "OK" | Accept displayed start/end time (timed event) |
| 10209 | BUTTON | "Cancel" | Abort -- do not create event |
| 10210 | BUTTON | "No Time" | Create untimed/floating event (shown at top) |
| 10212 | BUTTON | "All Day" | Create all-day event |

The dialog also shows hour buttons (1-12), minute columns, and AM/PM
selectors for choosing specific times, but for automation the four
named buttons above are sufficient.

### Creating Events

**Workflow -- N untimed events:**

```
# For each event 1..N:
palm_tap_id id=1015       # New: commits previous (if any), opens Set Time
palm_sleep ms=500
palm_tap_id id=10210      # "No Time" -- or 10208 for timed, 10212 for all-day
palm_sleep ms=500
palm_type text="..."      # text goes into inline TABLE editor
palm_sleep ms=300

# After the last event, commit without starting another:
palm_tap x=80 y=15        # tap an existing event row area to deselect
palm_sleep ms=300
```

**Workflow -- N timed events (accept default time):**

```
palm_tap_id id=1015       # New
palm_sleep ms=500
palm_tap_id id=10208      # OK -- accept displayed start/end time
palm_sleep ms=500
palm_type text="..."
palm_sleep ms=300
```

**Key facts:**
- `palm_tap_id id=1015` is atomic: commits current edit AND opens Set Time for next
- The Set Time dialog ALWAYS appears -- you cannot skip it
- After dismissing Set Time, text is typed immediately -- no extra focus tap
- `palm_key code=10` (Enter) does NOT commit inline edits
- DO NOT tap the title bar (y~5) to deselect -- it opens the Record menu
- Instead, tap an existing event row (`palm_tap x=80 y=15`) to commit the last item
- Untimed events appear at the top of the day with a diamond marker
- Timed events appear in their time slot
- All-day events appear with a special indicator

### Day Navigation

- `palm_button name=up action=tap` / `palm_button name=down action=tap` -- previous/next day
- `palm_tap_id id=1017` ("Go To") -- opens date picker

---

## Memo Pad

**Database name:** `Memo Pad`
**Main form:** `FORM id=1000 "Memo"`

### Key Objects -- Main List

| ID | Type | Label | Notes |
|----|------|-------|-------|
| 1003 | BUTTON | "All" | Category filter popup |
| 1005 | BUTTON | "New" | Create new memo |

### Menus

Memo Pad has two menus accessible via `palm_menu`:

**Record menu:**

| ID | Item | Notes |
|----|------|-------|
| 100 | "Beam Category" | Beam memos via IR; shows error if no receiver found |

**Options menu:**

| ID | Item | Notes |
|----|------|-------|
| 200 | "Font?" | Font picker |
| 201 | "Preferences?" | Preferences dialog |
| 202 | "Security?" | Security settings |
| 203 | "About Memo Pad" | About dialog (form id=11000) |

### Triggering Menus with palm_menu

`palm_menu` auto-activates the menu bar if not already open. Just call
it directly -- no need to press the menu key first.

**Workflow:**

```
palm_menu menu="Options" item="About"   # triggers item by name
palm_sleep ms=500
# Now palm_ui will show the About dialog:
# FORM id=11000 "About Memo Pad"
#   LABEL id=11004 "Copyright © 2001\rPalm, Inc. or its subsidiaries..."
#   BUTTON id=11006 "OK"
palm_tap_id id=11006      # dismiss About dialog
palm_sleep ms=300
```

**Key facts:**
- `palm_menu` auto-activates the menu bar if it's not already open
- Returns "current form has no menu bar" for dialogs/forms without menus
- After `palm_menu` posts the event the menu bar closes automatically
- The About dialog (form 11000) contains Latin-1 text (copyright symbol 0xA9)
  which the proxy now handles correctly via Latin-1 to UTF-8 conversion

---

## Address Book

**Database name:** `Address Book`

(Not yet explored -- to be documented by future agent runs.)

---

## General Palm OS Patterns

These apply across all built-in apps:

- **NEVER tap the silkscreen area**: The silkscreen buttons (Home, Menu, Find,
  Calc) are below the 160x160 display. Their pixel positions vary by device
  and skin. Always use key codes instead:
  - Home: `palm_launch app="Launcher"` or `palm_key code=264`
  - Menu: `palm_key code=261` (but prefer `palm_menu` for triggering items)
  - Find: `palm_key code=266`
  - Calculator: `palm_key code=267`
- **`palm_ui` does NOT show menus unless the menu bar is open**: To discover
  what menus an app has, first activate the menu bar with `palm_key code=261`,
  then call `palm_ui` -- the output will include MENUBAR/MENU/ITEM lines.
  Without this step, `palm_ui` only shows form objects (buttons, fields, etc.).
- **TABLE inline editing**: Most list-based apps (To Do, Date Book) use
  TABLE widgets with inline text editing. The `palm_ui` tool shows `TABLE`
  but not the text being edited. No FIELD element appears.
- **New button dual purpose**: The "New" button typically commits the
  current inline edit AND creates the next item in one atomic action.
- **No Enter to commit**: `palm_key code=10` (Enter/newline) does not commit
  inline table edits in any tested app.
- **500ms settle time**: Allow 500ms between actions (`palm_sleep ms=500`)
  for the emulated CPU to process events and update the display.
- **Latin-1 encoding**: All text is Latin-1, not UTF-8. The `palm_type`
  tool handles conversion, but keep text to ASCII/Latin-1 characters.
  The proxy now converts Latin-1 to UTF-8 before JSON serialisation so
  non-ASCII characters (e.g. copyright symbol 0xA9) are safe in `palm_ui`.
- **Hardware buttons for app switching**: `app1`=Date Book, `app2`=Address,
  `app3`=To Do, `app4`=Note Pad (not Memo Pad -- verified on m500 ROM 4.1).
- **Go Home**: `palm_launch app="Launcher"` or `palm_key code=264` (vchrLaunch).
- **palm_launch uses 'app' param**: `palm_launch app="Memo Pad"` (not `name=`).
  App names come from `palm_apps` output (the database name, e.g. "To Do List").
- **`palm_menu` auto-activates**: No need to press the menu key first.
  `palm_menu` will automatically activate the menu bar if it's not open.
- **Scrolling**: `palm_button name=up/down action=tap` or `palm_key code=11/12`
  (pageUp/pageDown). These work in lists, text views, and most scrollable forms.
- **Field navigation**: `palm_key code=259` (next field), `palm_key code=268`
  (previous field). Works like Tab/Shift-Tab in multi-field forms.
- **palm_run batch syntax**: semicolons separate commands. Supports `tap X Y`,
  `tap_id ID`, `key CODE`, `type TEXT`, `button NAME ACTION`, `sleep MS`,
  `repeat N { CMD; CMD }`. Returns `OK N commands` on success.
