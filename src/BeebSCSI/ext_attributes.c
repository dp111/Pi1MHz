
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "debug.h"
#include "fatfs/ff.h"
#include "filesystem.h"
#include "scsi.h"
#include "ext_attributes.h"


#define MAX_PREFS_TOKEN_LEN 25
#define MAX_PREFS_VALUE_LEN 482
#define MAX_PREFS_LINE_LEN (MAX_PREFS_TOKEN_LEN + MAX_PREFS_VALUE_LEN)



// Token written to start of ext attributes files
#define PREFS_TOKEN "# Drive Extended attributes - keep this line"

char extAttributes_fileName[255];		// path and filename for .ext filename

// INQUIRY Command default data
static uint8_t DefaultInquiryData[] =
{
	0x00,												// Peripherial Device Type
	0x00,												// RMB / Device-Type Qualifier
	0x00,												// ISO Version | ECMA Version | ANSI Version
	0x00,												// Reserved
	0x1E,												// Additional Length
	0x00,												// Vendor Unique
	0x00,												// Vendor Unique
	0x00,												// Vendor Unique
	'B','E','E','B','S','C','S','I',			// Vendor  Identification ASCII  8 bytes
	' ','G','E','N','E','R','I','C',			// Product Identification ASCII 16 bytes
	' ','H','D',' ',' ',' ',' ',' ',							
	'1','.','0','0'								// Product Revision Level ASCII  4 bytes
};

// Mode Parameter Header descriptor - 4 bytes
static uint8_t ModeParameterHeader6 [] =
{
// Mode Parameter Header (6)
	0x10,												// Mode Data length of(MPHeader + LBA BLOCK + Mode Page)-1 : not including this byte
	0x00,												// Medium type (0x00 = fixed hard drive)
	0x00,												// b7 Write Protect bit | b6 Rsvd | b5 DPOFUA | b4-b0	Reserved
	0x08												// the following LBA Block Descriptor Length
};

// Short LBA mode parameter block descriptor - 8 bytes
static uint8_t LBA_byte_block_descriptor_Mode6 [] =
{
	0x00, 0x00, 0x00, 0x00,						// Number of blocks MSB - LSB
	0x00,												// reserved
	(DEFAULT_BLOCK_SIZE & 0xFF0000) >> 16,			// Logical block length (sector size)
	(DEFAULT_BLOCK_SIZE & 0x00FF00) >> 8,
	(DEFAULT_BLOCK_SIZE & 0x0000FF)
};

// Mode Parameter Pages

// Mode Page 1 - Error Correction Status Parameters
static uint8_t ErrorCorrectionStatus[] =
{
	0x01,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
	0x03,						// Following data Length
	0x20,						// | 7 | 6 | TB | 4 | 3 | PER | DTE | DCR |
	0x04,						// Error recovery Retries (4)
	0x05,						// Error Correction Bit Span (5)
};

// Mode Page 3 - Format Device Parameters
static uint8_t FormatDevice[] =
{
	0x03,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
	0x13,						// Page Length (19)
	0x01, 0x32,				// Tracks per Zone (MSB, LSB)
	0x01, 0x32,				// Alternate Sectors per Zone (MSB, LSB)
	0x00, 0x06,				// Alternate Tracks per Zone (MSB, LSB)
	0x00, 0x06,				// Alternate Tracks per Volume (MSB, LSB)
	(DEFAULT_SECTORS_PER_TRACK & 0xFF00) >> 8,		// Sectors per Track (MSB, LSB)
	(DEFAULT_SECTORS_PER_TRACK & 0xFF),		
	(DEFAULT_BLOCK_SIZE & 0x00FF00) >> 8, 				// Data Bytes per Physical Sector (MSB, LSB)
	(DEFAULT_BLOCK_SIZE & 0x0000FF),
	0x00, 0x01,				// Interleave (MSB, LSB)
	0x00, 0x00,				// Track Skew Factor (MSB, LSB)
	0x00, 0x00,				// Cylinder Skew Factor (MSB, LSB)
	0x80,						// | b7 SSEC | b6 HSEC | b5 RMB | b4 SURF | b3-b0 Drive Type
};	

// Mode Page 4 - Rigid Disk Drive Geometry Parameters
uint8_t RigidDiskDriveGeometry[] =
{
	0x04,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
	0x04,						// Page Length (4)											
	0x00, 0x01, 0x32,		// Number of Cylinders (MSB-LSB)							
	0x04						// Number of Heads
};

//Mode Page 32 - Serial Number Parameters
static uint8_t SerialNumber[] =
{
	0x20,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
	0x08,						// Page Length (8)
	'0','0','0','0',
	'0','0','0','0'		// Serial Number ASCII (8 bytes)
};

// Mode Page 33 - Manufacture Parameters
static uint8_t Manufacturer[] =
{
	0x21,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
	0x07,						// Page Length (7)
	0x57, 0x0A, 0x0D,		// Manufacture Date and Build Level (6 bytes)
	0x02, 0x86, 0x00,
	02
};

// Mode Page 35 - System Flags Parameters
static uint8_t SystemFlags[] =
{
	0x23,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
	0x01,						// Page Length (1)
	0x00						// System Flags
};

// Mode Page 36 - Undocumented
static uint8_t Page0x24[] =
{
	0x24,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
	0x02,						// Page Length (2)
	0x00, 0x00				// 
};

// Mode Page 37 - User Page1 Parameters
static uint8_t UserPage1[] =
{
	0x25,						// Page Code	| b7 PS| b6 SPF| b5-b0 Page Code
	0x04,						// Page Length (4)
	'(','C',')','A'		// "(C)A",<cr>,0 (4-bytes)
};

// Mode Page 38 - User Page2 Parameters
static uint8_t UserPage2[] =
{
	0x26,						// Page Code
	0x04,						// Page Length (6)
	'c','o','r','n'		// ASCII (4-bytes)
};



// Looks up the value of token in the extended attibutes file
// for the current LUN and returns the value in buffer
// converting it from a hex string to hex values
//
uint16_t read_attribute(const char *token, char *buf) {

	FIL fileObject;
	FRESULT fsResult;

	char msg[256];

	char left[MAX_PREFS_TOKEN_LEN];
	char right[MAX_PREFS_VALUE_LEN];
	char *dlim_ptr, *end_ptr;
	char fbuf[MAX_PREFS_LINE_LEN];

	size_t token_length, value_length;

	if ((!token) || (!buf)) {
		if (debugFlag_extended_attributes) debugString_P(PSTR("ext_attributes: read_attribute: Invalid argument\r\n"));
		return 0;
	}

	if (debugFlag_extended_attributes) {
		sprintf(msg, "ext_attributes: read_attribute: filename = %s.\r\n", extAttributes_fileName);
		debugString_C(PSTR(msg), DEBUG_INFO);
	}

	fsResult = f_open(&fileObject, extAttributes_fileName, FA_READ);

	if (fsResult != FR_OK) {
		// Something went wrong
		if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: read_attribute: ERROR: Could not read .ext file\r\n"), DEBUG_ERROR);
		return 0;
	}
/*
	if (debugFlag_extended_attributes) {
		sprintf(msg, "ext_attributes: read_attribute: Attempting to find attribute '%s'\r\n", token);
		debugString_P(PSTR(msg));
	}
*/
	// loop through the file looking for the parameter
	while (f_gets(fbuf, MAX_PREFS_LINE_LEN, &fileObject)) {

		// Discard any lines that don't start with A-Z, a-z
		if ( !(((fbuf[0] & 0xDF) >= 'A') && ((fbuf[0] & 0xDF) <= 'Z'))) {
			continue;
		}
/*
		if (debugFlag_extended_attributes) {
			sprintf(msg, "ext_attributes: read_attribute: File read line: %s", fbuf);
			debugString_P(PSTR(msg));
		}
*/
		// try find a delimiting =, and the end of the line
		dlim_ptr = strstr(fbuf, "=");
		end_ptr = strstr(dlim_ptr, "\n");
	
		// check if a delimiter was found
		if (StartsWith(dlim_ptr, "=")){

			*left ='\0';
			*right = '\0';

			// get the token and the value from the line of data
			token_length = (size_t)(dlim_ptr - fbuf);

			if (token_length > MAX_PREFS_TOKEN_LEN){
				if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: read_attribute: token > MAX_TOKEN_LEN chars\r\n"), DEBUG_ERROR);
				f_close(&fileObject);
				return 0;
			}

			// this is the token
			strncpy(left, fbuf, token_length);
			left[token_length]='\0';

/*
			if (debugFlag_extended_attributes) {
				sprintf(msg, "ext_attributes: Token: '%s'\r\n", left);
				debugString_P(PSTR(msg));
				debugStringInt32_P(PSTR("ext_attributes: read_attribute: Token Length: "), (uint32_t)token_length, true);
			}
*/
			// is the left value the same as the token being searched for?
			if (strcmp(left, token) == 0) {
				if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: read_attribute: Attribute token found\r\n"), DEBUG_SUCCESS);


				// get the value
				value_length = (size_t)(end_ptr - dlim_ptr - 1);

				// this is the value
				strncpy(right, dlim_ptr + 1, value_length);
				right[value_length]='\0';
/*
				if (debugFlag_extended_attributes) {
					sprintf(msg, "ext_attributes: Value: %s\r\n", right);
					debugString_P(PSTR(msg));
					debugStringInt32_P(PSTR("ext_attributes: read_attribute: Value length: "), (uint32_t)value_length, true);
				}
*/
				if (value_length > MAX_PREFS_VALUE_LEN){
					if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: read_attribute: value > MAX_VALUE_LEN chars\r\n"), DEBUG_ERROR);
					f_close(&fileObject);
					return 0;
				}

				if (value_length == 0)	{
					if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: read_attribute: no value found for the token\r\n"), DEBUG_ERROR);
					f_close(&fileObject);
					return 0;
				}

				// check the buffer contains valid Hex digits and length (must be even number of digits)
				if (!ValidHexString(right)) {
						f_close(&fileObject);
						return 0;
				}

				// check there is a pointer to buf
				if (buf == NULL){
					if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: read_attribute: Invalid Buffer\r\n"), DEBUG_ERROR);
					f_close(&fileObject);
					return 0;
				}

				// At this point, attribute is found and is valid hex
				// convert from a Hex string into the buffer
//				if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: read_attribute: Valid value found for token.\r\n"), DEBUG_INFO);

				if (debugFlag_extended_attributes) {
					sprintf(msg, "ext_attributes: read_attribute: %s = %s.\r\n", token, right);
					debugString_C(PSTR(msg), DEBUG_SUCCESS);
				}

				FromHexString(right, buf, value_length);
				f_close(&fileObject);
				return (uint16_t)(value_length / 2);
			}

		}

	}

	f_close(&fileObject);

	if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: read_attribute: ERROR: Attribute token not found\r\n"), DEBUG_ERROR);

	return 0;
}

// Gets the Inquiry Data from the file or uses default data
// length is the number of expected bytes and the size of the buffer
//
// Returns 0 if successful and the Inquiry data in the buffer
//
uint8_t getInquiryData(uint8_t bytesRequested, uint8_t *buf, uint8_t LUN) {

	// ensure buffer is fully zeroed in case default data or ext attributes in file
	// is shorter than the amount of bytes requested

	uint8_t buffSize = sizeof buf;
	memset(buf, 0x00 , buffSize);

	if (filesystemCheckLunImage(LUN)) {

		// if extended attributes are available
		if (filesystemHasExtAttributes(LUN)){
			if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: getInquiryData: Extended attributes are available\r\n"), DEBUG_SUCCESS);

			// read the attribute from the file
			if (read_attribute("Inquiry",	(char *)buf) != 0) {
				return 0;
			}
			else {
				if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: getInquiryData: ERROR reading extended attribute: Inquiry\r\n"),DEBUG_ERROR);
				// drop through and use the default data
			}
		}

	}

	// extended attrtibutes not available

	// use the default data
	if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: getInquiryData: Use the default data\r\n"), DEBUG_INFO);

	memcpy(buf, DefaultInquiryData, bytesRequested);
	
	// get the LUN size from the DSC to add to the default model
	// 0 is returned if the DSC cannot be read
	uint16_t LUN_size =	(uint16_t)((filesystemGetLunTotalBytes(LUN)) >> 20);	// size in MB

	int len;

	// Place drive size in the drive model name - Max 5 chars
	if ((LUN_size >=1) && (LUN_size <= 999 ))
		len = sprintf((char*)buf+25,"%dM",LUN_size);
	else
		len = sprintf((char*)buf+25,"BAD M");

	// replace null char with 'B'
	buf[25+len] = 'B';

	// buffer updated with default values - return
	return 0;

}

// Read a mode page, either from the extended attributes file
// of from defaults if available
//
// Returns the length of the mode page data
// otherwise 0 if unsuccessful
//
uint8_t readModePage(uint8_t LUN, uint8_t Page, uint8_t PageLength, uint8_t *returnBuffer) {

	uint8_t status = 0;

	// b7 and b6 contains the PC (page control field which affect which mode page parameters
	// are returned
	// 00 = Current values
	// 01 = Changeable values
	// 10 = Default values
	// 11 = Saved values

	// This is ignored and the current values are always returned

	Page = Page & 0x3F;

	debugStringInt16_P(PSTR("ext_attributes: readModePage: Read Mode Page :"), (uint16_t)Page, true);

	// Which page has been requested? contained in b5-b0
	switch (Page) {

		case 1:		// Error Correction Status Parameters Page
			// get page defaults
			status=getModePage(LUN, ErrorCorrectionStatus, Page, PageLength, returnBuffer);
			break;

		case 3:		// Format Device Parameters Page
			// get page defaults
			status=getModePage(LUN, FormatDevice, Page, PageLength, returnBuffer);
			break;

		case 4:		// Rigid Disk Drive Geometry Parameters Page

			// set the default cylinders and heads to the value in the .dsc
			filesystemGetCylHeads(LUN, RigidDiskDriveGeometry);
			status=getModePage(LUN, RigidDiskDriveGeometry, Page, PageLength, returnBuffer);
			break;

		case 32:	// Serial Number Parameters Page
			// get page defaults
			status=getModePage(LUN, SerialNumber, Page, PageLength, returnBuffer);
			break;

		case 33:	// Manufacturer Parameters Page
			// get page defaults
			status=getModePage(LUN, Manufacturer, Page, PageLength, returnBuffer);
			break;

		case 35:	// System Flags Parameters Page
			// get page defaults
			status=getModePage(LUN, SystemFlags, Page, PageLength, returnBuffer);
			break;

		case 36:	// Undocumented Parameters Page
			// get page defaults
			status=getModePage(LUN, Page0x24, Page, PageLength, returnBuffer);
			break;

		case 37:	// User Page 1
			// get page defaults
			status=getModePage(LUN, UserPage1, Page, PageLength, returnBuffer);
			break;

		case 38:	// User Page 2
			// get page defaults
			status=getModePage(LUN, UserPage2, Page, PageLength, returnBuffer);
			break;

		default:
			if (debugFlag_scsiCommands) debugString_C(PSTR("ext_attributes: readModePage: Page Not Found Error\r\n"), DEBUG_ERROR);
			status = 0;
			break;
	}

	return status;

}

// Create the ModePage to return in the buffer
// either from the extended attributes file or from default values
// returns 0 as unsuccessful
// otherwise returns the total amount of data
//
uint8_t getModePage(uint8_t LUN, uint8_t *DefaultValue, uint8_t Page, uint8_t PageLength, uint8_t *returnBuffer)
{

	uint16_t length = 0;
	uint8_t ptr = 0;
	uint16_t LBA_size;

	if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: getModePage: Retrieving Mode Page data. Header.\r\n"), DEBUG_INFO);

	uint8_t temp_buffer[256];
	memset(temp_buffer, 0x00 , 256);

	// ---------------------------------------------------------------------------------------------------------------------------------------
	// Load the Mode Page Header
	// Curently only handles Mode(6) and Mode(10) formats
	// The difference is the size of the MP Header and the following LBA Descriptor

	if (filesystemHasExtAttributes(LUN))
		length = read_attribute("ModeParamHeader", (char *)temp_buffer);

	// otherwise length stays at 0 and default values are taken

	// if valid header length from the extended attributes
	if ((length == 4) || (length ==8 )) {
		if (debugFlag_extended_attributes) {
			debugString_C(PSTR("ext_attributes: getModePage: Mode Header read from disc successfully.\r\n"), DEBUG_SUCCESS);
			debugStringInt16_P(PSTR("ext_attributes: getModePage: Length of Mode Page Header :"), (uint16_t)length, true);
		}

		memcpy(returnBuffer, temp_buffer, length);
	}
	else {
		if (debugFlag_extended_attributes) {
			debugString_C(PSTR("ext_attributes: getModePage: Retrieving Mode Page Header.\r\n"), DEBUG_ERROR);
			debugStringInt16_P(PSTR("ext_attributes: getModePage: Length of Mode Page Header :"), (uint16_t)length, true);
			debugString_C(PSTR("ext_attributes: getModePage: Using default values for a 4byte Mode Page Header.\r\n"), DEBUG_INFO);
		}

		// Get the default header - 4 bytes
		length = 4;
		memcpy(returnBuffer, ModeParameterHeader6, length);
	}

	// get the size of the LBA Header
	if (length == 4) {
		LBA_size = returnBuffer[3];	// Mode 6
	}
	else {
		LBA_size = (uint16_t)((returnBuffer[6] * 0xff00) + returnBuffer[7]);		// Mode 10
	}

   // Index for the next byte in the buffer to write
	// It can only have the value 4 or 8
	ptr = (uint8_t)(length & 0xFF);

	// ---------------------------------------------------------------------------------------------------------------------------------------
	// read the LBA descriptor
	if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: getModePage: Retrieving Mode Page LBA Descriptor.\r\n"), DEBUG_INFO);

	if (filesystemHasExtAttributes(LUN))
		length = read_attribute("LBADescriptor", (char *)temp_buffer);
	else
		length = 0;

	if (length == LBA_size) {
		// expected  length retrieved from attributes file
/*		if (debugFlag_extended_attributes) {
			debugString_C(PSTR("ext_attributes: getModePage: LBA Descriptor read from disc successfully.\r\n"), DEBUG_SUCCESS);
			debugStringInt16_P(PSTR("ext_attributes: getModePage: Length of LBA descriptor :"), (uint16_t)length, true);
		}
*/
		memcpy(returnBuffer+ptr, temp_buffer, LBA_size);
	}
	else // invalid length size, use defaults.
	{

		if (debugFlag_extended_attributes) {
			debugString_C(PSTR("ext_attributes: getModePage: Error Retrieving LBA descriptor.\r\n"), DEBUG_ERROR);
			debugStringInt16_P(PSTR("ext_attributes: getModePage: Length of LBA descriptor :"), (uint16_t)length, true);
			debugStringInt16_P(PSTR("ext_attributes: getModePage: Expected length of LBA descriptor :"), (uint16_t)LBA_size, true);
			debugString_C(PSTR("ext_attributes: getModePage: Using default values for the LBA descriptor.\r\n"), DEBUG_INFO);
		}

		if (LBA_size == 8)
		{
			memcpy(returnBuffer+ptr, LBA_byte_block_descriptor_Mode6, LBA_size);
		}
		else {
			if (debugFlag_extended_attributes) debugStringInt16_P(PSTR("ext_attributes: getModePage: Invalid LBA length : "), (uint16_t)LBA_size, true);
			return 0;
		}
	}

	// update the pointer to the next position to place data at in the buffer
	ptr = (uint8_t)((ptr + LBA_size) & 0xFF);

	// ---------------------------------------------------------------------------------------------------------------------------------------
	// read the Page data 

//	if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: getModePage: Retrieving Mode Page Data.\r\n"), DEBUG_INFO);

	if (filesystemHasExtAttributes(LUN)){

		// set up the name of the token to lookup
		char token[10];
		sprintf(token, "ModePage%d",Page);

		// if read is successful swap the DefaultValue ptr to the return buffer

		if (read_attribute(token, (char *)temp_buffer)) 		
			DefaultValue = temp_buffer;
		else {
			if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: getModePage: Using default values for the page data.\r\n"), DEBUG_INFO);
		}
	}

	// Otherwise default attributes are being used	

/*
	if (debugFlag_extended_attributes) {
		char msg[60];
		sprintf(msg, "ext_attributes: getModePage: token= %s\r\n", token);
		debugString_C(PSTR(msg), DEBUG_WARNING);
		debugStringInt16_P(PSTR("ext_attributes: getModePage: Full Page Data length :"), (uint16_t)all_modepagedata_length, true);
	}
*/

	uint16_t modepagedata_length = (DefaultValue[1]);

	// Max page length in Mode6 is 241 bytes (255 - 4 (header) - 8 (LBA) - 2 (page header))
	if ((modepagedata_length >=1 ) && (modepagedata_length <=241 )){
		// mode page size is good
		// total length of data = current length + 2 bytes for the page header + page data length
		length = (uint8_t)(length + 2 + modepagedata_length);
	}
	else{
		// change mode page size to the length of data requested 
		length = (uint8_t)(length + 2 + (PageLength-14));

   	//adjust the mode page data lenth being returned
		modepagedata_length = PageLength-14;
	}
/*
	if (debugFlag_extended_attributes) {
		debugStringInt16_P(PSTR("ext_attributes: getModePage: Length of Mode Page Data :"), (uint16_t)modepagedata_length, true);
		debugStringInt16_P(PSTR("ext_attributes: getModePage: Length of data requested :"), (uint16_t)PageLength, true);
		debugStringInt16_P(PSTR("ext_attributes: getModePage: Length of total data:"), (uint16_t)length, true);
		debugStringInt16_P(PSTR("ext_attributes: getModePage: pointer :"), (uint16_t)ptr, true);
	}
*/
	// if length > page length requested, don't overrun the buffer
	if (length > PageLength) {
		if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: getModePage: data is longer than requesting buffer. Trimming.\r\n"), DEBUG_INFO);
		length = PageLength;					// shorten the length to the buffer
		modepagedata_length=length-14;   // shorten the amount of data being returned 
	}

	// insert mode page header
	returnBuffer[ptr++] = Page;
	returnBuffer[ptr++] = (uint8_t)(modepagedata_length & 0xFF);

	// insert mode page data skipping the first 2 bytes of page and length
	memcpy(returnBuffer+ptr, DefaultValue+2, modepagedata_length);

	// no more data to copy, so set pointer to the length of data
	ptr=(uint8_t)(ptr + (modepagedata_length & 0xFF));

//	if (debugFlag_extended_attributes)	debugStringInt16_P(PSTR("ext_attributes: getModePage: new pointer :"), (uint16_t)ptr, true);

	// Put the total data length in the header excluding the initial length byte
	returnBuffer[0] = ptr-1;

	// Show the current buffer data
//	if (debugFlag_extended_attributes) debugBuffer(returnBuffer, ptr-1);

	if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: getModePage: Mode Page read successfully.\r\n"), DEBUG_SUCCESS);
//	if (debugFlag_extended_attributes) debugStringInt16_P(PSTR("ext_attributes: getModePage: total packet length in buffer: "), (uint16_t)returnBuffer[0], true);

	return ptr;
}

// replace Mode Page data in the ext-attributes file with another string
uint8_t replaceModePage(uint8_t LUN, uint8_t *Data) {

	uint8_t status = 0;
	uint8_t retString[512];

//	uint8_t Page=Data[0] & 0x3F;
//	uint8_t DataLength=Data[1];
	
	status =	createModePage(LUN, Data, retString);
	
	return status;
}

// create a non-mode page either from default data or data passed
//
// Returns 0 if successful
// otherwise 1
// 
uint8_t createNonModePage(bool usedefaults, uint8_t token, uint8_t *Data, uint8_t *retString) {

	uint8_t status = 0;
	char HexString[496];

	switch (token) {
		case TOKEN_INQUIRY:
			if (usedefaults)
				ToHexString(HexString, DefaultInquiryData, sizeof(DefaultInquiryData));
			else
				ToHexString(HexString, Data, sizeof(Data));

			sprintf(retString, "Inquiry=%s\r\n", HexString);
			break;

		case TOKEN_MPHEADER:
			if (usedefaults)
				ToHexString(HexString, ModeParameterHeader6, sizeof(ModeParameterHeader6));
			else
				ToHexString(HexString, Data, sizeof(Data));

			sprintf(retString, "ModeParamHeader=%s\r\n", HexString);
			break;
	
		case TOKEN_LBADESCRIPTOR:
			if (usedefaults)
				ToHexString(HexString, LBA_byte_block_descriptor_Mode6, sizeof(LBA_byte_block_descriptor_Mode6));
			else
				ToHexString(HexString, Data, sizeof(Data));

			sprintf(retString, "LBADescriptor=%s\r\n", HexString);
			break;
	
		default:
			if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: createNonModePage: Unrecognised Token\r\n"), DEBUG_ERROR);
			status = 1;

	}

	if (debugFlag_extended_attributes) debugString_C(PSTR(retString), DEBUG_INFO);
	
	return status;
}


// create a mode page either from default data or data passed
//
// Returns 0 if successful
// otherwise 1
// 
uint8_t createModePage(uint8_t LUN, uint8_t *Data, uint8_t *retString) {

	uint8_t status = 0;
	uint8_t Page = Data[0] & 0x3F;
	uint8_t PageLength = Data[1];

	if (debugFlag_extended_attributes) {
		if (LUN & 1) {
			debugStringInt16_P(PSTR("ext_attributes: createModePage: Flag bit SP :"), (uint16_t)(LUN & 1), true);
		}
	}

	// Which page has been requested? contained in b5-b0
	switch (Page) {

		case 0:
			break;
		case 1:		// Error Correction Status Parameters Page
			if (PageLength==0)
				status=setModePage(ErrorCorrectionStatus, retString);
			else
				status=setModePage(Data, retString);
			break;

		case 3:		// Format Device Parameters Page
			if (PageLength==0)
				status=setModePage(FormatDevice, retString);
			else
				status=setModePage(Data, retString);
			break;

		case 4:		// Rigid Disk Drive Geometry Parameters Page
			if (PageLength==0) {
				// set the default cylinders and heads to the value in the .dsc if available
				filesystemGetCylHeads(LUN, RigidDiskDriveGeometry);
				status=setModePage(RigidDiskDriveGeometry, retString);
			}
			else
				status=setModePage(Data, retString);
			break;

		case 32:	// Serial Number Parameters Page
			if (PageLength==0)
				status=setModePage(SerialNumber, retString);
			else
				status=setModePage(Data, retString);
			break;

		case 33:	// Manufacturer Parameters Page
			if (PageLength==0)
				status=setModePage(Manufacturer, retString);
			else
				status=setModePage(Data, retString);
			break;

		case 35:	// System Flags Parameters Page
			if (PageLength==0)
				status=setModePage(SystemFlags, retString);
			else
				status=setModePage(Data, retString);
			break;

		case 36:	// Undocumented Parameters Page
			if (PageLength==0)
				status=setModePage(Page0x24, retString);
			else
				status=setModePage(Data, retString);
			break;

		case 37:	// User Page 1
			if (PageLength==0)
				status=setModePage(UserPage1, retString);
			else
				status=setModePage(Data, retString);
			break;
		case 38:	// User Page 2
			if (PageLength==0)
				status=setModePage(UserPage2, retString);
			else
				status=setModePage(Data, retString);
			break;
		default:
			if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: createModePage: Page not implemented Error\r\n"), DEBUG_ERROR);
			status = 1;
			break;
	}

	if (debugFlag_extended_attributes) debugString_C(PSTR(retString), DEBUG_INFO);
		
	return status;

}

// Return a string (retString) for the requested page in the format
// MODEPAGEx=data
uint8_t setModePage(uint8_t *Data, uint8_t *retString) {

	// set up the name of the token to lookup
	char HexString[496];
	ToHexString(HexString, Data, (Data[1])+2);

	sprintf(retString, "ModePage%d=%s\r\n", Data[0], HexString);

	return 0;
}


// Checks if string a starts with the character(s) given in b
//
bool StartsWith(const char *a, const char *b)
{
	return strncmp(a, b, strlen(b)) == 0;
}

// Converts hex number to ascii string of hex
//
// e.g. 210 -> 323130
void ToHexString (char *hex, char *string, size_t len) 
{

	// Convert hex to ascii
	for (size_t i = 0, j = 0; i < len; ++i, j += 2)
		sprintf(hex + j, "%02X", string[i] & 0xff);

	return;
}

// Converts hex string into values
// hex is the string to convert
// string is the destination
// length is the number of hex digits to convert
//
// e.g. 323130 -> 210
void FromHexString(char *hex, char *string, size_t length) {

	// temp variable to hold the value
	unsigned int val[1];

	for (size_t i = 0, j = 0; j < length; ++i, j += 2) {
		sscanf(hex + j, "%2x", val);
		string[i] = (char)val[0];
		string[i + 1] = '\0';
	}

	return;

}

// Checks a hex string is valid
// first with the number of digits
// then the character set in the string
//
bool ValidHexString(char *HexString) {

	// check for an odd number of chars (because the buffer starts at 0)
	if (!(strlen(HexString) % 2)) {
		//it's even
		if (debugFlag_extended_attributes) debugString_C(PSTR("ext_attributes: read_attribute: value length is odd number of characters\r\n"), DEBUG_ERROR);
		return false;
	}

	// check the digits are 0-9 or Aa-Ff
	for (size_t i = 0; i < strlen(HexString)-1; i++ ) {
		if (HexString[i] == '\0') break;
		if (!(strchr("0123456789ABCDEFabcdef", HexString[i]))) {

			if (debugFlag_extended_attributes) {
				char msg[100];
				sprintf(msg, "ext_attributes: read_attribute: Invalid HEX character found: '%d'\r\n", HexString[i]);
				debugString_C(PSTR(msg), DEBUG_ERROR);
			}
			return (false);
		}
	}

	return (true);
}