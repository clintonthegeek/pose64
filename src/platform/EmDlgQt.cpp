/* -*- mode: C++; tab-width: 4 -*- */
/* Qt PORT v2: Dialog implementations.
 *
 * Reuses the working New Session dialog from v1.
 * Implements HandleDialogs() and CloseAllDialogs() as stubs
 * (the FLTK modeless dialog infrastructure is not needed for Qt
 *  since we use native modal Qt dialogs directly).
 */

#include "EmCommon.h"
#include "EmDlg.h"
#include "EmDlgQt.h"
#include "EmApplication.h"		// gApplication, IsBoundFully
#include "EmDevice.h"
#include "EmFileRef.h"
#include "EmPatchState.h"		// EmPatchState::UIInitialized
#include "EmSession.h"			// gSession, EmSessionStopper
#include "EmWindow.h"			// gWindow, WindowReset
#include "EmStructs.h"			// Configuration, SlotInfoType
#include "EmTypes.h"			// EmResetType, EmErrorHandlingOption
#include "Logging.h"			// FOR_EACH_LOG_PREF, FOR_EACH_REPORT_PREF
#include "Miscellaneous.h"		// MemoryTextList, GetMemoryTextList, SetHotSyncUserName
#include "Platform.h"			// Platform::GetString, Platform::GetMilliseconds
#include "PreferenceMgr.h"		// gEmuPrefs
#include "Hordes.h"				// Hordes::New, GremlinNumber, etc.
#include "LoadApplication.h"	// SavePalmFile
#include "EmStreamFile.h"		// EmStreamFile
#include "ROMStubs.h"			// DmDatabaseInfo
#include "Skins.h"				// SkinGetSkinNames, SkinSetSkinName
#include "Strings.r.h"			// kStr_OK, kStr_Cancel, etc.
#include "EmTransportSerial.h"	// EmTransportSerial::GetDescriptorList
#include "EmTransportSocket.h"	// EmTransportSocket::GetDescriptorList

// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear

#include <QFileDialog>
#include <QMessageBox>
#include <QString>
#include <QApplication>
#include <QDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QTimer>

using namespace std;


// ---------------------------------------------------------------------------
//	Gremlin Control modeless window state
// ---------------------------------------------------------------------------

static QDialog* sGremlinControlDlg = nullptr;


// ---------------------------------------------------------------------------
//	HandleDialogs / CloseAllDialogs
// ---------------------------------------------------------------------------
// The FLTK version maintains a list of modeless dialogs and processes
// their events during idle.  For Qt, we use native modal dialogs
// (QDialog::exec) which handle their own event loops.  These functions
// are no-ops.

void HandleDialogs (void)
{
	// Nothing to do — Qt dialogs handle their own events.
}

void CloseAllDialogs (void)
{
	// Nothing to do.
}


// ---------------------------------------------------------------------------
//	Helper types and functions
// ---------------------------------------------------------------------------

// Mirror the SessionNewData struct from EmDlg.cpp
struct SessionNewData
{
	Configuration*			cfg;
	Configuration			fWorkingCfg;
	EmDeviceList			fDevices;
};

static QString GetFileFilter (EmFileType fileType)
{
	switch (fileType)
	{
		case kFileTypeROM:
			return "ROM Files (*.rom *.bin);;All Files (*)";
		case kFileTypeSession:
			return "Session Files (*.psf);;All Files (*)";
		case kFileTypePalmApp:
			return "Palm Applications (*.prc);;All Files (*)";
		case kFileTypePalmDB:
			return "Palm Databases (*.pdb);;All Files (*)";
		case kFileTypePalmAll:
			return "Palm Files (*.prc *.pdb);;All Files (*)";
		case kFileTypePicture:
			return "PNG Images (*.png);;All Files (*)";
		case kFileTypeAll:
		default:
			return "All Files (*)";
	}
}

// Filter device list to those compatible with the given ROM.
static EmDeviceList FilterDevicesForROM (const EmFileRef& romFile)
{
	EmDeviceList all = EmDevice::GetDeviceList ();

	if (!romFile.IsSpecified () || !romFile.Exists ())
		return all;

	EmDeviceList filtered;
	for (EmDeviceList::iterator it = all.begin (); it != all.end (); ++it)
	{
		if (it->SupportsROM (romFile))
			filtered.push_back (*it);
	}

	if (filtered.empty ())
		return all;

	return filtered;
}

// Filter memory sizes to those >= device minimum.
static MemoryTextList FilterMemorySizes (const EmDevice& device)
{
	MemoryTextList sizes;
	::GetMemoryTextList (sizes);

	RAMSizeType minSize = device.MinRAMSize ();

	MemoryTextList filtered;
	for (MemoryTextList::iterator it = sizes.begin (); it != sizes.end (); ++it)
	{
		if (it->first >= minSize)
			filtered.push_back (*it);
	}

	return filtered;
}

static int PopulateDeviceCombo (QComboBox* combo, const EmDeviceList& devices,
								const EmDevice& currentDevice)
{
	combo->blockSignals (true);
	combo->clear ();

	int selectedIndex = 0;
	for (EmDeviceList::size_type i = 0; i < devices.size (); i++)
	{
		combo->addItem (QString::fromStdString (devices[i].GetMenuString ()));
		if (devices[i] == currentDevice)
			selectedIndex = (int) i;
	}

	combo->setCurrentIndex (selectedIndex);
	combo->blockSignals (false);
	return selectedIndex;
}

static int PopulateRAMCombo (QComboBox* combo, const MemoryTextList& sizes,
							 RAMSizeType currentRAM)
{
	combo->blockSignals (true);
	combo->clear ();

	int selectedIndex = 0;
	for (MemoryTextList::size_type i = 0; i < sizes.size (); i++)
	{
		combo->addItem (QString::fromStdString (sizes[i].second));
		if (sizes[i].first == currentRAM)
			selectedIndex = (int) i;
	}

	combo->setCurrentIndex (selectedIndex);
	combo->blockSignals (false);
	return selectedIndex;
}


// ---------------------------------------------------------------------------
//	PrvHostSessionNew — Qt implementation of the New Session dialog
// ---------------------------------------------------------------------------

static EmDlgItemID PrvHostSessionNew (void* userData)
{
	SessionNewData& data = *(SessionNewData*) userData;
	Configuration   cfg  = *(data.cfg);

	QDialog dlg;
	dlg.setWindowTitle ("New Session");
	dlg.setMinimumWidth (600);

	QVBoxLayout* topLayout = new QVBoxLayout (&dlg);
	QFormLayout* form = new QFormLayout;
	topLayout->addLayout (form);

	// ROM selector: combo + browse button
	QHBoxLayout* romLayout = new QHBoxLayout;
	QComboBox* romCombo = new QComboBox;
	romCombo->setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Fixed);
	QPushButton* browseBtn = new QPushButton ("Browse...");
	romLayout->addWidget (romCombo, 1);
	romLayout->addWidget (browseBtn);
	form->addRow ("ROM File:", romLayout);

	// Device selector
	QComboBox* deviceCombo = new QComboBox;
	form->addRow ("Device:", deviceCombo);

	// Skin selector
	QComboBox* skinCombo = new QComboBox;
	form->addRow ("Skin:", skinCombo);

	// RAM size selector
	QComboBox* ramCombo = new QComboBox;
	form->addRow ("RAM Size:", ramCombo);

	// OK/Cancel buttons
	QDialogButtonBox* buttons = new QDialogButtonBox (
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	topLayout->addWidget (buttons);

	QPushButton* okButton = buttons->button (QDialogButtonBox::Ok);

	// --- State tracking ---
	EmDeviceList   currentDevices;
	MemoryTextList currentSizes;
	SkinNameList   currentSkins;

	auto refreshSkins = [&]() {
		currentSkins.clear ();
		SkinGetSkinNames (cfg.fDevice, currentSkins);

		skinCombo->blockSignals (true);
		skinCombo->clear ();

		// Pick the saved skin name for this device, if any.
		SkinName savedSkin = SkinGetSkinName (cfg.fDevice);
		int selectedIdx = 0;

		for (SkinNameList::size_type i = 0; i < currentSkins.size (); ++i)
		{
			skinCombo->addItem (QString::fromStdString (currentSkins[i]));
			if (currentSkins[i] == savedSkin)
				selectedIdx = (int) i;
		}

		// If there's a non-generic skin, default to index 1 (first real skin)
		// unless a preference was saved.
		if (selectedIdx == 0 && currentSkins.size () > 1 && savedSkin.empty ())
			selectedIdx = 1;

		skinCombo->setCurrentIndex (selectedIdx);
		skinCombo->blockSignals (false);
	};

	auto refreshFromROM = [&]() {
		currentDevices = FilterDevicesForROM (cfg.fROMFile);
		int devIdx = PopulateDeviceCombo (deviceCombo, currentDevices, cfg.fDevice);
		if (!currentDevices.empty ())
			cfg.fDevice = currentDevices[devIdx];

		refreshSkins ();

		currentSizes = FilterMemorySizes (cfg.fDevice);
		int ramIdx = PopulateRAMCombo (ramCombo, currentSizes, cfg.fRAMSize);
		if (!currentSizes.empty ())
			cfg.fRAMSize = currentSizes[ramIdx].first;

		okButton->setEnabled (cfg.fROMFile.IsSpecified ());
	};

	auto refreshFromDevice = [&]() {
		refreshSkins ();

		currentSizes = FilterMemorySizes (cfg.fDevice);
		int ramIdx = PopulateRAMCombo (ramCombo, currentSizes, cfg.fRAMSize);
		if (!currentSizes.empty ())
			cfg.fRAMSize = currentSizes[ramIdx].first;
	};

	// --- Populate ROM MRU combo ---
	EmFileRefList romMRU;
	gEmuPrefs->GetROMMRU (romMRU);

	romCombo->blockSignals (true);
	int romSelectedIndex = -1;
	for (EmFileRefList::size_type i = 0; i < romMRU.size (); i++)
	{
		romCombo->addItem (QString::fromStdString (romMRU[i].GetName ()),
						   QString::fromStdString (romMRU[i].GetFullPath ()));
		if (cfg.fROMFile.IsSpecified () && romMRU[i] == cfg.fROMFile)
			romSelectedIndex = (int) i;
	}

	if (romSelectedIndex < 0 && !romMRU.empty ())
	{
		romSelectedIndex = 0;
		cfg.fROMFile = romMRU[0];
	}

	if (romSelectedIndex >= 0)
		romCombo->setCurrentIndex (romSelectedIndex);
	romCombo->blockSignals (false);

	refreshFromROM ();

	// --- Connect signals ---
	QObject::connect (romCombo, QOverload<int>::of (&QComboBox::currentIndexChanged),
		[&](int index) {
			if (index >= 0 && index < (int) romMRU.size ())
			{
				cfg.fROMFile = romMRU[index];
				refreshFromROM ();
			}
		});

	QObject::connect (deviceCombo, QOverload<int>::of (&QComboBox::currentIndexChanged),
		[&](int index) {
			if (index >= 0 && index < (int) currentDevices.size ())
			{
				cfg.fDevice = currentDevices[index];
				refreshFromDevice ();
			}
		});

	QObject::connect (ramCombo, QOverload<int>::of (&QComboBox::currentIndexChanged),
		[&](int index) {
			if (index >= 0 && index < (int) currentSizes.size ())
			{
				cfg.fRAMSize = currentSizes[index].first;
			}
		});

	QObject::connect (browseBtn, &QPushButton::clicked, [&]() {
		QString startDir;
		if (cfg.fROMFile.IsSpecified ())
		{
			EmDirRef parent = cfg.fROMFile.GetParent ();
			if (parent.IsSpecified ())
				startDir = QString::fromStdString (parent.GetFullPath ());
		}

		QString fileName = QFileDialog::getOpenFileName (
			&dlg, "Choose ROM File", startDir,
			"ROM Files (*.rom *.bin);;All Files (*)");

		if (fileName.isEmpty ())
			return;

		EmFileRef newROM (fileName.toStdString ());
		if (!newROM.Exists ())
			return;

		gEmuPrefs->UpdateROMMRU (newROM);

		romMRU.clear ();
		gEmuPrefs->GetROMMRU (romMRU);

		romCombo->blockSignals (true);
		romCombo->clear ();
		int newIndex = 0;
		for (EmFileRefList::size_type i = 0; i < romMRU.size (); i++)
		{
			romCombo->addItem (QString::fromStdString (romMRU[i].GetName ()),
							   QString::fromStdString (romMRU[i].GetFullPath ()));
			if (romMRU[i] == newROM)
				newIndex = (int) i;
		}
		romCombo->setCurrentIndex (newIndex);
		romCombo->blockSignals (false);

		cfg.fROMFile = newROM;
		refreshFromROM ();
	});

	QObject::connect (buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect (buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec () == QDialog::Accepted)
	{
		// Persist the selected skin for this device.
		int skinIdx = skinCombo->currentIndex ();
		if (skinIdx >= 0 && skinIdx < (int) currentSkins.size ())
		{
			SkinSetSkinName (cfg.fDevice, currentSkins[skinIdx]);
		}

		*(data.cfg) = cfg;
		return kDlgItemOK;
	}

	return kDlgItemCancel;
}


// ---------------------------------------------------------------------------
//	PrvHostDatabaseImport — drive the import callback synchronously
// ---------------------------------------------------------------------------
// The FLTK version shows a progress dialog while the callback processes files
// via init/idle events.  For Qt, we drive the callback synchronously — import
// is fast enough for typical PRC files that no progress dialog is needed.

static EmDlgItemID PrvHostDatabaseImport (EmDlgFn fn, void* userData)
{
	EmDlgContext context;
	context.fFn       = fn;
	context.fUserData = userData;
	context.fDlg      = nullptr;
	context.fDlgID    = kDlgDatabaseImport;

	// Init: sets up the importer
	context.fCommandID = kDlgCmdInit;
	context.fItemID    = kDlgItemNone;
	fn (context);

	// Idle: process chunks until the callback says it's done
	context.fCommandID = kDlgCmdIdle;
	for (int iterations = 0; iterations < 100000; ++iterations)
	{
		EmDlgFnResult result = fn (context);
		if (result == kDlgResultClose)
			break;

		QApplication::processEvents ();
	}

	// Destroy
	context.fCommandID = kDlgCmdDestroy;
	fn (context);

	return kDlgItemOK;
}


// ---------------------------------------------------------------------------
//	PrvHostReset — Reset type dialog
// ---------------------------------------------------------------------------

static EmDlgItemID PrvHostReset (EmDlgFn /*fn*/, void* userData)
{
	EmResetType& choice = *(EmResetType*) userData;

	QDialog dlg;
	dlg.setWindowTitle ("Reset");

	QVBoxLayout* topLayout = new QVBoxLayout (&dlg);

	// --- Reset Type group ---

	QGroupBox* resetGroup = new QGroupBox ("Reset Type");
	QGridLayout* resetLayout = new QGridLayout (resetGroup);

	QPushButton* softBtn = new QPushButton ("Soft reset");
	QLabel* softDesc = new QLabel (
		"This is the same as inserting a pin in the reset hole on "
		"a device.  It performs a standard reset.");
	softDesc->setWordWrap (true);
	resetLayout->addWidget (softBtn,  0, 0, Qt::AlignTop);
	resetLayout->addWidget (softDesc, 0, 1);

	QPushButton* hardBtn = new QPushButton ("Hard reset");
	QLabel* hardDesc = new QLabel (
		"This is the same as a Soft Reset while holding down "
		"the Power key.  It erases the storage heap.");
	hardDesc->setWordWrap (true);
	resetLayout->addWidget (hardBtn,  1, 0, Qt::AlignTop);
	resetLayout->addWidget (hardDesc, 1, 1);

	QPushButton* debugBtn = new QPushButton ("Debug reset");
	QLabel* debugDesc = new QLabel (
		"This is the same as a Soft Reset while holding down "
		"the Page Down key.  It causes the ROM to execute "
		"a DbgBreak early in the boot sequence.");
	debugDesc->setWordWrap (true);
	resetLayout->addWidget (debugBtn,  2, 0, Qt::AlignTop);
	resetLayout->addWidget (debugDesc, 2, 1);

	resetLayout->setColumnStretch (1, 1);
	topLayout->addWidget (resetGroup);

	// --- Extensions group ---

	QGroupBox* extGroup = new QGroupBox ("Extensions");
	QGridLayout* extLayout = new QGridLayout (extGroup);

	QCheckBox* noExtCheck = new QCheckBox ("No extensions");
	QLabel* noExtDesc = new QLabel (
		"This is the same as a Soft Reset while holding down "
		"the Page Up key.  It skips the loading of extensions, "
		"patches, and certain libraries, as well as inhibiting "
		"the sending of sysAppLaunchCmdSystemReset to "
		"applications.");
	noExtDesc->setWordWrap (true);
	extLayout->addWidget (noExtCheck, 0, 0, Qt::AlignTop);
	extLayout->addWidget (noExtDesc,  0, 1);

	extLayout->setColumnStretch (1, 1);
	topLayout->addWidget (extGroup);

	// --- Cancel ---

	QHBoxLayout* btnLayout = new QHBoxLayout;
	btnLayout->addStretch ();
	QPushButton* cancelBtn = new QPushButton ("Cancel");
	btnLayout->addWidget (cancelBtn);
	topLayout->addLayout (btnLayout);

	softBtn->setDefault (true);

	EmResetType result = kResetSoft;
	bool accepted = false;

	QObject::connect (softBtn, &QPushButton::clicked, [&]() {
		result = kResetSoft; accepted = true; dlg.accept ();
	});
	QObject::connect (hardBtn, &QPushButton::clicked, [&]() {
		result = kResetHard; accepted = true; dlg.accept ();
	});
	QObject::connect (debugBtn, &QPushButton::clicked, [&]() {
		result = kResetDebug; accepted = true; dlg.accept ();
	});
	QObject::connect (cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

	dlg.exec ();

	if (!accepted)
		return kDlgItemCancel;

	if (noExtCheck->isChecked ())
		result = (EmResetType) ((int) result | kResetNoExt);

	choice = result;
	return kDlgItemOK;
}


// ---------------------------------------------------------------------------
//	PrvHostCommonDialog — error/warning/info message box
// ---------------------------------------------------------------------------
// The POSE common dialog has up to 3 configurable buttons.  We decode the
// button flags and present a QMessageBox.  Returns kDlgItemCmnButton1/2/3
// so that DoCommonDialog can translate to the button ID (kDlgItemOK, etc.).

struct PrvCommonDialogData
{
	const char*          fMessage;
	EmCommonDialogFlags  fFlags;
};

static EmDlgItemID PrvHostCommonDialog (EmDlgFn /*fn*/, void* userData)
{
	PrvCommonDialogData& data = *(PrvCommonDialogData*) userData;

	// Decode buttons from flags.  Each button is an 8-bit field
	// with ID in low 4 bits and visibility/enabled flags in high 4.
	struct ButtonInfo {
		QString     label;
		EmDlgItemID id;		// kDlgItemOK, kDlgItemCancel, etc.
		bool        isDefault;
		bool        visible;
	};

	ButtonInfo buttons[3];
	int visibleCount = 0;

	for (int ii = 0; ii < 3; ++ii)
	{
		int flags = GET_BUTTON (ii, data.fFlags);
		buttons[ii].visible   = (flags & kButtonVisible) != 0;
		buttons[ii].isDefault = (flags & kButtonDefault) != 0;
		buttons[ii].id        = (EmDlgItemID) (flags & kButtonMask);

		if (buttons[ii].visible)
		{
			visibleCount++;

			// Map button ID to label text
			switch (buttons[ii].id)
			{
				case kDlgItemOK:       buttons[ii].label = "OK";       break;
				case kDlgItemCancel:   buttons[ii].label = "Cancel";   break;
				case kDlgItemYes:      buttons[ii].label = "Yes";      break;
				case kDlgItemNo:       buttons[ii].label = "No";       break;
				case kDlgItemContinue: buttons[ii].label = "Continue"; break;
				case kDlgItemDebug:    buttons[ii].label = "Debug";    break;
				case kDlgItemReset:    buttons[ii].label = "Reset";    break;
				default:               buttons[ii].label = "OK";       break;
			}
		}
	}

	// Build a QMessageBox
	QMessageBox msgBox;
	msgBox.setWindowTitle ("POSE64");
	msgBox.setText (QString::fromUtf8 (data.fMessage));

	// Add buttons and track which QPushButton maps to which slot
	QPushButton* qButtons[3] = { nullptr, nullptr, nullptr };

	for (int ii = 0; ii < 3; ++ii)
	{
		if (buttons[ii].visible)
		{
			QMessageBox::ButtonRole role = QMessageBox::AcceptRole;
			if (buttons[ii].id == kDlgItemCancel || buttons[ii].id == kDlgItemNo)
				role = QMessageBox::RejectRole;

			qButtons[ii] = msgBox.addButton (buttons[ii].label, role);

			if (buttons[ii].isDefault)
				msgBox.setDefaultButton (qButtons[ii]);
		}
	}

	msgBox.exec ();

	QAbstractButton* clicked = msgBox.clickedButton ();

	for (int ii = 0; ii < 3; ++ii)
	{
		if (qButtons[ii] && clicked == qButtons[ii])
			return (EmDlgItemID) (kDlgItemCmnButton1 + ii);
	}

	// Fallback: return first visible button
	return kDlgItemCmnButton1;
}


// ---------------------------------------------------------------------------
//	PrvInsertSpaces — convert "FreeChunkAccess" to "Free Chunk Access"
// ---------------------------------------------------------------------------

static QString PrvInsertSpaces (const char* name)
{
	QString result;
	for (const char* p = name; *p; ++p)
	{
		if (p != name && isupper (*p) && islower (*(p - 1)))
			result += ' ';
		result += QChar (*p);
	}
	return result;
}


// ---------------------------------------------------------------------------
//	PrvHostSessionInfo — Session Info dialog (read-only)
// ---------------------------------------------------------------------------

static EmDlgItemID PrvHostSessionInfo (void)
{
	Preference<Configuration>	prefCfg (kPrefKeyLastConfiguration);
	Preference<EmFileRef>		prefPSF (kPrefKeyLastPSF);

	const Configuration& cfg = *prefCfg;

	QDialog dlg;
	dlg.setWindowTitle ("Session Info");

	QFormLayout* form = new QFormLayout (&dlg);

	form->addRow ("Device:", new QLabel (QString::fromStdString (cfg.fDevice.GetMenuString ())));

	QString ramStr = QString ("%1 K").arg (cfg.fRAMSize);
	form->addRow ("RAM Size:", new QLabel (ramStr));

	form->addRow ("ROM File:", new QLabel (QString::fromStdString (cfg.fROMFile.GetFullPath ())));

	const EmFileRef& psf = *prefPSF;
	QString sessionStr = psf.IsSpecified () ? QString::fromStdString (psf.GetFullPath ()) : "(Untitled)";
	form->addRow ("Session:", new QLabel (sessionStr));

	// Show PTY slave paths for active serial transports
	struct { PrefKeyType key; const char* label; } portPrefs[] = {
		{ kPrefKeyPortSerial,  "Serial PTY:" },
		{ kPrefKeyPortIR,      "IR PTY:" },
		{ kPrefKeyPortMystery, "Mystery PTY:" },
	};

	for (const auto& pp : portPrefs)
	{
		Preference<EmTransportDescriptor> pref (pp.key);
		EmTransportDescriptor desc = *pref;

		if (desc.GetType () != kTransportSerial)
			continue;

		// Check for an active transport with a PTY slave
		EmTransportSerial::ConfigSerial	tmpCfg;
		tmpCfg.fPort = desc.GetSchemeSpecific ();

		EmTransportSerial* transport = EmTransportSerial::GetTransport (tmpCfg);
		if (!transport)
			continue;

		string slaveName = transport->GetPtySlaveName ();
		if (!slaveName.empty ())
		{
			QLabel* pathLabel = new QLabel (QString::fromStdString (slaveName));
			pathLabel->setTextInteractionFlags (Qt::TextSelectableByMouse);
			form->addRow (pp.label, pathLabel);
		}
	}

	QDialogButtonBox* buttons = new QDialogButtonBox (QDialogButtonBox::Ok);
	form->addRow (buttons);

	QObject::connect (buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);

	dlg.exec ();
	return kDlgItemOK;
}


// ---------------------------------------------------------------------------
//	PrvHostDebuggingOptions — 18 violation-report checkboxes + Dialog Beep
// ---------------------------------------------------------------------------

static EmDlgItemID PrvHostDebuggingOptions (void)
{
	// Read current preferences
	struct PrefEntry { const char* name; PrefKeyType key; bool value; };

	PrefEntry entries[] = {
		#undef ENTRY
		#define ENTRY(name) { #name + 6, kPrefKey##name, false },
		FOR_EACH_REPORT_PREF (ENTRY)
	};
	const int count = sizeof(entries) / sizeof(entries[0]);

	for (int i = 0; i < count; ++i)
	{
		Preference<bool> pref (entries[i].key);
		entries[i].value = *pref;
	}

	Preference<bool> prefBeep (kPrefKeyDialogBeep);
	bool beepValue = *prefBeep;

	// Build dialog
	QDialog dlg;
	dlg.setWindowTitle ("Debugging Options");

	QVBoxLayout* topLayout = new QVBoxLayout (&dlg);

	QGroupBox* group = new QGroupBox ("Violation Reports");
	QGridLayout* grid = new QGridLayout (group);

	QCheckBox* checks[count];
	for (int i = 0; i < count; ++i)
	{
		checks[i] = new QCheckBox (PrvInsertSpaces (entries[i].name));
		checks[i]->setChecked (entries[i].value);
		grid->addWidget (checks[i], i / 2, i % 2);
	}
	topLayout->addWidget (group);

	QCheckBox* beepCheck = new QCheckBox ("Beep on Dialog");
	beepCheck->setChecked (beepValue);
	topLayout->addWidget (beepCheck);

	QDialogButtonBox* buttons = new QDialogButtonBox (
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	topLayout->addWidget (buttons);

	QObject::connect (buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect (buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec () == QDialog::Accepted)
	{
		for (int i = 0; i < count; ++i)
		{
			Preference<bool> pref (entries[i].key);
			pref = checks[i]->isChecked ();
		}
		Preference<bool> pBeep (kPrefKeyDialogBeep);
		pBeep = beepCheck->isChecked ();
		return kDlgItemOK;
	}

	return kDlgItemCancel;
}


// ---------------------------------------------------------------------------
//	PrvHostPreferences — General emulator preferences
// ---------------------------------------------------------------------------

static EmDlgItemID PrvHostPreferences (void)
{
	// Read current prefs
	Preference<EmTransportDescriptor>	prefPortSerial (kPrefKeyPortSerial);
	Preference<EmTransportDescriptor>	prefPortIR (kPrefKeyPortIR);
	Preference<EmTransportDescriptor>	prefPortMystery (kPrefKeyPortMystery);
	Preference<bool>					prefNetLib (kPrefKeyRedirectNetLib);
	Preference<bool>					prefSounds (kPrefKeyEnableSounds);
	Preference<CloseActionType>			prefClose (kPrefKeyCloseAction);
	Preference<string>					prefUser (kPrefKeyUserName);

	QDialog dlg;
	dlg.setWindowTitle ("Preferences");

	QVBoxLayout* topLayout = new QVBoxLayout (&dlg);
	QFormLayout* form = new QFormLayout;
	topLayout->addLayout (form);

	// Build combined transport descriptor list: None, serial ports, socket
	EmTransportDescriptorList	portDescList;

	{
		EmTransportDescriptorList	nullList;
		EmTransportNull::GetDescriptorList (nullList);
		portDescList.insert (portDescList.end (), nullList.begin (), nullList.end ());

		EmTransportDescriptorList	serialList;
		EmTransportSerial::GetDescriptorList (serialList);
		portDescList.insert (portDescList.end (), serialList.begin (), serialList.end ());

		EmTransportDescriptorList	socketList;
		EmTransportSocket::GetDescriptorList (socketList);
		portDescList.insert (portDescList.end (), socketList.begin (), socketList.end ());
	}

	// Helper to create and populate a port combo box
	auto makePortCombo = [&](const EmTransportDescriptor& currentDesc) -> QComboBox*
	{
		QComboBox* combo = new QComboBox;
		int currentIndex = 0;

		for (size_t i = 0; i < portDescList.size (); i++)
		{
			string menuName = portDescList[i].GetMenuName ();
			if (menuName.empty ())
				menuName = "None";

			combo->addItem (QString::fromStdString (menuName));

			if (portDescList[i] == currentDesc)
				currentIndex = (int) i;
		}

		combo->setCurrentIndex (currentIndex);
		return combo;
	};

	// Port selection combo boxes
	QComboBox* comboSerial  = makePortCombo (*prefPortSerial);
	QComboBox* comboIR      = makePortCombo (*prefPortIR);
	QComboBox* comboMystery = makePortCombo (*prefPortMystery);

	form->addRow ("Serial Port:", comboSerial);
	form->addRow ("IR Port:",     comboIR);
	form->addRow ("Mystery Port:", comboMystery);

	QCheckBox* netLibCheck = new QCheckBox ("Redirect NetLib Calls to Host TCP/IP");
	netLibCheck->setChecked (*prefNetLib);
	topLayout->addWidget (netLibCheck);

	QCheckBox* soundCheck = new QCheckBox ("Enable Sounds");
	soundCheck->setChecked (*prefSounds);
	topLayout->addWidget (soundCheck);

	// Close action radio group
	QGroupBox* closeGroup = new QGroupBox ("On Close/Quit");
	QVBoxLayout* closeLayout = new QVBoxLayout (closeGroup);
	QRadioButton* saveAlways = new QRadioButton ("Always Save Session");
	QRadioButton* saveAsk    = new QRadioButton ("Ask to Save Session");
	QRadioButton* saveNever  = new QRadioButton ("Never Save Session");
	closeLayout->addWidget (saveAlways);
	closeLayout->addWidget (saveAsk);
	closeLayout->addWidget (saveNever);
	topLayout->addWidget (closeGroup);

	switch (*prefClose)
	{
		case kSaveAlways: saveAlways->setChecked (true); break;
		case kSaveAsk:    saveAsk->setChecked (true);    break;
		case kSaveNever:  saveNever->setChecked (true);  break;
	}

	// User name
	QLineEdit* userEdit = new QLineEdit (QString::fromStdString (*prefUser));
	userEdit->setMaxLength (40);
	form->addRow ("HotSync User Name:", userEdit);

	QDialogButtonBox* buttons = new QDialogButtonBox (
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	topLayout->addWidget (buttons);

	QObject::connect (buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect (buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec () == QDialog::Accepted)
	{
		string newUserName = userEdit->text ().toStdString ();

		// Validate user name length (dlkMaxUserNameLength = 40)
		if (newUserName.size () > 40)
		{
			EmDlg::DoCommonDialog ("User name is too long (max 40 characters).", kDlgFlags_OK);
			return kDlgItemCancel;
		}

		// Write port prefs
		{
			Preference<EmTransportDescriptor> p (kPrefKeyPortSerial);
			p = portDescList[comboSerial->currentIndex ()];
		}
		{
			Preference<EmTransportDescriptor> p (kPrefKeyPortIR);
			p = portDescList[comboIR->currentIndex ()];
		}
		{
			Preference<EmTransportDescriptor> p (kPrefKeyPortMystery);
			p = portDescList[comboMystery->currentIndex ()];
		}

		// Write prefs
		{
			Preference<bool> p (kPrefKeyRedirectNetLib);
			p = netLibCheck->isChecked ();
		}
		{
			Preference<bool> p (kPrefKeyEnableSounds);
			p = soundCheck->isChecked ();
		}
		{
			Preference<CloseActionType> p (kPrefKeyCloseAction);
			if (saveAlways->isChecked ())     p = kSaveAlways;
			else if (saveAsk->isChecked ())   p = kSaveAsk;
			else                              p = kSaveNever;
		}
		{
			Preference<string> p (kPrefKeyUserName);
			p = newUserName;
		}

		// Update running session if UI is initialized
		if (gSession && EmPatchState::UIInitialized ())
		{
			EmSessionStopper stopper (gSession, kStopOnSysCall);
			if (stopper.Stopped ())
			{
				::SetHotSyncUserName (newUserName.c_str ());
				gEmuPrefs->SetTransports ();
			}
		}

		return kDlgItemOK;
	}

	return kDlgItemCancel;
}


// ---------------------------------------------------------------------------
//	PrvHostEditSkins — Skin and display preferences
// ---------------------------------------------------------------------------

static EmDlgItemID PrvHostEditSkins (void)
{
	Preference<Configuration> prefCfg (kPrefKeyLastConfiguration);
	EmDevice device = (*prefCfg).fDevice;

	SkinNameList skinNames;
	SkinGetSkinNames (device, skinNames);
	SkinName currentSkin = SkinGetSkinName (device);

	Preference<ScaleType> prefScale (kPrefKeyScale);
	Preference<bool> prefDim (kPrefKeyDimWhenInactive);
	Preference<bool> prefDebug (kPrefKeyShowDebugMode);
	Preference<bool> prefGremlin (kPrefKeyShowGremlinMode);
	Preference<bool> prefOnTop (kPrefKeyStayOnTop);
	Preference<bool> prefFrameless (kPrefKeyFramelessWindow);
	Preference<bool> prefFeather (kPrefKeyFeatheredEdges);
	Preference<bool> prefTransparent (kPrefKeyTransparentLCD);

	QDialog dlg;
	dlg.setWindowTitle ("Skins");

	QVBoxLayout* topLayout = new QVBoxLayout (&dlg);
	QFormLayout* form = new QFormLayout;
	topLayout->addLayout (form);

	QComboBox* skinCombo = new QComboBox;
	int skinIdx = 0;
	for (SkinNameList::size_type i = 0; i < skinNames.size (); ++i)
	{
		skinCombo->addItem (QString::fromStdString (skinNames[i]));
		if (skinNames[i] == currentSkin)
			skinIdx = (int) i;
	}
	skinCombo->setCurrentIndex (skinIdx);
	form->addRow ("Skin:", skinCombo);

	QCheckBox* doubleCheck = new QCheckBox ("Double Size");
	doubleCheck->setChecked (*prefScale >= 2);
	topLayout->addWidget (doubleCheck);

	QCheckBox* dimCheck = new QCheckBox ("Dim When Inactive");
	dimCheck->setChecked (*prefDim);
	topLayout->addWidget (dimCheck);

	QCheckBox* debugCheck = new QCheckBox ("Show Debug Mode");
	debugCheck->setChecked (*prefDebug);
	topLayout->addWidget (debugCheck);

	QCheckBox* gremlinCheck = new QCheckBox ("Show Gremlin Mode");
	gremlinCheck->setChecked (*prefGremlin);
	topLayout->addWidget (gremlinCheck);

	QCheckBox* onTopCheck = new QCheckBox ("Stay On Top");
	onTopCheck->setChecked (*prefOnTop);
	topLayout->addWidget (onTopCheck);

	QCheckBox* framelessCheck = new QCheckBox ("Frameless Window (skin-shaped)");
	framelessCheck->setChecked (*prefFrameless);
	topLayout->addWidget (framelessCheck);

	QCheckBox* featherCheck = new QCheckBox ("Feathered Edges (anti-aliased)");
	featherCheck->setChecked (*prefFeather);
	topLayout->addWidget (featherCheck);

	QCheckBox* transparentCheck = new QCheckBox ("Transparent LCD (skin shows through)");
	transparentCheck->setChecked (*prefTransparent);
	topLayout->addWidget (transparentCheck);

	QDialogButtonBox* buttons = new QDialogButtonBox (
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	topLayout->addWidget (buttons);

	QObject::connect (buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect (buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec () == QDialog::Accepted)
	{
		int idx = skinCombo->currentIndex ();
		if (idx >= 0 && idx < (int) skinNames.size ())
			SkinSetSkinName (device, skinNames[idx]);

		{
			Preference<ScaleType> p (kPrefKeyScale);
			p = doubleCheck->isChecked () ? (ScaleType) 2 : (ScaleType) 1;
		}
		{
			Preference<bool> p (kPrefKeyDimWhenInactive);
			p = dimCheck->isChecked ();
		}
		{
			Preference<bool> p (kPrefKeyShowDebugMode);
			p = debugCheck->isChecked ();
		}
		{
			Preference<bool> p (kPrefKeyShowGremlinMode);
			p = gremlinCheck->isChecked ();
		}
		{
			Preference<bool> p (kPrefKeyStayOnTop);
			p = onTopCheck->isChecked ();
		}
		{
			Preference<bool> p (kPrefKeyFramelessWindow);
			p = framelessCheck->isChecked ();
		}
		{
			Preference<bool> p (kPrefKeyFeatheredEdges);
			p = featherCheck->isChecked ();
		}
		{
			Preference<bool> p (kPrefKeyTransparentLCD);
			p = transparentCheck->isChecked ();
		}

		// Re-apply skin at new scale — reloads skin image, recalculates
		// LCD rect, and resizes the window to match.
		if (gWindow)
			gWindow->WindowReset ();

		return kDlgItemOK;
	}

	return kDlgItemCancel;
}


// ---------------------------------------------------------------------------
//	PrvHostErrorHandling — Warning/Error behavior dropdowns
// ---------------------------------------------------------------------------

static EmDlgItemID PrvHostErrorHandling (void)
{
	Preference<EmErrorHandlingOption> prefWarnOff (kPrefKeyWarningOff);
	Preference<EmErrorHandlingOption> prefErrOff  (kPrefKeyErrorOff);
	Preference<EmErrorHandlingOption> prefWarnOn  (kPrefKeyWarningOn);
	Preference<EmErrorHandlingOption> prefErrOn   (kPrefKeyErrorOn);

	QDialog dlg;
	dlg.setWindowTitle ("Error Handling");

	QVBoxLayout* topLayout = new QVBoxLayout (&dlg);

	// Helper to build a combo for a given set of options
	auto makeCombo = [](const QStringList& items, int current) -> QComboBox* {
		QComboBox* combo = new QComboBox;
		combo->addItems (items);
		if (current >= 0 && current < items.size ())
			combo->setCurrentIndex (current);
		return combo;
	};

	auto optionToIndex2 = [](EmErrorHandlingOption opt) -> int {
		switch (opt) { case kShow: return 0; case kContinue: return 1; default: return 0; }
	};
	auto optionToIndex2q = [](EmErrorHandlingOption opt) -> int {
		switch (opt) { case kShow: return 0; case kQuit: return 1; default: return 0; }
	};
	auto optionToIndex3 = [](EmErrorHandlingOption opt) -> int {
		switch (opt) { case kShow: return 0; case kContinue: return 1; case kSwitch: return 2; default: return 0; }
	};
	auto optionToIndex3q = [](EmErrorHandlingOption opt) -> int {
		switch (opt) { case kShow: return 0; case kQuit: return 1; case kSwitch: return 2; default: return 0; }
	};

	// Gremlins Off
	QGroupBox* offGroup = new QGroupBox ("Gremlins Off");
	QFormLayout* offForm = new QFormLayout (offGroup);

	QComboBox* warnOffCombo = makeCombo ({"Show in Dialog", "Automatically Continue"},
										  optionToIndex2 (*prefWarnOff));
	offForm->addRow ("Warning:", warnOffCombo);

	QComboBox* errOffCombo = makeCombo ({"Show in Dialog", "Automatically Quit"},
										 optionToIndex2q (*prefErrOff));
	offForm->addRow ("Error:", errOffCombo);
	topLayout->addWidget (offGroup);

	// Gremlins On
	QGroupBox* onGroup = new QGroupBox ("Gremlins On");
	QFormLayout* onForm = new QFormLayout (onGroup);

	QComboBox* warnOnCombo = makeCombo ({"Show in Dialog", "Automatically Continue", "Next Gremlin"},
										 optionToIndex3 (*prefWarnOn));
	onForm->addRow ("Warning:", warnOnCombo);

	QComboBox* errOnCombo = makeCombo ({"Show in Dialog", "Automatically Quit", "Next Gremlin"},
										optionToIndex3q (*prefErrOn));
	onForm->addRow ("Error:", errOnCombo);
	topLayout->addWidget (onGroup);

	QDialogButtonBox* buttons = new QDialogButtonBox (
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	topLayout->addWidget (buttons);

	QObject::connect (buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect (buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec () == QDialog::Accepted)
	{
		// Gremlins Off — Warning
		static const EmErrorHandlingOption warnOffMap[] = { kShow, kContinue };
		{
			Preference<EmErrorHandlingOption> p (kPrefKeyWarningOff);
			p = warnOffMap[warnOffCombo->currentIndex ()];
		}

		// Gremlins Off — Error
		static const EmErrorHandlingOption errOffMap[] = { kShow, kQuit };
		{
			Preference<EmErrorHandlingOption> p (kPrefKeyErrorOff);
			p = errOffMap[errOffCombo->currentIndex ()];
		}

		// Gremlins On — Warning
		static const EmErrorHandlingOption warnOnMap[] = { kShow, kContinue, kSwitch };
		{
			Preference<EmErrorHandlingOption> p (kPrefKeyWarningOn);
			p = warnOnMap[warnOnCombo->currentIndex ()];
		}

		// Gremlins On — Error
		static const EmErrorHandlingOption errOnMap[] = { kShow, kQuit, kSwitch };
		{
			Preference<EmErrorHandlingOption> p (kPrefKeyErrorOn);
			p = errOnMap[errOnCombo->currentIndex ()];
		}

		return kDlgItemOK;
	}

	return kDlgItemCancel;
}


// ---------------------------------------------------------------------------
//	PrvHostLoggingOptions — 20 logging-category checkboxes with mode toggle
// ---------------------------------------------------------------------------

static EmDlgItemID PrvHostLoggingOptions (void)
{
	// Read current preferences
	struct LogEntry { const char* name; PrefKeyType key; uint8 value; };

	LogEntry entries[] = {
		#undef LENTRY
		#define LENTRY(name) { #name + 3, kPrefKey##name, 0 },
		FOR_EACH_LOG_PREF (LENTRY)
	};
	const int count = sizeof(entries) / sizeof(entries[0]);

	for (int i = 0; i < count; ++i)
	{
		Preference<uint8> pref (entries[i].key);
		entries[i].value = *pref;
	}

	// Build dialog
	QDialog dlg;
	dlg.setWindowTitle ("Logging Options");

	QVBoxLayout* topLayout = new QVBoxLayout (&dlg);

	// Mode radio buttons
	QHBoxLayout* modeLayout = new QHBoxLayout;
	QRadioButton* normalRadio  = new QRadioButton ("Normal");
	QRadioButton* gremlinRadio = new QRadioButton ("Gremlin");
	normalRadio->setChecked (true);
	modeLayout->addWidget (normalRadio);
	modeLayout->addWidget (gremlinRadio);
	topLayout->addLayout (modeLayout);

	// Checkboxes
	QGroupBox* group = new QGroupBox ("Logging Categories");
	QGridLayout* grid = new QGridLayout (group);

	QCheckBox* checks[count];
	for (int i = 0; i < count; ++i)
	{
		checks[i] = new QCheckBox (PrvInsertSpaces (entries[i].name));
		grid->addWidget (checks[i], i / 2, i % 2);
	}
	topLayout->addWidget (group);

	// Current active bitmask
	int activeBit = kNormalLogging;

	// Load checkboxes for the active bit
	auto loadChecks = [&]() {
		for (int i = 0; i < count; ++i)
			checks[i]->setChecked ((entries[i].value & activeBit) != 0);
	};

	// Save checkboxes to the active bit
	auto saveChecks = [&]() {
		for (int i = 0; i < count; ++i)
		{
			if (checks[i]->isChecked ())
				entries[i].value |= (uint8) activeBit;
			else
				entries[i].value &= ~(uint8) activeBit;
		}
	};

	loadChecks ();

	QObject::connect (normalRadio, &QRadioButton::toggled, [&](bool checked) {
		if (checked)
		{
			saveChecks ();
			activeBit = kNormalLogging;
			loadChecks ();
		}
	});

	QObject::connect (gremlinRadio, &QRadioButton::toggled, [&](bool checked) {
		if (checked)
		{
			saveChecks ();
			activeBit = kGremlinLogging;
			loadChecks ();
		}
	});

	QDialogButtonBox* buttons = new QDialogButtonBox (
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	topLayout->addWidget (buttons);

	QObject::connect (buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect (buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec () == QDialog::Accepted)
	{
		// Save final checkbox state
		saveChecks ();

		for (int i = 0; i < count; ++i)
		{
			Preference<uint8> pref (entries[i].key);
			pref = entries[i].value;
		}

		return kDlgItemOK;
	}

	return kDlgItemCancel;
}


// ---------------------------------------------------------------------------
//	PrvHostEditHostFS — Card slot filesystem mapping
// ---------------------------------------------------------------------------

static EmDlgItemID PrvHostEditHostFS (void)
{
	Preference<SlotInfoList> prefSlots (kPrefKeySlotList);
	SlotInfoList slotList = *prefSlots;

	// Ensure we have 8 slots
	while ((int) slotList.size () < 8)
	{
		SlotInfoType info;
		info.fSlotNumber  = (int32) slotList.size () + 1;
		info.fSlotOccupied = false;
		info.fSlotRoot     = EmDirRef ();
		slotList.push_back (info);
	}

	QDialog dlg;
	dlg.setWindowTitle ("Host FS Options");

	QVBoxLayout* topLayout = new QVBoxLayout (&dlg);

	QListWidget* listWidget = new QListWidget;
	topLayout->addWidget (listWidget);

	auto refreshList = [&]() {
		listWidget->clear ();
		for (int i = 0; i < (int) slotList.size (); ++i)
		{
			QString text = QString ("Slot %1: ").arg (slotList[i].fSlotNumber);
			if (slotList[i].fSlotOccupied)
				text += QString::fromStdString (slotList[i].fSlotRoot.GetFullPath ());
			else
				text += "(empty)";
			listWidget->addItem (text);
		}
	};

	refreshList ();

	QHBoxLayout* btnLayout = new QHBoxLayout;
	QPushButton* mountBtn   = new QPushButton ("Mount...");
	QPushButton* unmountBtn = new QPushButton ("Unmount");
	btnLayout->addWidget (mountBtn);
	btnLayout->addWidget (unmountBtn);
	topLayout->addLayout (btnLayout);

	QObject::connect (mountBtn, &QPushButton::clicked, [&]() {
		int row = listWidget->currentRow ();
		if (row < 0 || row >= (int) slotList.size ())
			return;
		QString dir = QFileDialog::getExistingDirectory (&dlg, "Choose Directory to Mount");
		if (!dir.isEmpty ())
		{
			slotList[row].fSlotOccupied = true;
			slotList[row].fSlotRoot = EmDirRef (dir.toStdString ());
			refreshList ();
			listWidget->setCurrentRow (row);
		}
	});

	QObject::connect (unmountBtn, &QPushButton::clicked, [&]() {
		int row = listWidget->currentRow ();
		if (row < 0 || row >= (int) slotList.size ())
			return;
		slotList[row].fSlotOccupied = false;
		slotList[row].fSlotRoot = EmDirRef ();
		refreshList ();
		listWidget->setCurrentRow (row);
	});

	QDialogButtonBox* buttons = new QDialogButtonBox (
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	topLayout->addWidget (buttons);

	QObject::connect (buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect (buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec () == QDialog::Accepted)
	{
		Preference<SlotInfoList> p (kPrefKeySlotList);
		p = slotList;
		return kDlgItemOK;
	}

	return kDlgItemCancel;
}


// ---------------------------------------------------------------------------
//	PrvHostHordeNew — native Qt Horde (Gremlin) New dialog
// ---------------------------------------------------------------------------

static EmDlgItemID PrvHostHordeNew (void)
{
	// Read saved preferences.

	Preference<HordeInfo> pref (kPrefKeyHordeInfo);
	HordeInfo info = *pref;

	// Get the list of applications.

	DatabaseInfoList appList;
	::GetDatabases (appList, kApplicationsOnly);

	QDialog dlg;
	dlg.setWindowTitle ("New Gremlin Horde");

	QVBoxLayout* topLayout = new QVBoxLayout (&dlg);

	// Application list (multi-select)
	topLayout->addWidget (new QLabel ("Applications:"));

	QListWidget* appListWidget = new QListWidget;
	appListWidget->setSelectionMode (QAbstractItemView::ExtendedSelection);

	for (DatabaseInfoList::size_type i = 0; i < appList.size (); ++i)
	{
		appListWidget->addItem (QString::fromUtf8 (appList[i].name));

		// Pre-select apps that were previously selected.
		DatabaseInfoList::iterator begin = info.fAppList.begin ();
		DatabaseInfoList::iterator end   = info.fAppList.end ();
		if (find (begin, end, appList[i]) != end)
			appListWidget->item ((int) i)->setSelected (true);
	}

	// If nothing is selected, select the first item.
	if (appListWidget->selectedItems ().empty () && appListWidget->count () > 0)
		appListWidget->item (0)->setSelected (true);

	topLayout->addWidget (appListWidget);

	// Gremlin number range
	QHBoxLayout* rangeLayout = new QHBoxLayout;
	rangeLayout->addWidget (new QLabel ("Start Gremlin #:"));
	QSpinBox* startSpin = new QSpinBox;
	startSpin->setRange (0, 999);
	startSpin->setValue (info.fStartNumber);
	rangeLayout->addWidget (startSpin);

	rangeLayout->addWidget (new QLabel ("Stop Gremlin #:"));
	QSpinBox* stopSpin = new QSpinBox;
	stopSpin->setRange (0, 999);
	stopSpin->setValue (info.fStopNumber);
	rangeLayout->addWidget (stopSpin);
	topLayout->addLayout (rangeLayout);

	// Limits group
	QGroupBox* limitsGroup = new QGroupBox ("Limits");
	QGridLayout* limitsGrid = new QGridLayout (limitsGroup);

	QCheckBox* switchCheck = new QCheckBox ("Switch after");
	switchCheck->setChecked (info.fCanSwitch);
	QSpinBox* switchSpin = new QSpinBox;
	switchSpin->setRange (1, 99999999);
	switchSpin->setValue (info.fDepthSwitch);
	switchSpin->setEnabled (info.fCanSwitch);
	limitsGrid->addWidget (switchCheck, 0, 0);
	limitsGrid->addWidget (switchSpin, 0, 1);
	limitsGrid->addWidget (new QLabel ("events"), 0, 2);

	QCheckBox* saveCheck = new QCheckBox ("Save after");
	saveCheck->setChecked (info.fCanSave);
	QSpinBox* saveSpin = new QSpinBox;
	saveSpin->setRange (1, 99999999);
	saveSpin->setValue (info.fDepthSave);
	saveSpin->setEnabled (info.fCanSave);
	limitsGrid->addWidget (saveCheck, 1, 0);
	limitsGrid->addWidget (saveSpin, 1, 1);
	limitsGrid->addWidget (new QLabel ("events"), 1, 2);

	QCheckBox* stopCheck = new QCheckBox ("Stop after");
	stopCheck->setChecked (info.fCanStop);
	QSpinBox* stopEvtSpin = new QSpinBox;
	stopEvtSpin->setRange (1, 99999999);
	stopEvtSpin->setValue (info.fDepthStop);
	stopEvtSpin->setEnabled (info.fCanStop);
	limitsGrid->addWidget (stopCheck, 2, 0);
	limitsGrid->addWidget (stopEvtSpin, 2, 1);
	limitsGrid->addWidget (new QLabel ("events"), 2, 2);

	topLayout->addWidget (limitsGroup);

	QObject::connect (switchCheck, &QCheckBox::toggled, switchSpin, &QSpinBox::setEnabled);
	QObject::connect (saveCheck, &QCheckBox::toggled, saveSpin, &QSpinBox::setEnabled);
	QObject::connect (stopCheck, &QCheckBox::toggled, stopEvtSpin, &QSpinBox::setEnabled);

	// First Launched App combo
	QHBoxLayout* firstAppLayout = new QHBoxLayout;
	firstAppLayout->addWidget (new QLabel ("First Launched App:"));
	QComboBox* firstAppCombo = new QComboBox;
	topLayout->addLayout (firstAppLayout);
	firstAppLayout->addWidget (firstAppCombo);

	// Populate first app combo from currently selected items.
	auto updateFirstAppCombo = [&]() {
		QString previousSelection = firstAppCombo->currentText ();
		firstAppCombo->clear ();

		QList<QListWidgetItem*> selected = appListWidget->selectedItems ();
		int restoreIndex = 0;
		for (int i = 0; i < selected.size (); ++i)
		{
			firstAppCombo->addItem (selected[i]->text ());
			if (selected[i]->text () == previousSelection)
				restoreIndex = i;
		}
		if (firstAppCombo->count () > 0)
			firstAppCombo->setCurrentIndex (restoreIndex);
	};

	updateFirstAppCombo ();

	// Set initial selection from saved preference.
	if (!info.fFirstLaunchedAppName.empty ())
	{
		int idx = firstAppCombo->findText (
			QString::fromStdString (info.fFirstLaunchedAppName));
		if (idx >= 0)
			firstAppCombo->setCurrentIndex (idx);
	}

	QObject::connect (appListWidget, &QListWidget::itemSelectionChanged,
		[&]() { updateFirstAppCombo (); });

	// OK/Cancel
	QDialogButtonBox* buttons = new QDialogButtonBox (
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	topLayout->addWidget (buttons);

	QObject::connect (buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect (buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec () != QDialog::Accepted)
		return kDlgItemCancel;

	// Gather results from dialog.

	info.fStartNumber  = startSpin->value ();
	info.fStopNumber   = stopSpin->value ();
	info.fCanSwitch    = switchCheck->isChecked ();
	info.fDepthSwitch  = switchSpin->value ();
	info.fCanSave      = saveCheck->isChecked ();
	info.fDepthSave    = saveSpin->value ();
	info.fCanStop      = stopCheck->isChecked ();
	info.fDepthStop    = stopEvtSpin->value ();

	// Determine which app is launched first.

	string firstAppName;
	if (firstAppCombo->currentIndex () >= 0)
		firstAppName = firstAppCombo->currentText ().toStdString ();

	info.fFirstLaunchedAppName = firstAppName;

	// Build the app list — first launched app goes first, then the rest.

	info.fAppList.clear ();

	// Find and insert the first-launched app.
	for (DatabaseInfoList::size_type i = 0; i < appList.size (); ++i)
	{
		if (string (appList[i].name) == firstAppName)
		{
			info.fAppList.push_back (appList[i]);
			break;
		}
	}

	// Add remaining selected apps (skip the first-launched one).
	QList<QListWidgetItem*> selected = appListWidget->selectedItems ();
	for (int i = 0; i < selected.size (); ++i)
	{
		int index = appListWidget->row (selected[i]);
		if (index >= 0 && index < (int) appList.size ())
		{
			if (string (appList[index].name) != firstAppName)
				info.fAppList.push_back (appList[index]);
		}
	}

	// Transfer new fields to old and save preferences.

	info.NewToOld ();

	{
		Preference<HordeInfo> p (kPrefKeyHordeInfo);
		p = info;
	}

	// Start the gremlins.

	Hordes::New (info);

	return kDlgItemOK;
}


// ---------------------------------------------------------------------------
//	PrvHostDatabaseExport — native Qt database export dialog
// ---------------------------------------------------------------------------
// Bypasses the cross-platform PrvDatabaseExport callback (which needs
// GetSelectedItems/AppendToList stubs) and handles the full flow natively.

static EmDlgItemID PrvHostDatabaseExport (void)
{
	// Get the list of installed databases.

	DatabaseInfoList dbList;
	::GetDatabases (dbList, kAllDatabases);

	if (dbList.empty ())
	{
		QMessageBox::information (nullptr, "Export Database",
			"No databases are installed.");
		return kDlgItemCancel;
	}

	QDialog dlg;
	dlg.setWindowTitle ("Export Database");

	QVBoxLayout* topLayout = new QVBoxLayout (&dlg);

	topLayout->addWidget (new QLabel ("Select databases to export:"));

	QListWidget* listWidget = new QListWidget;
	listWidget->setSelectionMode (QAbstractItemView::ExtendedSelection);

	for (DatabaseInfoList::size_type i = 0; i < dbList.size (); ++i)
	{
		string itemText (dbList[i].dbName);

		if (strcmp (dbList[i].dbName, dbList[i].name) != 0)
		{
			itemText += " (";
			itemText += dbList[i].name;
			itemText += ")";
		}

		listWidget->addItem (QString::fromStdString (itemText));
	}
	topLayout->addWidget (listWidget);

	QDialogButtonBox* buttons = new QDialogButtonBox (
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	topLayout->addWidget (buttons);

	QObject::connect (buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect (buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec () != QDialog::Accepted)
		return kDlgItemCancel;

	// Export each selected database.

	QList<QListWidgetItem*> selected = listWidget->selectedItems ();
	if (selected.empty ())
		return kDlgItemCancel;

	for (int i = 0; i < selected.size (); ++i)
	{
		int index = listWidget->row (selected[i]);
		const DatabaseInfo& db = dbList[index];

		// Get database attributes to determine file type.

		UInt16 dbAttributes = 0;
		UInt32 dbType = 0;
		UInt32 dbCreator = 0;
		ErrCode err = ::DmDatabaseInfo (db.cardNo, db.dbID, NULL,
			&dbAttributes, NULL, NULL,
			NULL, NULL, NULL, NULL,
			NULL, &dbType, &dbCreator);
		if (err)
			continue;

		EmFileType type = kFileTypeNone;

		if ((dbAttributes & dmHdrAttrResDB) != 0)
			type = kFileTypePalmApp;
		else if (dbCreator == sysFileCClipper)
			type = kFileTypePalmQA;
		else
			type = kFileTypePalmDB;

		// Build default filename with appropriate extension.

		string defaultName (db.dbName);
		string extension = type == kFileTypePalmApp ? ".prc" :
						   type == kFileTypePalmQA  ? ".pqa" : ".pdb";

		if (!::EndsWith (defaultName.c_str (), extension.c_str ()))
			defaultName += extension;

		// Show save dialog.

		EmFileRef		result;
		string			prompt ("Save as...");
		EmDirRef		defaultPath;
		EmFileTypeList	filterList;

		filterList.push_back (type);
		filterList.push_back (kFileTypePalmAll);
		filterList.push_back (kFileTypeAll);

		EmDlgItemID item = EmDlg::DoPutFile (result, prompt,
			defaultPath, filterList, defaultName);

		if (item != kDlgItemOK)
			break;		// User cancelled — stop exporting remaining files.

		EmStreamFile stream (result, kCreateOrEraseForUpdate,
			kFileCreatorInstaller, type);
		::SavePalmFile (stream, db.cardNo, db.dbName);
	}

	return kDlgItemOK;
}


// ---------------------------------------------------------------------------
//	DoManualSpeed — numerator/denominator speed fraction dialog
// ---------------------------------------------------------------------------

EmDlgItemID EmDlg::DoManualSpeed (int& numerator, int& denominator)
{
	QDialog dlg;
	dlg.setWindowTitle ("Manual Speed");

	QVBoxLayout* topLayout = new QVBoxLayout (&dlg);

	topLayout->addWidget (new QLabel (
		"Enter speed as a fraction of real-time:"));

	QHBoxLayout* fracLayout = new QHBoxLayout;

	QSpinBox* numSpin = new QSpinBox;
	numSpin->setRange (1, 9999);
	numSpin->setValue (numerator);

	QSpinBox* denSpin = new QSpinBox;
	denSpin->setRange (1, 9999);
	denSpin->setValue (denominator);

	fracLayout->addWidget (numSpin);
	fracLayout->addWidget (new QLabel ("/"));
	fracLayout->addWidget (denSpin);
	fracLayout->addWidget (new QLabel ("x"));

	topLayout->addLayout (fracLayout);

	QDialogButtonBox* buttons = new QDialogButtonBox (
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

	topLayout->addWidget (buttons);

	QObject::connect (buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect (buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec () == QDialog::Accepted)
	{
		numerator = numSpin->value ();
		denominator = denSpin->value ();
		return kDlgItemOK;
	}

	return kDlgItemCancel;
}


// ---------------------------------------------------------------------------
//	HostRunDialog — dispatch to the appropriate Qt dialog
// ---------------------------------------------------------------------------

EmDlgItemID EmDlg::HostRunDialog (const void* parameters)
{
	const RunDialogParameters* params =
		static_cast<const RunDialogParameters*> (parameters);
	if (!params)
		return kDlgItemNone;

	switch (params->fDlgID)
	{
		case kDlgSessionNew:
			return PrvHostSessionNew (params->fUserData);

		case kDlgDatabaseImport:
			return PrvHostDatabaseImport (params->fFn, params->fUserData);

		case kDlgReset:
			return PrvHostReset (params->fFn, params->fUserData);

		case kDlgCommonDialog:
			return PrvHostCommonDialog (params->fFn, params->fUserData);

		case kDlgSessionInfo:
			return PrvHostSessionInfo ();

		case kDlgEditDebugging:
			return PrvHostDebuggingOptions ();

		case kDlgEditPreferences:
		case kDlgEditPreferencesFullyBound:
			return PrvHostPreferences ();

		case kDlgEditSkins:
			return PrvHostEditSkins ();

		case kDlgEditErrorHandling:
			return PrvHostErrorHandling ();

		case kDlgEditLogging:
			return PrvHostLoggingOptions ();

		case kDlgEditHostFS:
			return PrvHostEditHostFS ();

		case kDlgDatabaseExport:
			return PrvHostDatabaseExport ();

		case kDlgHordeNew:
			return PrvHostHordeNew ();

		case kDlgROMTransferQuery:
			QMessageBox::information (nullptr, "Transfer ROM",
				"ROM Transfer requires a physical Palm device connected "
				"via serial port. This feature is not available.");
			return kDlgItemCancel;

		default:
			return kDlgItemNone;
	}
}

EmDlgRef EmDlg::HostDialogOpen (EmDlgFn fn, void* userData, EmDlgID dlgID)
{
	if (dlgID == kDlgGremlinControl)
	{
		if (sGremlinControlDlg)
		{
			sGremlinControlDlg->raise ();
			sGremlinControlDlg->activateWindow ();
			return (EmDlgRef) sGremlinControlDlg;
		}

		QDialog* dlg = new QDialog (nullptr, Qt::Window);
		dlg->setWindowTitle ("Gremlin Control");
		dlg->setAttribute (Qt::WA_DeleteOnClose);

		QVBoxLayout* topLayout = new QVBoxLayout (dlg);

		// Status labels
		QLabel* gremlinLabel = new QLabel ("Gremlin #: --");
		QLabel* eventLabel   = new QLabel ("Event #: --");
		QLabel* elapsedLabel = new QLabel ("Elapsed: --:--:--");

		gremlinLabel->setFont (QFont ("monospace", 10));
		eventLabel->setFont (QFont ("monospace", 10));
		elapsedLabel->setFont (QFont ("monospace", 10));

		topLayout->addWidget (gremlinLabel);
		topLayout->addWidget (eventLabel);
		topLayout->addWidget (elapsedLabel);

		// Buttons
		QHBoxLayout* btnLayout = new QHBoxLayout;
		QPushButton* stopBtn   = new QPushButton ("Stop");
		QPushButton* resumeBtn = new QPushButton ("Resume");
		QPushButton* stepBtn   = new QPushButton ("Step");
		btnLayout->addWidget (stopBtn);
		btnLayout->addWidget (resumeBtn);
		btnLayout->addWidget (stepBtn);
		topLayout->addLayout (btnLayout);

		// Update function — called periodically by QTimer
		auto updateLabels = [gremlinLabel, eventLabel, elapsedLabel,
							 stopBtn, resumeBtn, stepBtn]() {
			int32 number  = Hordes::GremlinNumber ();
			int32 counter = Hordes::EventCounter ();
			int32 limit   = Hordes::EventLimit ();
			uint32 elapsed = Hordes::ElapsedMilliseconds ();

			gremlinLabel->setText (QString ("Gremlin #: %1").arg (number));

			if (limit > 0)
				eventLabel->setText (QString ("Event #: %1 of %2").arg (counter).arg (limit));
			else
				eventLabel->setText (QString ("Event #: %1").arg (counter));

			// Format elapsed time as HH:MM:SS
			uint32 secs = elapsed / 1000;
			uint32 mins = secs / 60;
			uint32 hrs  = mins / 60;
			elapsedLabel->setText (QString ("Elapsed: %1:%2:%3")
				.arg (hrs, 2, 10, QChar ('0'))
				.arg (mins % 60, 2, 10, QChar ('0'))
				.arg (secs % 60, 2, 10, QChar ('0')));

			stopBtn->setEnabled (Hordes::CanStop ());
			resumeBtn->setEnabled (Hordes::CanResume ());
			stepBtn->setEnabled (Hordes::CanStep ());
		};

		// Initial update
		updateLabels ();

		// Timer for periodic updates
		QTimer* timer = new QTimer (dlg);
		QObject::connect (timer, &QTimer::timeout, updateLabels);
		timer->start (500);

		// Button handlers
		QObject::connect (stopBtn, &QPushButton::clicked, [updateLabels]() {
			EmSessionStopper stopper (gSession, kStopNow);
			if (stopper.Stopped ())
				Hordes::Stop ();
			updateLabels ();
		});

		QObject::connect (resumeBtn, &QPushButton::clicked, [updateLabels]() {
			EmSessionStopper stopper (gSession, kStopOnSysCall);
			if (stopper.Stopped ())
				Hordes::Resume ();
			updateLabels ();
		});

		QObject::connect (stepBtn, &QPushButton::clicked, [updateLabels]() {
			EmSessionStopper stopper (gSession, kStopOnSysCall);
			if (stopper.Stopped ())
				Hordes::Step ();
			updateLabels ();
		});

		// On close: clean up static pointer
		QObject::connect (dlg, &QDialog::destroyed, []() {
			sGremlinControlDlg = nullptr;
		});

		sGremlinControlDlg = dlg;

		// Restore saved position
		Preference<PointType> posPref (kPrefKeyGCWLocation);
		if (posPref.Loaded ())
			dlg->move ((*posPref).x, (*posPref).y);

		dlg->show ();
		return (EmDlgRef) dlg;
	}

	return nullptr;
}

void EmDlg::HostDialogClose (EmDlgRef dlgRef)
{
	if (sGremlinControlDlg && (EmDlgRef) sGremlinControlDlg == dlgRef)
	{
		// Save window position before closing.
		PointType pt;
		pt.x = (int16) sGremlinControlDlg->x ();
		pt.y = (int16) sGremlinControlDlg->y ();

		Preference<PointType> posPref (kPrefKeyGCWLocation);
		posPref = pt;

		sGremlinControlDlg->close ();
		// WA_DeleteOnClose + destroyed signal will clear sGremlinControlDlg
	}
}

void EmDlg::HostStartIdling (EmDlgContext& context)
{
}

EmRect EmDlg::GetDlgBounds (EmDlgRef dlg)
{
	return EmRect (0, 0, 0, 0);
}

void EmDlg::SetDlgBounds (EmDlgRef dlg, const EmRect& bounds)
{
}

void EmDlg::SetItemText (EmDlgRef dlg, EmDlgItemID itemID, string text)
{
}

void EmDlg::SetItemMin (EmDlgRef dlg, EmDlgItemID itemID, long min)
{
}

void EmDlg::SetItemMax (EmDlgRef dlg, EmDlgItemID itemID, long max)
{
}

void EmDlg::SetItemValue (EmDlgRef dlg, EmDlgItemID itemID, long value)
{
}

void EmDlg::EnableItem (EmDlgRef dlg, EmDlgItemID itemID)
{
}

void EmDlg::DisableItem (EmDlgRef dlg, EmDlgItemID itemID)
{
}

void EmDlg::ShowItem (EmDlgRef dlg, EmDlgItemID itemID)
{
}

void EmDlg::HideItem (EmDlgRef dlg, EmDlgItemID itemID)
{
}

void EmDlg::AppendToMenu (EmDlgRef dlg, EmDlgItemID itemID, const StringList& items)
{
}

void EmDlg::AppendToList (EmDlgRef dlg, EmDlgItemID itemID, const StringList& items)
{
}

void EmDlg::SelectListItems (EmDlgRef dlg, EmDlgItemID itemID, const EmDlgListIndexList& indices)
{
}

void EmDlg::UnselectListItems (EmDlgRef dlg, EmDlgItemID itemID, const EmDlgListIndexList& indices)
{
}

void EmDlg::GetSelectedItems (EmDlgRef dlg, EmDlgItemID itemID, EmDlgListIndexList& indices)
{
	indices.clear ();
}

void EmDlg::ClearMenu (EmDlgRef dlg, EmDlgItemID itemID)
{
}

void EmDlg::DisableMenuItem (EmDlgRef dlg, EmDlgItemID itemID, long index)
{
}

void EmDlg::ClearList (EmDlgRef dlg, EmDlgItemID itemID)
{
}

long EmDlg::GetItemValue (EmDlgRef dlg, EmDlgItemID itemID)
{
	return 0;
}

string EmDlg::GetItemText (EmDlgRef dlg, EmDlgItemID itemID)
{
	return string ();
}

void EmDlg::SetDlgDefaultButton (EmDlgContext& context, EmDlgItemID itemID)
{
}

void EmDlg::SetDlgCancelButton (EmDlgContext& context, EmDlgItemID itemID)
{
}

EmRect EmDlg::GetItemBounds (EmDlgRef dlg, EmDlgItemID itemID)
{
	return EmRect (0, 0, 0, 0);
}

int EmDlg::GetTextHeight (EmDlgRef dlg, EmDlgItemID itemID, const string& text)
{
	return 12;
}

void EmDlg::CenterDlg (EmDlgRef dlg)
{
}


// ---------------------------------------------------------------------------
//	File dialogs
// ---------------------------------------------------------------------------

EmDlgItemID EmDlg::HostRunGetFile (const void* parameters)
{
	const DoGetFileParameters* params = static_cast<const DoGetFileParameters*> (parameters);

	QString filter = "All Files (*)";
	if (!params->fFilterList.empty ())
	{
		filter = GetFileFilter (params->fFilterList[0]);
	}

	QString defaultPath;
	if (params->fDefaultPath.IsSpecified ())
	{
		defaultPath = QString::fromStdString (params->fDefaultPath.GetFullPath ());
	}

	QString fileName = QFileDialog::getOpenFileName (
		nullptr,
		QString::fromStdString (params->fPrompt),
		defaultPath,
		filter
	);

	if (!fileName.isEmpty ())
	{
		params->fResult = EmFileRef (fileName.toStdString ());
		return kDlgItemOK;
	}

	return kDlgItemCancel;
}

EmDlgItemID EmDlg::HostRunGetFileList (const void* parameters)
{
	const DoGetFileListParameters* params = static_cast<const DoGetFileListParameters*> (parameters);

	QString filter = "All Files (*)";
	if (!params->fFilterList.empty ())
	{
		filter = GetFileFilter (params->fFilterList[0]);
	}

	QString defaultPath;
	if (params->fDefaultPath.IsSpecified ())
	{
		defaultPath = QString::fromStdString (params->fDefaultPath.GetFullPath ());
	}

	QStringList fileNames = QFileDialog::getOpenFileNames (
		nullptr,
		QString::fromStdString (params->fPrompt),
		defaultPath,
		filter
	);

	if (!fileNames.isEmpty ())
	{
		params->fResults.clear ();
		for (const QString& fileName : fileNames)
		{
			params->fResults.push_back (EmFileRef (fileName.toStdString ()));
		}
		return kDlgItemOK;
	}

	return kDlgItemCancel;
}

EmDlgItemID EmDlg::HostRunPutFile (const void* parameters)
{
	const DoPutFileParameters* params = static_cast<const DoPutFileParameters*> (parameters);

	QString filter = "All Files (*)";
	if (!params->fFilterList.empty ())
	{
		filter = GetFileFilter (params->fFilterList[0]);
	}

	QString defaultPath;
	if (params->fDefaultPath.IsSpecified ())
	{
		defaultPath = QString::fromStdString (params->fDefaultPath.GetFullPath ());
	}
	if (!params->fDefaultName.empty ())
	{
		if (!defaultPath.isEmpty () && !defaultPath.endsWith ('/'))
			defaultPath += '/';
		defaultPath += QString::fromStdString (params->fDefaultName);
	}

	QString fileName = QFileDialog::getSaveFileName (
		nullptr,
		QString::fromStdString (params->fPrompt),
		defaultPath,
		filter
	);

	if (!fileName.isEmpty ())
	{
		params->fResult = EmFileRef (fileName.toStdString ());
		return kDlgItemOK;
	}

	return kDlgItemCancel;
}

EmDlgItemID EmDlg::HostRunGetDirectory (const void* parameters)
{
	const DoGetDirectoryParameters* params = static_cast<const DoGetDirectoryParameters*> (parameters);

	QString defaultPath;
	if (params->fDefaultPath.IsSpecified ())
	{
		defaultPath = QString::fromStdString (params->fDefaultPath.GetFullPath ());
	}

	QString dirName = QFileDialog::getExistingDirectory (
		nullptr,
		QString::fromStdString (params->fPrompt),
		defaultPath
	);

	if (!dirName.isEmpty ())
	{
		params->fResult = EmDirRef (dirName.toStdString ());
		return kDlgItemOK;
	}

	return kDlgItemCancel;
}

EmDlgItemID EmDlg::HostRunAboutBox (const void* parameters)
{
	QMessageBox aboutBox;
	aboutBox.setWindowTitle ("About POSE64");
	aboutBox.setTextFormat (Qt::RichText);
	QString version = QApplication::applicationVersion ();
	aboutBox.setText (
		"<b>POSE64</b> - Palm OS Emulator<br>"
		"Version " + version + "<br><br>"
		"64-bit Qt6 Port<br>"
		"Based on Palm OS Emulator 3.5<br>"
		"Copyright &copy; 1999-2001 Palm, Inc.<br><br>"
		"<a href=\"https://github.com/clintonthegeek/pose64\">"
		"https://github.com/clintonthegeek/pose64</a>"
	);
	aboutBox.exec ();

	return kDlgItemOK;
}

EmDlgItemID EmDlg::HostRunSessionSave (const void* parameters)
{
	const DoSessionSaveParameters* params =
		static_cast<const DoSessionSaveParameters*> (parameters);

	QString message = QString ("Save changes to \"%1\"?")
		.arg (QString::fromStdString (params->fDocName));

	QMessageBox::StandardButton result = QMessageBox::question (
		nullptr,
		QString::fromStdString (params->fAppName),
		message,
		QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

	switch (result)
	{
		case QMessageBox::Yes:    return kDlgItemYes;
		case QMessageBox::No:     return kDlgItemNo;
		default:                  return kDlgItemCancel;
	}
}
