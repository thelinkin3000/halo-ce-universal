/*
ANDROID_LOG.H

The NDK's log interface, for the Switch port.

The Android host library logs through __android_log_print and its siblings.
The Switch has no logcat, but the names are kept rather than changing the
shared sources: host_main.c defines the four of them itself, writing to the
standard error stream that the Homebrew Menu shows. This header declares
them and the priorities, so that the files shared with the Android port need
no changes to be built here.

The priority numbers are the NDK's (ANDROID_LOG_INFO and upwards), because
the guest is told what they are - port/android/guest/runtime/guest_host.h
lists them - and passes them back through host_logf. On the Switch they only
choose a letter at the start of the line.
*/

#ifndef __HALO_SWITCH_ANDROID_LOG_H
#define __HALO_SWITCH_ANDROID_LOG_H

typedef enum android_LogPriority
{
	ANDROID_LOG_UNKNOWN = 0,
	ANDROID_LOG_DEFAULT = 1,
	ANDROID_LOG_VERBOSE = 2,
	ANDROID_LOG_DEBUG = 3,
	ANDROID_LOG_INFO = 4,
	ANDROID_LOG_WARN = 5,
	ANDROID_LOG_ERROR = 6,
	ANDROID_LOG_FATAL = 7,
	ANDROID_LOG_SILENT = 8,
} android_LogPriority;

int __android_log_write(int priority, const char *tag, const char *text);
int __android_log_vprint(int priority, const char *tag, const char *format, __builtin_va_list arguments);
int __android_log_print(int priority, const char *tag, const char *format, ...);

#endif
