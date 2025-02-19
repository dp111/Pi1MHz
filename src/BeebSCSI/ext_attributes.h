
#ifndef EXT_ATTRIBUTES_H_
#define EXT_ATTRIBUTES_H


#define TOKEN_INQUIRY 01
#define TOKEN_MPHEADER 02
#define TOKEN_LBADESCRIPTOR 03


// Function prototypes
uint16_t read_attribute(const char *token, char *buf);

uint8_t getInquiryData(uint8_t bytesRequested, uint8_t *buf, uint8_t LUN);
uint8_t readModePage(uint8_t LUN, uint8_t Page, uint8_t PageLength, uint8_t *returnBuffer);
uint8_t getModePage(uint8_t LUN, uint8_t *DefaultValue, uint8_t Page, uint8_t PageLength, uint8_t *returnBuffer);
uint8_t replaceModePage(uint8_t LUN, uint8_t *Data);

uint8_t createNonModePage(bool usedefaults, uint8_t token, uint8_t *Data, uint8_t *retString);
uint8_t createModePage(uint8_t LUN, uint8_t *Data, uint8_t *retString);
uint8_t setModePage(uint8_t *Data, uint8_t *retString);

bool StartsWith(const char *a, const char *b);
void ToHexString(char *hex, char *string, size_t len);
void FromHexString(char *hex, char *string, size_t length);
bool ValidHexString(char *buf);


// Globals to track extended attributes
extern char extAttributes_fileName[255];

#endif /* EXT_ATTRIBUTES_H_ */