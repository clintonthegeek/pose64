========================================================================
Skin information for the Palm OS Emulator
Copyright (c) 2000-2002 Palm, Inc. or its subsidiaries.
All rights reserved.

Please send bug reports, comments, suggestions, etc. to
<mailto:bug.reports@corp.palm.com>
========================================================================

Version History
---------------
v1.9 - 6/27/02
	Added Palm m130, m515, and i705 skins.

v1.8 - 6/10/01
	Added HandEra 330 skins.  Removed Handspring skins.  In order to ensure
	that the Handspring skins on the Palm site and those on the Handspring
	site don't get out of sync, the Handspring site will now be the official
	source for their skins.  You can download them from:

		<http://www.handspring.com/developers/VisorSkins_3.1H1.zip>

	If that link gets out-of-date (because they've released a new version
	with a new version number), you should be able to go to the Web page:

		<http://www.handspring.com/developers/tech_pose.jhtml>

	and find them there.

v1.7 - 5/29/01
	Added Palm m500 & m505 skins.

v1.6 - 2/9/01
	Added Visor Prism and Visor Platinum skins from Handsrping.  Moved
	licensee skins into their own directories.  Updated comments on how to
	install skin files to reflect rules implemented in Palm OS Emulator
	3.0a9.  Added links to 3rd party skin sites.

v1.5 - 12/10/00
	Added TRGpro skins.

v1.4 - 8/9/00
	Added Palm VIIx skins.  Moved the LCD area in the m100 skin down a
	little bit so that the entire 220 vertical range of the touchscreen
	reaches to the keyboard silkscreen buttons.

v1.3 - 8/7/00
	Added m100 skins.  Rolled in latest skins from Handspring.  Added more
	explanatory text and clarifications to this file.

v1.2 - 4/28/00
	Added Visor Japanese skins.  Changed Visor skin names to more closely
	match those used by the Palm skins.

v1.1 - 4/17/00
	Changed names from "Standard - English", etc., to "Standard-English",
	etc.  The spaces were removed to make specifying them on command lines
	easier.

v1.0 - 3/6/00
	Initial release. Includes skins for Pilot, PalmPilot, Palm III, Palm
	IIIc, Palm IIIe, Palm IIIx (English and Japanese), Palm V (English and
	Japanese), Palm Vx, Palm VII, Palm VIIEZ, Symbol 1700, and Visor (Blue
	and Graphite).


About Skins
-----------
A "skin" is something that defines the appearance of an application.  For
the Palm OS Emulator, skins are images of the devices that the Emulator
supports and emulates.


Installing Skins
----------------
To use the skins with the Palm OS Emulator, simply put them in a directory
called "Skins".  If running on Mac or Windows, the "Skins" directory (or an
alias or shortcut to it) needs to be in the same directory as the emulator
itself.  On Unix, the "Skins" directory (or a link to it) needs to be in the
$POSER_DIR directory (or the $HOME directory if $POSER_DIR is not defined),
or in "/usr/local/share/pose/" or "/usr/share/pose/".  Note that the name of
the directory on Unix (or the link to it) can actually be either "Skins" or
"skins".

When starting up, the Palm OS Emulator looks for the "Skins" directory in
the locations described above.  If it finds one, it recursively looks for
all files ending in ".skin".  As described below, it uses the information in
these ".skin" files (which are just text files) to get the corresponding
graphics files and display them.

You can shield a directory from being iterated over by surrounding its name
in parentheses (e.g., "(SkipMe)").  The contents of any such directories are
ignored.

Because the "Skins" directory is searched recursively, you can be flexible
with how you arrange its contents.  For instance, the Skins archive from
Palm looks as follows:

	Skins v1.x
		Handspring
			Visor_Blue.skin
			Visor_Blue_16.jpg
			Visor_Blue_16_Japanese.jpg
			...
		Palm
			m100.skin
			m100_16.jpg
			m100_32.jpg
			...
		Symbol
			Symbol_1500.skin
			Symbol_1500_16.jpg
			Symbol_1500_32.jpg
			...
		TRG
			TRGpro.skin
			TRGpro_16.jpg
			TRGpro_32.jpg

You can merely rename "Skins v1.x" to "Skins", or you could copy the
contents of the "Skins v1.x" directory into the "Skins" directory, or you
could put the "Skins v1.x" directory itself into the "Skins" directory.  Any
of those methods works fine.


More Skins
----------
There are a number of sites on the Web containing additional skins for the
Palm OS Emulator.  Here are the ones we know about:

<http://www.skinz.org>

	Contains 16 or so .zip files containing .bmp images.  One of them
	(ThugLife) appears to be corrupted, but it can be downloaded from the
	author's site.  These .bmp files need to be converted to .jpg files and
	have .skin files created for them.  Two of the skins (Visor-related)
	are already in .jpg/.skin format.

<http://www.desktopian.org/palmskins.html>

	A few of these are also on the skinz.org site, but there are some new
	ones as well. These are also .zips of .bmp files, and so have to be
	converted to .jpg files and have .skin files created for them.

<http://home.att.net/~foursquaredev/>

	Appears to be the home for the "Steel Case" and "VisiPalm" skins from
	www.skinz.org. "VisiPalm" is also on the Desktopian site.  However, the
	skin archive on this site comes with .jpg and .skin files.

<http://www.the-labs.com/PDA/>

	Includes some skins in .xpm format.  These .bmp files need to be
	converted to .jpg files and have .skin files created for them.  I
	haven't looked at these so I don't know if they are just different
	versions of skins that can be found elsewhere.  But the names are
	different, so probably not.

<http://www.praxxis.net/ThugLife/themesandskinz.htm>

	Home of the "Thug Life" skin.  Needs to be converted from .bmp format.


Creating Skins
--------------
Skins are defined by a pair of files: an image file and a .skin file that
describes the image file.

The image file is just a plain old graphic.  Currently, only JPEG format is
supported.  Support for more formats may be added in the future (hint: this
is a good area for a 3rd party contribution...).

The associated .skin file is a text file that describes the image.  The text
file is made up of a series of lines, each line defining an attribute of the
image.

Each definition is of the form:

	<attribute>=<value>

This is similar to the way .ini files are stored on Windows, and how the
emulator saves its own preferences.  In .skin files:

*	The attribute is case-senstive.  Thus, "Name" and "name" are not
	equivalent.

*	There can be only one definition of each attribute.  For instance,
	if the skin can be used with multiple devices, do NOT say:

		Devices = Pilot1000
		Devices = Pilot5000

	Instead, say:

		Devices = Pilot1000, Pilot5000

*	White space is optional, both around the equal sign and in the
	specification of the value.  Thus, "color=1,2,3" is the same as
	"color = 1, 2, 3".

The file can include comments, which are ignored when the file is parsed.
Comments appear on their own lines, and start with "#" or ';'.

Invalid files are detected and silently ignored.  There is currently no
error reporting when invalid values are encountered.  Your only indication
that something is wrong is that your skin won't show up in the Skins menu or
dialog (hint: this is a good area for a 3rd party contribution...).

Following is a definition of the attributes and how their values are
specified:


Name
----
	This is the name of the skin.  It's what appears in the Skin menu in the
	New Configuration dialog and in the Skin preferences dialog.

	Example:

		Name = Keith's Cool Skin


File1x
File2x
------
	These are the names of the image files.  Two image files must be
	specified: one used for single-scale and one used for double-scale.

	Image files are expected to be in the same directory as the .skin file,
	though a relative path may be specified.

	Currently, both image files must exist and be specified.  In the future,
	it may be possible to specify just one and have it scaled (hint: this is
	a good area for a 3rd party contribution...).

	Examples:

		File1x = MySkin1.jpg
		File2x = MySkin2.jpg

		# Macintosh relative-path format
		File1x = :Small Images:MySkin.jpg
		File2x = :Large Images:MySkin.jpg

		# Windows relative-path format
		File1x = Small Images\MySkin.jpg
		File2x = Large Images\MySkin.jpg


BackgroundColor
HighlightColor
---------------
	These fields define the color used when displaying the LCD area of the
	emulator's display.  BackgroundColor specifies the normal background
	color; Highlightcolor specifies the color used when backlighting is
	turned on (that is, the user holds down the power button).

	The value is specified as an r,g,b tuple.  The three components are
	provided as hexadecimal or decimal values in the range 0..255, seperated
	by commas.

	Examples:

		BackgroundColor = 0x7B, 0x8C, 0x5A
		HighlightColor	= 132, 240, 220


Devices
-------
	Provides the list of devices with which this skin can be used.  One or
	more devices can be provided, seperated by commas.  The current list of
	devices is:

		Pilot, Pilot1000, Pilot5000
		PalmPilot, PalmPilotPersonal, PalmPilotProfessional
		PalmIII, PalmIIIc, PalmIIIe, PalmIIIx
		PalmV, PalmVx
		PalmVII, PalmVIIEZ, Palm VIIx
		m100
		Symbol1700
		TRGpro
		Visor, VisorPrism, VisorPlatinum

	Examples:

		Devices = Pilot1000, Pilot5000
		Devices = PalmIIIc


Element#
--------
	A class of attributes that describes the layout of the image.  There is
	one attribute for each item in the image that can be clicked on.  There
	are also attributes for the LCD and touchscreen areas.

	The value for each attribute is a list of 5 items: the name of the
	element and its coordinates on the screen.  The current set of valid
	element names is:

		PowerButton
		UpButton
		DownButton
		App1Button
		App2Button
		App3Button
		App4Button
		CradleButton
		Antenna
		ContrastButton

			# Symbol-specific
		TriggerLeft
		TriggerCenter
		TriggerRight
		UpButtonLeft
		UpButtonRight
		DownButtonLeft
		DownButtonRight

		Touchscreen
		LCD

	All elements except for the last two are optional.

	The coordinates of each element are provided by specifying the left
	coordinate, the top coordinate, the element width, and the element
	height.  Only single-scale coordinates can be provided; double-scale
	coordinates are derived from these.  Coordinate values can be specified
	in hexadecimal or decimal.

	Each attribute name must start with the text "Element", and must be
	suffixed with characters that make it unique from all the other
	Element-related attributes.

	Example:

		#									  x    y    w    h
		#								   ---- ---- ---- ----
		Element1		= PowerButton,		 10, 295,  16,  24
		Element2		= UpButton,			110, 292,  20,  21
		Element3		= DownButton,		110, 313,  20,  21
		Element4		= App1Button,		 37, 295,  23,  25
		Element5		= App2Button,		 76, 297,  23,  25
		Element6		= App3Button,		141, 297,  23,  25
		Element7		= App4Button,		180, 294,  23,  25
		Element8		= Touchscreen,		 39,  44, 160, 220
		Element9		= LCD,				 39,  44, 160, 160
