#ifndef TASK_TYPES_H
#define TASK_TYPES_H

#include <Arduino.h>

#define MODEM_JOB_ARG1_SIZE 96
#define MODEM_JOB_ARG2_SIZE 1024
#define MODEM_RESULT_MESSAGE_SIZE 1536
#define NOTIFY_SENDER_SIZE 32
#define NOTIFY_TIMESTAMP_SIZE 32
#define NOTIFY_SUBJECT_SIZE 192
#define NOTIFY_BODY_SIZE 2048

enum ModemJobType {
  MODEM_JOB_INIT = 0,
  MODEM_JOB_AT,
  MODEM_JOB_SEND_SMS,
  MODEM_JOB_PING,
  MODEM_JOB_SOFT_RESET,
  MODEM_JOB_HARD_RESET
};

struct ModemJob {
  uint32_t id;
  ModemJobType type;
  unsigned long timeoutMs;
  bool wantsReply;
  char arg1[MODEM_JOB_ARG1_SIZE];
  char arg2[MODEM_JOB_ARG2_SIZE];
};

struct ModemResult {
  uint32_t id;
  bool success;
  char message[MODEM_RESULT_MESSAGE_SIZE];
};

enum NotifyJobType {
  NOTIFY_JOB_SMS_PUSH = 0,
  NOTIFY_JOB_EMAIL
};

struct NotifyJob {
  NotifyJobType type;
  char sender[NOTIFY_SENDER_SIZE];
  char timestamp[NOTIFY_TIMESTAMP_SIZE];
  char subject[NOTIFY_SUBJECT_SIZE];
  char body[NOTIFY_BODY_SIZE];
};

#endif
