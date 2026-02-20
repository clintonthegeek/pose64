#ifndef PalmFormReader_h
#define PalmFormReader_h

#include <string>

// M68K struct offset constants for direct memory reading of PalmOS forms.
//
// These constants define byte offsets within PalmOS structures as laid out
// in m68k memory (32-bit pointers, 2-byte alignment, big-endian).
//
// M68K ABI rules:
// - Alignment: All types 2-byte aligned
// - UInt16: 2 bytes
// - UInt32/pointers: 4 bytes (big-endian in memory)
// - Signed types: Same size as unsigned
// - Bitfields: Packed in memory (details below)
// - Structs: Start at 2-byte boundary, natural alignment applies
//
// References:
// - WindowType in Window.h
// - FormType in Form.h
// - FormObjListType in Form.h
// - ControlType in Control.h
// - FieldType in Field.h
// - ListType in List.h
// - ScrollBarType in ScrollBar.h

// ============================================================================
// WindowType Structure Analysis
// ============================================================================
// struct WindowType {
//   Coord displayWidthV20;          // Int16, +0
//   Coord displayHeightV20;         // Int16, +2
//   void *displayAddrV20;           // ptr (4 bytes), +4
//   WindowFlagsType windowFlags;    // UInt16 (bitfield), +8
//   RectangleType windowBounds;     // PointType x2, +10
//     PointType topLeft;            //   Coord x, y (+10, +12)
//     PointType extent;             //   Coord x, y (+14, +16)
//   AbsRectType clippingBounds;     // 4x Coord, +18
//   BitmapPtr bitmapP;              // ptr (4 bytes), +26
//   FrameBitsType frameType;        // UInt16 (bitfield), +30
//   DrawStateType *drawStateP;      // ptr (4 bytes), +32
//   struct WindowType *nextWindow;  // ptr (4 bytes), +36
// };
// Total m68k size: 40 bytes

constexpr int kWindowType_size = 40;
constexpr int kWindowType_displayWidthV20 = 0;
constexpr int kWindowType_displayHeightV20 = 2;
constexpr int kWindowType_displayAddrV20 = 4;
constexpr int kWindowType_windowFlags = 8;
constexpr int kWindowType_windowBounds = 10;
constexpr int kWindowType_windowBounds_topLeft = 10;
constexpr int kWindowType_windowBounds_topLeft_x = 10;
constexpr int kWindowType_windowBounds_topLeft_y = 12;
constexpr int kWindowType_windowBounds_extent = 14;
constexpr int kWindowType_windowBounds_extent_x = 14;
constexpr int kWindowType_windowBounds_extent_y = 16;
constexpr int kWindowType_clippingBounds = 18;
constexpr int kWindowType_clippingBounds_left = 18;
constexpr int kWindowType_clippingBounds_top = 20;
constexpr int kWindowType_clippingBounds_right = 22;
constexpr int kWindowType_clippingBounds_bottom = 24;
constexpr int kWindowType_bitmapP = 26;
constexpr int kWindowType_frameType = 30;
constexpr int kWindowType_drawStateP = 32;
constexpr int kWindowType_nextWindow = 36;

// ============================================================================
// FormType Structure Analysis
// ============================================================================
// struct FormType {
//   WindowType window;              // embedded, +0 (40 bytes)
//   UInt16 formId;                  // +40
//   FormAttrType attr;              // UInt16 + reserved2 (UInt16), +42 (4 bytes)
//   WinHandle bitsBehindForm;       // ptr (4 bytes), +46
//   FormEventHandlerType *handler;  // ptr (4 bytes), +50
//   UInt16 focus;                   // +54
//   UInt16 defaultButton;           // +56
//   UInt16 helpRscId;               // +58
//   UInt16 menuRscId;               // +60
//   UInt16 numObjects;              // +62
//   FormObjListType *objects;       // ptr (4 bytes), +64
// };
// Total m68k size: 68 bytes

constexpr int kFormType_size = 68;
constexpr int kFormType_window = 0;
constexpr int kFormType_formId = 40;
constexpr int kFormType_attr = 42;
constexpr int kFormType_bitsBehindForm = 46;
constexpr int kFormType_handler = 50;
constexpr int kFormType_focus = 54;
constexpr int kFormType_defaultButton = 56;
constexpr int kFormType_helpRscId = 58;
constexpr int kFormType_menuRscId = 60;
constexpr int kFormType_numObjects = 62;
constexpr int kFormType_objects = 64;

// ============================================================================
// FormObjListType Structure Analysis
// ============================================================================
// struct FormObjListType {
//   FormObjectKind objectType;      // UInt16 (enum), +0
//   UInt8 reserved;                 // +2
//   [1 byte padding]                // +3 (alignment to 4-byte ptr)
//   FormObjectType object;          // union ptr (4 bytes), +4
// };
// Each FormObjListType is 8 bytes

constexpr int kFormObjListType_size = 8;
constexpr int kFormObjListType_objectType = 0;
constexpr int kFormObjListType_reserved = 2;
constexpr int kFormObjListType_object = 4;

// FormObjectKind enum values
constexpr int kFrmFieldObj = 0;
constexpr int kFrmControlObj = 1;
constexpr int kFrmListObj = 2;
constexpr int kFrmTableObj = 3;
constexpr int kFrmBitmapObj = 4;
constexpr int kFrmLineObj = 5;
constexpr int kFrmFrameObj = 6;
constexpr int kFrmRectangleObj = 7;
constexpr int kFrmLabelObj = 8;
constexpr int kFrmTitleObj = 9;
constexpr int kFrmPopupObj = 10;
constexpr int kFrmGraffitiStateObj = 11;
constexpr int kFrmGadgetObj = 12;
constexpr int kFrmScrollBarObj = 13;

// ============================================================================
// ControlType Structure Analysis
// ============================================================================
// struct ControlType {
//   UInt16 id;                      // +0
//   RectangleType bounds;           // PointType x2, +2
//     PointType topLeft;            //   Coord x, y (+2, +4)
//     PointType extent;             //   Coord x, y (+6, +8)
//   Char *text;                     // ptr (4 bytes), +10
//   ControlAttrType attr;           // UInt8 (bitfield), +14
//   ControlStyleType style;         // UInt8 (enum), +15
//   FontID font;                    // UInt8, +16
//   UInt8 group;                    // +17
//   UInt8 reserved;                 // +18
//   [1 byte padding]                // +19 (padding to align)
// };
// Total m68k size: 20 bytes

constexpr int kControlType_size = 20;
constexpr int kControlType_id = 0;
constexpr int kControlType_bounds = 2;
constexpr int kControlType_bounds_topLeft = 2;
constexpr int kControlType_bounds_topLeft_x = 2;
constexpr int kControlType_bounds_topLeft_y = 4;
constexpr int kControlType_bounds_extent = 6;
constexpr int kControlType_bounds_extent_x = 6;
constexpr int kControlType_bounds_extent_y = 8;
constexpr int kControlType_text = 10;
constexpr int kControlType_attr = 14;
constexpr int kControlType_style = 15;
constexpr int kControlType_font = 16;
constexpr int kControlType_group = 17;
constexpr int kControlType_reserved = 18;

// ControlStyleType enum values
constexpr int kButtonCtl = 0;
constexpr int kPushButtonCtl = 1;
constexpr int kCheckboxCtl = 2;
constexpr int kPopupTriggerCtl = 3;
constexpr int kSelectorTriggerCtl = 4;
constexpr int kRepeatingButtonCtl = 5;
constexpr int kSliderCtl = 6;
constexpr int kFeedbackSliderCtl = 7;

// ============================================================================
// FieldType Structure Analysis
// ============================================================================
// struct FieldType {
//   UInt16 id;                      // +0
//   RectangleType rect;             // PointType x2, +2
//     PointType topLeft;            //   Coord x, y (+2, +4)
//     PointType extent;             //   Coord x, y (+6, +8)
//   FieldAttrType attr;             // UInt16 (bitfield), +10
//   Char *text;                     // ptr (4 bytes), +12
//   MemHandle textHandle;           // handle (4 bytes), +16
//   LineInfoPtr lines;              // ptr (4 bytes), +20
//   UInt16 textLen;                 // +24
//   UInt16 textBlockSize;           // +26
//   UInt16 maxChars;                // +28
//   UInt16 selFirstPos;             // +30
//   UInt16 selLastPos;              // +32
//   UInt16 insPtXPos;               // +34
//   UInt16 insPtYPos;               // +36
//   FontID fontID;                  // UInt8, +38
//   UInt8 reserved;                 // +39
// };
// Total m68k size: 40 bytes

constexpr int kFieldType_size = 40;
constexpr int kFieldType_id = 0;
constexpr int kFieldType_rect = 2;
constexpr int kFieldType_rect_topLeft = 2;
constexpr int kFieldType_rect_topLeft_x = 2;
constexpr int kFieldType_rect_topLeft_y = 4;
constexpr int kFieldType_rect_extent = 6;
constexpr int kFieldType_rect_extent_x = 6;
constexpr int kFieldType_rect_extent_y = 8;
constexpr int kFieldType_attr = 10;
constexpr int kFieldType_text = 12;
constexpr int kFieldType_textHandle = 16;
constexpr int kFieldType_lines = 20;
constexpr int kFieldType_textLen = 24;
constexpr int kFieldType_textBlockSize = 26;
constexpr int kFieldType_maxChars = 28;
constexpr int kFieldType_selFirstPos = 30;
constexpr int kFieldType_selLastPos = 32;
constexpr int kFieldType_insPtXPos = 34;
constexpr int kFieldType_insPtYPos = 36;
constexpr int kFieldType_fontID = 38;
constexpr int kFieldType_reserved = 39;

// ============================================================================
// ListType Structure Analysis
// ============================================================================
// struct ListType {
//   UInt16 id;                      // +0
//   RectangleType bounds;           // PointType x2, +2
//     PointType topLeft;            //   Coord x, y (+2, +4)
//     PointType extent;             //   Coord x, y (+6, +8)
//   ListAttrType attr;              // UInt16 (bitfield), +10
//   Char **itemsText;               // ptr to array of ptrs (4 bytes), +12
//   Int16 numItems;                 // +16
//   Int16 currentItem;              // +18
//   Int16 topItem;                  // +20
//   FontID font;                    // UInt8, +22
//   UInt8 reserved;                 // +23
//   WinHandle popupWin;             // ptr (4 bytes), +24
//   ListDrawDataFuncPtr drawItemsCallback; // ptr (4 bytes), +28
// };
// Total m68k size: 32 bytes

constexpr int kListType_size = 32;
constexpr int kListType_id = 0;
constexpr int kListType_bounds = 2;
constexpr int kListType_bounds_topLeft = 2;
constexpr int kListType_bounds_topLeft_x = 2;
constexpr int kListType_bounds_topLeft_y = 4;
constexpr int kListType_bounds_extent = 6;
constexpr int kListType_bounds_extent_x = 6;
constexpr int kListType_bounds_extent_y = 8;
constexpr int kListType_attr = 10;
constexpr int kListType_itemsText = 12;
constexpr int kListType_numItems = 16;
constexpr int kListType_currentItem = 18;
constexpr int kListType_topItem = 20;
constexpr int kListType_font = 22;
constexpr int kListType_reserved = 23;
constexpr int kListType_popupWin = 24;
constexpr int kListType_drawItemsCallback = 28;

// ============================================================================
// ScrollBarType Structure Analysis
// ============================================================================
// struct ScrollBarType {
//   RectangleType bounds;           // PointType x2, +0
//     PointType topLeft;            //   Coord x, y (+0, +2)
//     PointType extent;             //   Coord x, y (+4, +6)
//   UInt16 id;                      // +8
//   ScrollBarAttrType attr;         // UInt16 (bitfield), +10
//   Int16 value;                    // +12
//   Int16 minValue;                 // +14
//   Int16 maxValue;                 // +16
//   Int16 pageSize;                 // +18
//   Int16 penPosInCar;              // +20
//   Int16 savePos;                  // +22
// };
// Total m68k size: 24 bytes

constexpr int kScrollBarType_size = 24;
constexpr int kScrollBarType_bounds = 0;
constexpr int kScrollBarType_bounds_topLeft = 0;
constexpr int kScrollBarType_bounds_topLeft_x = 0;
constexpr int kScrollBarType_bounds_topLeft_y = 2;
constexpr int kScrollBarType_bounds_extent = 4;
constexpr int kScrollBarType_bounds_extent_x = 4;
constexpr int kScrollBarType_bounds_extent_y = 6;
constexpr int kScrollBarType_id = 8;
constexpr int kScrollBarType_attr = 10;
constexpr int kScrollBarType_value = 12;
constexpr int kScrollBarType_minValue = 14;
constexpr int kScrollBarType_maxValue = 16;
constexpr int kScrollBarType_pageSize = 18;
constexpr int kScrollBarType_penPosInCar = 20;
constexpr int kScrollBarType_savePos = 22;

// ============================================================================
// Helper Functions
// ============================================================================

/// Read the active form from emulated m68k memory and format as text.
/// Returns a string with:
///   "OK FORM <formId> <numObjects>\n"
///   <one line per object>
///   ".\n"
std::string PalmFormReader_ReadActiveForm(void);

#endif // PalmFormReader_h
