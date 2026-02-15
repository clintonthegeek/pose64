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
#include "EmDevice.h"
#include "EmFileRef.h"
#include "EmStructs.h"			// Configuration
#include "Miscellaneous.h"		// MemoryTextList, GetMemoryTextList
#include "PreferenceMgr.h"		// gEmuPrefs

// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear

#include <QFileDialog>
#include <QMessageBox>
#include <QString>
#include <QApplication>
#include <QDialog>
#include <QComboBox>
#include <QPushButton>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QDialogButtonBox>

using namespace std;


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

	auto refreshFromROM = [&]() {
		currentDevices = FilterDevicesForROM (cfg.fROMFile);
		int devIdx = PopulateDeviceCombo (deviceCombo, currentDevices, cfg.fDevice);
		if (!currentDevices.empty ())
			cfg.fDevice = currentDevices[devIdx];

		currentSizes = FilterMemorySizes (cfg.fDevice);
		int ramIdx = PopulateRAMCombo (ramCombo, currentSizes, cfg.fRAMSize);
		if (!currentSizes.empty ())
			cfg.fRAMSize = currentSizes[ramIdx].first;

		okButton->setEnabled (cfg.fROMFile.IsSpecified ());
	};

	auto refreshFromDevice = [&]() {
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
		*(data.cfg) = cfg;
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
		default:
			return kDlgItemNone;
	}
}

EmDlgRef EmDlg::HostDialogOpen (EmDlgFn fn, void* userData, EmDlgID dlgID)
{
	return nullptr;
}

void EmDlg::HostDialogClose (EmDlgRef dlg)
{
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
	QMessageBox::about (
		nullptr,
		"QtPOSE",
		"QtPOSE v2 - Palm OS Emulator\n"
		"Qt6 Port\n\n"
		"Based on Palm OS Emulator 3.5\n"
		"Copyright (c) 1999-2001 Palm, Inc."
	);

	return kDlgItemOK;
}

EmDlgItemID EmDlg::HostRunSessionSave (const void* parameters)
{
	// Session save is handled by HostRunPutFile
	return kDlgItemCancel;
}
