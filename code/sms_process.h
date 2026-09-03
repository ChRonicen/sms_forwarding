#ifndef SMS_PROCESS_H
#define SMS_PROCESS_H

#include "globals.h"

void initConcatBuffer();
void initSmsDeliveryGuard();
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts);
String assembleConcatSms(int slot);
void clearConcatSlot(int slot);
void checkConcatTimeout();
String readSerialLine(HardwareSerial& port);
bool isHexString(const String& str);
bool isInNumberBlackList(const char* sender);
bool isAdmin(const char* sender);
void processAdminCommand(const char* sender, const char* text);
bool processSmsContent(const char* sender, const char* text, const char* timestamp);
bool processReceivedPdu(const String& line, int storedIndex = -1, const char* source = "未知");
void checkSerial1URC();
void pollStoredSms();
void processPendingSmsDeletes();

#endif
