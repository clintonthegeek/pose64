#include "PalmFormReader.h"

#include "EmCommon.h"
#include "EmSession.h"
#include "EmMemory.h"
#include "EmLowMem.h"

#include <sstream>
#include <algorithm>

// Maximum string length to read from emulated memory
static const int kMaxStringLen = 256;

// Maximum output string length before truncation with "..."
static const int kMaxDisplayLen = 80;

// Valid emulated memory range check
static bool IsValidPtr(emuptr p)
{
	if (p == 0) return false;
	// RAM: 0x00000000 - 0x00FFFFFF
	// ROM: 0x10000000 - 0x10FFFFFF
	if (p <= 0x00FFFFFF) return true;
	if (p >= 0x10000000 && p <= 0x10FFFFFF) return true;
	return false;
}

// Read a null-terminated string from emulated memory
static std::string ReadEmuString(emuptr addr)
{
	if (!IsValidPtr(addr)) return "(bad ptr)";

	char buf[kMaxStringLen + 1];
	int i = 0;
	for (; i < kMaxStringLen; ++i)
	{
		uint8 ch = (uint8)EmMemGet8(addr + i);
		if (ch == 0) break;
		buf[i] = (char)ch;
	}
	buf[i] = '\0';

	std::string s(buf);
	if (s.length() > (size_t)kMaxDisplayLen)
	{
		s.resize(kMaxDisplayLen);
		s += "...";
	}
	return s;
}

// Escape a string for output (replace control chars)
static std::string EscapeString(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s)
	{
		if (c == '"')       out += "\\\"";
		else if (c == '\\') out += "\\\\";
		else if (c == '\n') out += "\\n";
		else if (c == '\r') out += "\\r";
		else if (c == '\t') out += "\\t";
		else if ((unsigned char)c < 0x20) out += '?';
		else out += c;
	}
	return out;
}

// Format bounds as (x,y,w,h)
static std::string FormatBounds(int16 x, int16 y, int16 w, int16 h)
{
	return "(" + std::to_string(x) + "," + std::to_string(y) + ","
	     + std::to_string(w) + "," + std::to_string(h) + ")";
}

// Object type name lookup
static const char* ObjTypeName(int type)
{
	switch (type)
	{
		case kFrmFieldObj:          return "FIELD";
		case kFrmControlObj:        return "CONTROL";
		case kFrmListObj:           return "LIST";
		case kFrmTableObj:          return "TABLE";
		case kFrmBitmapObj:         return "BITMAP";
		case kFrmLineObj:           return "LINE";
		case kFrmFrameObj:          return "FRAME";
		case kFrmRectangleObj:      return "RECT";
		case kFrmLabelObj:          return "LABEL";
		case kFrmTitleObj:          return "TITLE";
		case kFrmPopupObj:          return "POPUP";
		case kFrmGraffitiStateObj:  return "GRAFFITI";
		case kFrmGadgetObj:         return "GADGET";
		case kFrmScrollBarObj:      return "SCROLLBAR";
		default:                    return "UNKNOWN";
	}
}

// Control style name lookup
static const char* ControlStyleName(int style)
{
	switch (style)
	{
		case kButtonCtl:            return "BUTTON";
		case kPushButtonCtl:        return "PUSHBUTTON";
		case kCheckboxCtl:          return "CHECKBOX";
		case kPopupTriggerCtl:      return "POPUP";
		case kSelectorTriggerCtl:   return "SELECTOR";
		case kRepeatingButtonCtl:   return "REPEATING";
		case kSliderCtl:            return "SLIDER";
		case kFeedbackSliderCtl:    return "FSLIDER";
		default:                    return "BUTTON";
	}
}

//============================================================================
// Read a ControlType object
//============================================================================
static void ReadControl(std::ostringstream& out, emuptr dataPtr, bool focused)
{
	uint16 id    = EmMemGet16(dataPtr + kControlType_id);
	int16  bx    = (int16)EmMemGet16(dataPtr + kControlType_bounds_topLeft_x);
	int16  by    = (int16)EmMemGet16(dataPtr + kControlType_bounds_topLeft_y);
	int16  bw    = (int16)EmMemGet16(dataPtr + kControlType_bounds_extent_x);
	int16  bh    = (int16)EmMemGet16(dataPtr + kControlType_bounds_extent_y);
	emuptr textP = EmMemGet32(dataPtr + kControlType_text);
	uint8  style = (uint8)EmMemGet8(dataPtr + kControlType_style);

	out << (focused ? " *" : "  ");
	out << ControlStyleName(style);
	out << " id=" << id;

	if (IsValidPtr(textP))
	{
		std::string text = ReadEmuString(textP);
		out << " \"" << EscapeString(text) << "\"";
	}

	out << " " << FormatBounds(bx, by, bw, bh);

	// For checkboxes, read the value from attr bitfield
	if (style == kCheckboxCtl)
	{
		uint8 attr = (uint8)EmMemGet8(dataPtr + kControlType_attr);
		// Bit 0 of ControlAttrType is 'usable', bit 1 is 'enabled',
		// bit 2 is 'visible', bit 3 is 'on' (the checked state)
		// In m68k bit order (MSB first in byte), on = bit 4 from MSB = 0x08
		int on = (attr & 0x08) ? 1 : 0;
		out << " val=" << on;
	}

	out << "\n";
}

//============================================================================
// Read a FieldType object
//============================================================================
static void ReadField(std::ostringstream& out, emuptr dataPtr, bool focused)
{
	uint16 id    = EmMemGet16(dataPtr + kFieldType_id);
	int16  bx    = (int16)EmMemGet16(dataPtr + kFieldType_rect_topLeft_x);
	int16  by    = (int16)EmMemGet16(dataPtr + kFieldType_rect_topLeft_y);
	int16  bw    = (int16)EmMemGet16(dataPtr + kFieldType_rect_extent_x);
	int16  bh    = (int16)EmMemGet16(dataPtr + kFieldType_rect_extent_y);
	emuptr textP = EmMemGet32(dataPtr + kFieldType_text);

	out << (focused ? " *" : "  ");
	out << "FIELD id=" << id;
	out << " " << FormatBounds(bx, by, bw, bh);

	if (IsValidPtr(textP))
	{
		std::string text = ReadEmuString(textP);
		if (!text.empty())
			out << " \"" << EscapeString(text) << "\"";
	}

	out << "\n";
}

//============================================================================
// Read a ListType object
//============================================================================
static void ReadList(std::ostringstream& out, emuptr dataPtr, bool focused)
{
	uint16 id       = EmMemGet16(dataPtr + kListType_id);
	int16  bx       = (int16)EmMemGet16(dataPtr + kListType_bounds_topLeft_x);
	int16  by       = (int16)EmMemGet16(dataPtr + kListType_bounds_topLeft_y);
	int16  bw       = (int16)EmMemGet16(dataPtr + kListType_bounds_extent_x);
	int16  bh       = (int16)EmMemGet16(dataPtr + kListType_bounds_extent_y);
	int16  numItems = (int16)EmMemGet16(dataPtr + kListType_numItems);
	int16  curItem  = (int16)EmMemGet16(dataPtr + kListType_currentItem);
	int16  topItem  = (int16)EmMemGet16(dataPtr + kListType_topItem);
	emuptr itemsP   = EmMemGet32(dataPtr + kListType_itemsText);
	emuptr drawCB   = EmMemGet32(dataPtr + kListType_drawItemsCallback);

	out << (focused ? " *" : "  ");
	out << "LIST id=" << id;
	out << " " << FormatBounds(bx, by, bw, bh);
	out << " sel=" << curItem << " top=" << topItem;
	out << "\n";

	// If there's a custom draw callback, items may not have text
	if (drawCB != 0)
	{
		out << "   (custom-draw)\n";
		return;
	}

	// Read list items (itemsP is a Char** — array of string pointers)
	if (IsValidPtr(itemsP) && numItems > 0)
	{
		int maxShow = std::min((int)numItems, 20);  // cap at 20 items
		for (int i = 0; i < maxShow; ++i)
		{
			emuptr strP = EmMemGet32(itemsP + i * 4);
			out << "   [" << i << "] ";
			if (IsValidPtr(strP))
			{
				std::string text = ReadEmuString(strP);
				out << "\"" << EscapeString(text) << "\"";
			}
			else
			{
				out << "(null)";
			}
			out << "\n";
		}
		if (numItems > maxShow)
			out << "   ... (" << numItems << " total)\n";
	}
}

//============================================================================
// Read a FormTitleType object
//============================================================================
// FormTitleType: RectangleType rect (8 bytes), Char* text (4 bytes)
static const int kFormTitleType_rect_topLeft_x = 0;
static const int kFormTitleType_rect_topLeft_y = 2;
static const int kFormTitleType_rect_extent_x = 4;
static const int kFormTitleType_rect_extent_y = 6;
static const int kFormTitleType_text = 8;

static void ReadTitle(std::ostringstream& out, emuptr dataPtr)
{
	int16  bx    = (int16)EmMemGet16(dataPtr + kFormTitleType_rect_topLeft_x);
	int16  by    = (int16)EmMemGet16(dataPtr + kFormTitleType_rect_topLeft_y);
	int16  bw    = (int16)EmMemGet16(dataPtr + kFormTitleType_rect_extent_x);
	int16  bh    = (int16)EmMemGet16(dataPtr + kFormTitleType_rect_extent_y);
	emuptr textP = EmMemGet32(dataPtr + kFormTitleType_text);

	out << "  TITLE";
	if (IsValidPtr(textP))
	{
		std::string text = ReadEmuString(textP);
		out << " \"" << EscapeString(text) << "\"";
	}
	out << " " << FormatBounds(bx, by, bw, bh);
	out << "\n";
}

//============================================================================
// Read a FormLabelType object
//============================================================================
// FormLabelType: UInt16 id (2), PointType pos (4), FormObjAttrType attr (2),
//                FontID fontID (1), UInt8 reserved (1), Char* text (4) = 14 bytes
static const int kFormLabelType_id = 0;
static const int kFormLabelType_pos_x = 2;
static const int kFormLabelType_pos_y = 4;
static const int kFormLabelType_text = 10;

static void ReadLabel(std::ostringstream& out, emuptr dataPtr)
{
	uint16 id    = EmMemGet16(dataPtr + kFormLabelType_id);
	int16  px    = (int16)EmMemGet16(dataPtr + kFormLabelType_pos_x);
	int16  py    = (int16)EmMemGet16(dataPtr + kFormLabelType_pos_y);
	emuptr textP = EmMemGet32(dataPtr + kFormLabelType_text);

	out << "  LABEL id=" << id;
	if (IsValidPtr(textP))
	{
		std::string text = ReadEmuString(textP);
		out << " \"" << EscapeString(text) << "\"";
	}
	out << " (" << px << "," << py << ")";
	out << "\n";
}

//============================================================================
// Read a ScrollBarType object
//============================================================================
static void ReadScrollBar(std::ostringstream& out, emuptr dataPtr)
{
	uint16 id    = EmMemGet16(dataPtr + kScrollBarType_id);
	int16  bx    = (int16)EmMemGet16(dataPtr + kScrollBarType_bounds_topLeft_x);
	int16  by    = (int16)EmMemGet16(dataPtr + kScrollBarType_bounds_topLeft_y);
	int16  bw    = (int16)EmMemGet16(dataPtr + kScrollBarType_bounds_extent_x);
	int16  bh    = (int16)EmMemGet16(dataPtr + kScrollBarType_bounds_extent_y);
	int16  val   = (int16)EmMemGet16(dataPtr + kScrollBarType_value);
	int16  minV  = (int16)EmMemGet16(dataPtr + kScrollBarType_minValue);
	int16  maxV  = (int16)EmMemGet16(dataPtr + kScrollBarType_maxValue);

	out << "  SCROLLBAR id=" << id;
	out << " " << FormatBounds(bx, by, bw, bh);
	out << " val=" << val << " min=" << minV << " max=" << maxV;
	out << "\n";
}

//============================================================================
// Read a FormGadgetType object
//============================================================================
// FormGadgetType: UInt16 id (2), FormGadgetAttrType attr (2),
//                 RectangleType rect (8), const void* data (4),
//                 handler (4) = 20 bytes
static const int kFormGadgetType_id = 0;
static const int kFormGadgetType_rect_topLeft_x = 4;
static const int kFormGadgetType_rect_topLeft_y = 6;
static const int kFormGadgetType_rect_extent_x = 8;
static const int kFormGadgetType_rect_extent_y = 10;

static void ReadGadget(std::ostringstream& out, emuptr dataPtr)
{
	uint16 id = EmMemGet16(dataPtr + kFormGadgetType_id);
	int16  bx = (int16)EmMemGet16(dataPtr + kFormGadgetType_rect_topLeft_x);
	int16  by = (int16)EmMemGet16(dataPtr + kFormGadgetType_rect_topLeft_y);
	int16  bw = (int16)EmMemGet16(dataPtr + kFormGadgetType_rect_extent_x);
	int16  bh = (int16)EmMemGet16(dataPtr + kFormGadgetType_rect_extent_y);

	out << "  GADGET id=" << id;
	out << " " << FormatBounds(bx, by, bw, bh);
	out << "\n";
}

//============================================================================
// Read MenuBarType from emulated memory
//============================================================================
static void ReadMenuBar(std::ostringstream& out)
{
	// Read uiCurrentMenu from PalmOS low-memory globals
	emuptr menuBarPtr = EmLowMem_GetGlobal(uiGlobalsCommon.uiCurrentMenu);

	if (!IsValidPtr(menuBarPtr))
		return;  // No menu bar active — normal case

	int16  curMenu  = (int16)EmMemGet16(menuBarPtr + kMenuBarType_curMenu);
	int16  curItem  = (int16)EmMemGet16(menuBarPtr + kMenuBarType_curItem);
	int16  numMenus = (int16)EmMemGet16(menuBarPtr + kMenuBarType_numMenus);
	emuptr menusPtr = EmMemGet32(menuBarPtr + kMenuBarType_menus);

	if (!IsValidPtr(menusPtr) || numMenus <= 0 || numMenus > 20)
		return;

	out << " MENUBAR curMenu=" << curMenu << " curItem=" << curItem << "\n";

	for (int m = 0; m < numMenus; ++m)
	{
		emuptr pullDown = menusPtr + (m * kMenuPullDownType_size);

		emuptr titlePtr = EmMemGet32(pullDown + kMenuPullDownType_title);
		uint16 hiddenNumItems = EmMemGet16(pullDown + kMenuPullDownType_hiddenNumItems);
		// Bit 15 (MSB) = hidden flag, bits 14..0 = numItems
		int numItems = hiddenNumItems & 0x7FFF;
		emuptr itemsPtr = EmMemGet32(pullDown + kMenuPullDownType_items);

		out << "  MENU ";
		if (IsValidPtr(titlePtr))
		{
			std::string title = ReadEmuString(titlePtr);
			out << "\"" << EscapeString(title) << "\"";
		}
		else
		{
			out << "(null)";
		}
		out << "\n";

		if (!IsValidPtr(itemsPtr) || numItems <= 0)
			continue;

		int maxItems = std::min(numItems, 30);  // Safety cap
		for (int i = 0; i < maxItems; ++i)
		{
			emuptr item = itemsPtr + (i * kMenuItemType_size);

			uint16 id      = EmMemGet16(item + kMenuItemType_id);
			uint8  command  = (uint8)EmMemGet8(item + kMenuItemType_command);
			uint8  hiddenByte = (uint8)EmMemGet8(item + kMenuItemType_hidden);
			emuptr itemStr = EmMemGet32(item + kMenuItemType_itemStr);

			// Bit 7 (MSB) = hidden flag
			if (hiddenByte & 0x80)
				continue;

			out << "   ITEM id=" << id;

			if (IsValidPtr(itemStr))
			{
				std::string text = ReadEmuString(itemStr);
				out << " \"" << EscapeString(text) << "\"";
			}

			if (command != 0)
				out << " cmd=" << (char)command;

			out << "\n";
		}
	}
}

//============================================================================
// PalmFormReader_ReadActiveForm
//============================================================================

std::string PalmFormReader_ReadActiveForm(void)
{
	std::ostringstream result;

	// Get active form pointer from PalmOS low-memory globals
	emuptr formPtr = EmLowMem_GetGlobal(uiGlobalsCommon.currentForm);

	if (!IsValidPtr(formPtr))
	{
		result << "ERR transient: no active form\n";
		return result.str();
	}

	// Read form header
	uint16 formId     = EmMemGet16(formPtr + kFormType_formId);
	uint16 numObjects = EmMemGet16(formPtr + kFormType_numObjects);
	emuptr objectsPtr = EmMemGet32(formPtr + kFormType_objects);
	uint16 focusIdx   = EmMemGet16(formPtr + kFormType_focus);

	// Try to find the form title from its objects
	std::string formTitle;
	if (IsValidPtr(objectsPtr))
	{
		for (uint16 i = 0; i < numObjects; ++i)
		{
			emuptr objEntry = objectsPtr + (i * kFormObjListType_size);
			uint8  objType  = (uint8)EmMemGet8(objEntry + kFormObjListType_objectType);
			emuptr dataPtr  = EmMemGet32(objEntry + kFormObjListType_object);

			if (objType == kFrmTitleObj && IsValidPtr(dataPtr))
			{
				emuptr textP = EmMemGet32(dataPtr + kFormTitleType_text);
				if (IsValidPtr(textP))
					formTitle = ReadEmuString(textP);
				break;
			}
		}
	}

	// Output form header
	result << "OK FORM id=" << formId;
	if (!formTitle.empty())
		result << " \"" << EscapeString(formTitle) << "\"";
	result << "\n";

	// Read each object
	if (IsValidPtr(objectsPtr))
	{
		for (uint16 i = 0; i < numObjects; ++i)
		{
			emuptr objEntry = objectsPtr + (i * kFormObjListType_size);
			uint8  objType  = (uint8)EmMemGet8(objEntry + kFormObjListType_objectType);
			emuptr dataPtr  = EmMemGet32(objEntry + kFormObjListType_object);

			if (!IsValidPtr(dataPtr))
			{
				result << "  " << ObjTypeName(objType) << " (null)\n";
				continue;
			}

			bool focused = (i == focusIdx);

			switch (objType)
			{
				case kFrmControlObj:
					ReadControl(result, dataPtr, focused);
					break;

				case kFrmFieldObj:
					ReadField(result, dataPtr, focused);
					break;

				case kFrmListObj:
					ReadList(result, dataPtr, focused);
					break;

				case kFrmTitleObj:
					ReadTitle(result, dataPtr);
					break;

				case kFrmLabelObj:
					ReadLabel(result, dataPtr);
					break;

				case kFrmScrollBarObj:
					ReadScrollBar(result, dataPtr);
					break;

				case kFrmGadgetObj:
					ReadGadget(result, dataPtr);
					break;

				case kFrmTableObj:
					result << "  TABLE\n";
					break;

				case kFrmBitmapObj:
				case kFrmLineObj:
				case kFrmFrameObj:
				case kFrmRectangleObj:
				case kFrmPopupObj:
				case kFrmGraffitiStateObj:
					result << "  " << ObjTypeName(objType) << "\n";
					break;

				default:
					result << "  UNKNOWN type=" << objType << "\n";
					break;
			}
		}
	}

	// Append menu bar info if a menu is active
	ReadMenuBar(result);

	result << ".\n";
	return result.str();
}

//============================================================================
// PalmFormReader_GetObjectBounds
//============================================================================

std::vector<PalmObjInfo> PalmFormReader_GetObjectBounds(void)
{
	std::vector<PalmObjInfo> result;

	emuptr formPtr = EmLowMem_GetGlobal(uiGlobalsCommon.currentForm);
	if (!IsValidPtr(formPtr))
		return result;

	// Form window origin — needed to convert form-local to screen-absolute
	int16 winX = (int16)EmMemGet16(formPtr + kWindowType_windowBounds_topLeft_x);
	int16 winY = (int16)EmMemGet16(formPtr + kWindowType_windowBounds_topLeft_y);

	uint16 numObjects = EmMemGet16(formPtr + kFormType_numObjects);
	emuptr objectsPtr = EmMemGet32(formPtr + kFormType_objects);
	uint16 focusIdx   = EmMemGet16(formPtr + kFormType_focus);

	if (!IsValidPtr(objectsPtr) || numObjects == 0)
		return result;

	for (uint16 i = 0; i < numObjects; ++i)
	{
		emuptr objEntry = objectsPtr + (i * kFormObjListType_size);
		uint8  objType  = (uint8)EmMemGet8(objEntry + kFormObjListType_objectType);
		emuptr dataPtr  = EmMemGet32(objEntry + kFormObjListType_object);

		if (!IsValidPtr(dataPtr))
			continue;

		PalmObjInfo info;
		info.type    = objType;
		info.id      = 0;
		info.focused = (focusIdx != 0xFFFF) && (i == focusIdx);

		int16 bx = 0, by = 0, bw = 0, bh = 0;

		switch (objType)
		{
			case kFrmControlObj:
			{
				info.id = EmMemGet16(dataPtr + kControlType_id);
				bx = (int16)EmMemGet16(dataPtr + kControlType_bounds_topLeft_x);
				by = (int16)EmMemGet16(dataPtr + kControlType_bounds_topLeft_y);
				bw = (int16)EmMemGet16(dataPtr + kControlType_bounds_extent_x);
				bh = (int16)EmMemGet16(dataPtr + kControlType_bounds_extent_y);
				uint8 style = (uint8)EmMemGet8(dataPtr + kControlType_style);
				emuptr textP = EmMemGet32(dataPtr + kControlType_text);
				info.label = ControlStyleName(style);
				if (IsValidPtr(textP))
					info.label += std::string(" \"") + EscapeString(ReadEmuString(textP)) + "\"";
				break;
			}
			case kFrmFieldObj:
			{
				info.id = EmMemGet16(dataPtr + kFieldType_id);
				bx = (int16)EmMemGet16(dataPtr + kFieldType_rect_topLeft_x);
				by = (int16)EmMemGet16(dataPtr + kFieldType_rect_topLeft_y);
				bw = (int16)EmMemGet16(dataPtr + kFieldType_rect_extent_x);
				bh = (int16)EmMemGet16(dataPtr + kFieldType_rect_extent_y);
				info.label = "FIELD";
				break;
			}
			case kFrmListObj:
			{
				info.id = EmMemGet16(dataPtr + kListType_id);
				bx = (int16)EmMemGet16(dataPtr + kListType_bounds_topLeft_x);
				by = (int16)EmMemGet16(dataPtr + kListType_bounds_topLeft_y);
				bw = (int16)EmMemGet16(dataPtr + kListType_bounds_extent_x);
				bh = (int16)EmMemGet16(dataPtr + kListType_bounds_extent_y);
				info.label = "LIST";
				break;
			}
			case kFrmTitleObj:
			{
				bx = (int16)EmMemGet16(dataPtr + kFormTitleType_rect_topLeft_x);
				by = (int16)EmMemGet16(dataPtr + kFormTitleType_rect_topLeft_y);
				bw = (int16)EmMemGet16(dataPtr + kFormTitleType_rect_extent_x);
				bh = (int16)EmMemGet16(dataPtr + kFormTitleType_rect_extent_y);
				emuptr textP = EmMemGet32(dataPtr + kFormTitleType_text);
				info.label = "TITLE";
				if (IsValidPtr(textP))
					info.label += std::string(" \"") + EscapeString(ReadEmuString(textP)) + "\"";
				break;
			}
			case kFrmLabelObj:
			{
				info.id = EmMemGet16(dataPtr + kFormLabelType_id);
				bx = (int16)EmMemGet16(dataPtr + kFormLabelType_pos_x);
				by = (int16)EmMemGet16(dataPtr + kFormLabelType_pos_y);
				bw = 0;
				bh = 0;
				emuptr textP = EmMemGet32(dataPtr + kFormLabelType_text);
				info.label = "LABEL";
				if (IsValidPtr(textP))
					info.label += std::string(" \"") + EscapeString(ReadEmuString(textP)) + "\"";
				break;
			}
			case kFrmGadgetObj:
			{
				info.id = EmMemGet16(dataPtr + kFormGadgetType_id);
				bx = (int16)EmMemGet16(dataPtr + kFormGadgetType_rect_topLeft_x);
				by = (int16)EmMemGet16(dataPtr + kFormGadgetType_rect_topLeft_y);
				bw = (int16)EmMemGet16(dataPtr + kFormGadgetType_rect_extent_x);
				bh = (int16)EmMemGet16(dataPtr + kFormGadgetType_rect_extent_y);
				info.label = "GADGET";
				break;
			}
			case kFrmScrollBarObj:
			{
				info.id = EmMemGet16(dataPtr + kScrollBarType_id);
				bx = (int16)EmMemGet16(dataPtr + kScrollBarType_bounds_topLeft_x);
				by = (int16)EmMemGet16(dataPtr + kScrollBarType_bounds_topLeft_y);
				bw = (int16)EmMemGet16(dataPtr + kScrollBarType_bounds_extent_x);
				bh = (int16)EmMemGet16(dataPtr + kScrollBarType_bounds_extent_y);
				info.label = "SCROLLBAR";
				break;
			}
			default:
				continue;  // skip non-visual types
		}

		info.screenX = winX + bx;
		info.screenY = winY + by;
		info.w       = bw;
		info.h       = bh;
		result.push_back(info);
	}

	return result;
}
