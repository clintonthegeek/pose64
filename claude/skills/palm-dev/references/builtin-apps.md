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

## ShadowPlan 4.31 (patched, creator=Coog)

**Database name:** `Shadow`
**PRC file:** `shadow431hhonly/Shadow431_patched.prc`
**Install:** `palm_install path=".../Shadow431_patched.prc"`
**Launch:** `palm_launch app="Shadow"`

ShadowPlan is a hierarchical outliner / task manager. It organises data as
"lists" (files) containing tree nodes. Each node has a body text (multi-line
FIELD), metadata (priority, dates, percent complete, link), and can have
child nodes. The main tree view is a custom-draw GADGET -- `palm_ui` shows
the GADGET bounds but does not enumerate tree items.

### Form Map

| Form ID | Title | Description |
|---------|-------|-------------|
| 1300 | "ShadowPlan" | HomeForm -- list of .pdb files |
| 1400 | "List Preferences" | New/edit list (file) properties |
| 1000 | (list title) | List view -- tree of nodes |
| 1200 | "Details" | Node detail editor (body text + metadata) |
| 12000 | "Broken Links" | Warning when links point to deleted items |

### HomeForm (1300) -- Key Objects

| ID | Type | Label | Notes |
|----|------|-------|-------|
| 1301 | BUTTON | "New" | Create new list (goes to form 1400) |
| 1302 | BUTTON | "Open" | Open selected list (goes to form 1000) |
| 1304 | BUTTON | "Unfiled" | Category filter popup |
| 1305 | LIST | (custom-draw) | File list; sel=-1 means no selection |
| 1307 | BUTTON | "" | Delete/trash button (icon) |
| 1308 | BUTTON | "Recent" | Recently opened lists |

**Opening a list from HomeForm:**
The file list (1305) is custom-draw. To select and open a list:
1. Use `palm_pen x=80 y=26 action=tap` to tap the first item row (y~25)
   -- this selects it but does NOT open it (sel changes from -1 to 0)
2. Tap Open: `palm_tap_id id=1302`
3. A "Broken Links" dialog (form 12000) may appear if the list contains
   links to deleted items -- dismiss with `palm_tap_id id=12004` (OK)
4. Form 1000 opens with the list title

Alternatively, double-tap on the list item (two rapid pen down/up at
the same y) also opens the list, but may trigger the Broken Links dialog.

### List Preferences Form (1400) -- Creating a New List

Reached via: HomeForm > New (1301)

| ID | Type | Label | Notes |
|----|------|-------|-------|
| 1401 | FIELD | (focused) | List filename/title -- auto-focused on open |
| 1402 | BUTTON | "OK" | Save and open the new list |
| 1403 | BUTTON | "Cancel" | Cancel |
| 1404 | FIELD | | (secondary field, read-only looking) |
| 1405 | BUTTON | "Checklist" | List type selector popup |
| 1406 | LABEL | "Filename:" | |
| 1409 | GADGET | | Tab bar: "List" / "Auto" / "Options" tabs |
| 1419 | BUTTON | "Unfiled" | Category popup |
| 1421 | LABEL | "List Type:" | |
| 1422 | LABEL | "Category:" | |
| 1423 | BUTTON | "Custom" | (custom settings button) |
| 1424 | BUTTON | "Synchronize" | Checkbox-style |
| 1426 | BUTTON | "Color theme" | Checkbox-style |
| 1427 | BUTTON | "Mini editor" | Checkbox-style |
| 1428 | BUTTON | "Show headings" | Checkbox-style (checked by default) |

**Workflow -- create a named list:**

```
palm_tap_id id=1301       # New on HomeForm
palm_sleep ms=800
# Field 1401 is auto-focused (marked with * in palm_ui)
palm_type text="My List"  # type the list name
palm_sleep ms=300
palm_tap_id id=1402       # OK -- creates list and opens form 1000
palm_sleep ms=1500
# Now on form 1000 with title = "My List"
```

**Key facts:**
- Field 1401 is auto-focused -- no need to tap it first
- OK creates the list file and immediately opens form 1000 (list view)
- The list is empty when first opened

### List View Form (1000) -- Tree/Node View

This is the main outliner view. The title is the list name.
The central content is `GADGET id=1001 (0,18,160,125)` -- a custom-drawn
tree widget. `palm_ui` shows the gadget bounds but not its contents.
Screenshots are required to see the node tree visually.

| ID | Type | Label | Notes |
|----|------|-------|-------|
| 1001 | GADGET | | Tree view (custom-draw, 160x125 starting at y=18) |
| 1002 | BUTTON | "New" | New sibling node (opens form 1200) |
| 1003 | BUTTON | "Done" | Return to HomeForm (1300) |
| 1004 | BUTTON | "Details" | Edit selected node (opens form 1200) |
| 1005 | BUTTON | "Child" | New child node |
| 1006 | BUTTON | "?" | Scroll up (small, at y=144) |
| 1007 | BUTTON | "?" | Scroll down (small, at y=152) |
| 1008 | LIST | | (column header popup?) |
| 1009 | LIST | | (indent/level popup?) |
| 1010 | BUTTON | "" | Toolbar popup (top-right) |
| 1015 | BUTTON | "" | Search icon |
| 1017 | BUTTON | "" | Delete/trash icon |
| 1027-1035 | BUTTON | "" | Toolbar icon buttons (row at y=146) |
| 1036 | SCROLLBAR | | Vertical scrollbar (right edge) |
| 1037 | BUTTON | "Popup" | Mini popup at bottom-left |

**Workflow -- add a new node:**

```
palm_tap_id id=1002       # New: opens form 1200 (Details)
palm_sleep ms=800
# Field 1201 is auto-focused
palm_type text="My item"  # type node body text
palm_sleep ms=300
palm_tap_id id=1202       # OK: saves node, returns to form 1000
palm_sleep ms=800
# Node now visible in tree view
```

**Key facts on item creation:**
- "New" (1002) opens form 1200 as a blank node
- `palm_key code=10` (Enter) adds a newline WITHIN the body text field --
  it does NOT save the node or create another one
- Each newline-separated line in the body FIELD appears as a separate visual
  row in the tree, so `palm_type text="A\nB\nC"` creates three visible rows
  under one node
- "Child" (1005) works similarly but creates a child of the currently selected node
- "Done" (1003) returns to HomeForm and updates the file list

### Node Details Form (1200) -- Editing a Node

Reached via: List View > New (1002) or > Details (1004)

| ID | Type | Label | Notes |
|----|------|-------|-------|
| 1201 | FIELD | (auto-focused) | Node body text (multi-line, 149px wide, ~7 rows) |
| 1202 | BUTTON | "OK" | Save and return to form 1000 |
| 1203 | BUTTON | "Cancel" | Discard and return |
| 1205 | BUTTON | "Note" | Open/edit associated note |
| 1206 | BUTTON | "Link" | Manage links to other nodes |
| 1207 | LABEL | "Start:" | |
| 1208 | LABEL | "Finish:" | |
| 1209 | BUTTON | "No pref." | Priority popup |
| 1210 | LIST | | Priority values: "No pref.", "None (#)", "1-2-3-4-5", etc. |
| 1211 | LABEL | "Targ:" | Target date label |
| 1212 | BUTTON | "Not Set" | Target date popup |
| 1213 | BUTTON | "-" | Start date popup |
| 1214 | BUTTON | "0%" | Completion % popup |
| 1215 | LIST | | Priority 1-5 or "-" |
| 1216 | LIST | | Completion % (0-100%) |
| 1217 | SCROLLBAR | | Vertical scroll for body text |
| 1218 | BUTTON | "Not Set" | Start date popup |
| 1219 | BUTTON | "Not Set" | Finish date popup |
| 1220 | FIELD | "3/13/26" | Creation date (read-only) |
| 1221 | LIST | | (view selector) |
| 1222 | BUTTON | "B" | Bold toggle |
| 1223 | GADGET | | (formatting gadget) |
| 1224 | BUTTON | "Checklist" | Node type popup |
| 1225 | LABEL | "cr:" | Creation date label |
| 1226 | BUTTON | "" | (icon button, top toolbar) |
| 1230 | BUTTON | "" | Targ: date clear/set |
| 1231 | BUTTON | "" | Start: date clear/set |
| 1232 | BUTTON | "" | Finish: date clear/set |
| 1233 | BUTTON | "" | (small icon) |
| 1235 | BUTTON | "" | (icon button, top toolbar) |

### Broken Links Form (12000)

Appears when opening a list that contains links to deleted items.

| ID | Type | Label | Notes |
|----|------|-------|-------|
| 12003 | FIELD | (read-only) | Warning message text |
| 12004 | BUTTON | "OK" | Keep broken links (shows again next open) |
| 12005 | BUTTON | "Sever All Broken Links" | Permanently remove all broken link references |

**Workflow -- dismiss Broken Links:**

```
palm_tap_id id=12004      # OK: dismiss, links remain (warning repeats on reopen)
# or:
palm_tap_id id=12005      # Sever All: permanently cleans up broken links
```

### Complete Workflow -- Create List and Add Items

```
# Install (first time only):
palm_install path="/path/to/Shadow431_patched.prc"
palm_sleep ms=1000

# Launch:
palm_launch app="Shadow"
palm_sleep ms=1500

# Create new list:
palm_tap_id id=1301       # New
palm_sleep ms=800
palm_type text="Test List"
palm_sleep ms=300
palm_tap_id id=1402       # OK
palm_sleep ms=1500
# Now on form 1000 "Test List"

# Add first node (3 lines appear as 3 rows):
palm_tap_id id=1002       # New
palm_sleep ms=800
palm_type text="Buy groceries"
palm_sleep ms=300
palm_tap_id id=1202       # OK
palm_sleep ms=800

# Add second node:
palm_tap_id id=1002
palm_sleep ms=800
palm_type text="Call doctor"
palm_sleep ms=300
palm_tap_id id=1202
palm_sleep ms=800

# Return to home:
palm_tap_id id=1003       # Done
palm_sleep ms=500
# HomeForm shows "Test List (0/N)" where N = total item count
```

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
