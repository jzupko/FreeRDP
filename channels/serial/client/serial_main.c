/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Serial Port Device Service Virtual Channel
 *
 * Copyright 2011 O.S. Systems Software Ltda.
 * Copyright 2011 Eduardo Fiss Beloni <beloni@ossystems.com.br>
 * Copyright 2014 Hewlett-Packard Development Company, L.P.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/collections.h>
#include <winpr/comm.h>
#include <winpr/crt.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/wlog.h>
#include <winpr/assert.h>

#include <freerdp/freerdp.h>
#include <freerdp/channels/rdpdr.h>
#include <freerdp/channels/log.h>
#include <freerdp/utils/rdpdr_utils.h>

#define TAG CHANNELS_TAG("serial.client")

#define MAX_IRP_THREADS 5

typedef struct
{
	DEVICE device;
	BOOL permissive;
	SERIAL_DRIVER_ID ServerSerialDriverId;
	HANDLE hComm;

	wLog* log;
	HANDLE MainThread;
	wMessageQueue* MainIrpQueue;

	/* one thread per pending IRP and indexed according their CompletionId */
	wListDictionary* IrpThreads;
	CRITICAL_SECTION TerminatingIrpThreadsLock;
	rdpContext* rdpcontext;
} SERIAL_DEVICE;

typedef struct
{
	SERIAL_DEVICE* serial;
	IRP* irp;
} IRP_THREAD_DATA;

static void close_terminated_irp_thread_handles(SERIAL_DEVICE* serial, BOOL forceClose);
static NTSTATUS GetLastErrorToIoStatus(SERIAL_DEVICE* serial)
{
	/* http://msdn.microsoft.com/en-us/library/ff547466%28v=vs.85%29.aspx#generic_status_values_for_serial_device_control_requests
	 */
	switch (GetLastError())
	{
		case ERROR_BAD_DEVICE:
			return STATUS_INVALID_DEVICE_REQUEST;

		case ERROR_CALL_NOT_IMPLEMENTED:
			return STATUS_NOT_IMPLEMENTED;

		case ERROR_CANCELLED:
			return STATUS_CANCELLED;

		case ERROR_INSUFFICIENT_BUFFER:
			return STATUS_BUFFER_TOO_SMALL; /* NB: STATUS_BUFFER_SIZE_TOO_SMALL not defined  */

		case ERROR_INVALID_DEVICE_OBJECT_PARAMETER: /* eg: SerCx2.sys' _purge() */
			return STATUS_INVALID_DEVICE_STATE;

		case ERROR_INVALID_HANDLE:
			return STATUS_INVALID_DEVICE_REQUEST;

		case ERROR_INVALID_PARAMETER:
			return STATUS_INVALID_PARAMETER;

		case ERROR_IO_DEVICE:
			return STATUS_IO_DEVICE_ERROR;

		case ERROR_IO_PENDING:
			return STATUS_PENDING;

		case ERROR_NOT_SUPPORTED:
			return STATUS_NOT_SUPPORTED;

		case ERROR_TIMEOUT:
			return STATUS_TIMEOUT;
		default:
			break;
	}

	WLog_Print(serial->log, WLOG_DEBUG, "unexpected last-error: 0x%08" PRIX32 "", GetLastError());
	return STATUS_UNSUCCESSFUL;
}

static UINT serial_process_irp_create(SERIAL_DEVICE* serial, IRP* irp)
{
	DWORD DesiredAccess = 0;
	DWORD SharedAccess = 0;
	DWORD CreateDisposition = 0;
	UINT32 PathLength = 0;

	WINPR_ASSERT(serial);
	WINPR_ASSERT(irp);

	if (!Stream_CheckAndLogRequiredLengthWLog(serial->log, irp->input, 32))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT32(irp->input, DesiredAccess);     /* DesiredAccess (4 bytes) */
	Stream_Seek_UINT64(irp->input);                    /* AllocationSize (8 bytes) */
	Stream_Seek_UINT32(irp->input);                    /* FileAttributes (4 bytes) */
	Stream_Read_UINT32(irp->input, SharedAccess);      /* SharedAccess (4 bytes) */
	Stream_Read_UINT32(irp->input, CreateDisposition); /* CreateDisposition (4 bytes) */
	Stream_Seek_UINT32(irp->input);                    /* CreateOptions (4 bytes) */
	Stream_Read_UINT32(irp->input, PathLength);        /* PathLength (4 bytes) */

	if (!Stream_SafeSeek(irp->input, PathLength)) /* Path (variable) */
		return ERROR_INVALID_DATA;

	WINPR_ASSERT(PathLength == 0); /* MS-RDPESP 2.2.2.2 */
#ifndef _WIN32
	/* Windows 2012 server sends on a first call :
	 *     DesiredAccess     = 0x00100080: SYNCHRONIZE | FILE_READ_ATTRIBUTES
	 *     SharedAccess      = 0x00000007: FILE_SHARE_DELETE | FILE_SHARE_WRITE | FILE_SHARE_READ
	 *     CreateDisposition = 0x00000001: CREATE_NEW
	 *
	 * then Windows 2012 sends :
	 *     DesiredAccess     = 0x00120089: SYNCHRONIZE | READ_CONTROL | FILE_READ_ATTRIBUTES |
	 * FILE_READ_EA | FILE_READ_DATA SharedAccess      = 0x00000007: FILE_SHARE_DELETE |
	 * FILE_SHARE_WRITE | FILE_SHARE_READ CreateDisposition = 0x00000001: CREATE_NEW
	 *
	 * WINPR_ASSERT(DesiredAccess == (GENERIC_READ | GENERIC_WRITE));
	 * WINPR_ASSERT(SharedAccess == 0);
	 * WINPR_ASSERT(CreateDisposition == OPEN_EXISTING);
	 *
	 */
	WLog_Print(serial->log, WLOG_DEBUG,
	           "DesiredAccess: 0x%" PRIX32 ", SharedAccess: 0x%" PRIX32
	           ", CreateDisposition: 0x%" PRIX32 "",
	           DesiredAccess, SharedAccess, CreateDisposition);
	/* FIXME: As of today only the flags below are supported by CommCreateFileA: */
	DesiredAccess = GENERIC_READ | GENERIC_WRITE;
	SharedAccess = 0;
	CreateDisposition = OPEN_EXISTING;
#endif
	serial->hComm = winpr_CreateFile(serial->device.name, DesiredAccess, SharedAccess,
	                                 NULL,                 /* SecurityAttributes */
	                                 CreateDisposition, 0, /* FlagsAndAttributes */
	                                 NULL);                /* TemplateFile */

	if (!serial->hComm || (serial->hComm == INVALID_HANDLE_VALUE))
	{
		WLog_Print(serial->log, WLOG_WARN, "CreateFile failure: %s last-error: 0x%08" PRIX32 "",
		           serial->device.name, GetLastError());
		irp->IoStatus = STATUS_UNSUCCESSFUL;
		goto error_handle;
	}

	_comm_setServerSerialDriver(serial->hComm, serial->ServerSerialDriverId);
	_comm_set_permissive(serial->hComm, serial->permissive);
	/* NOTE: binary mode/raw mode required for the redirection. On
	 * Linux, CommCreateFileA forces this setting.
	 */
	/* ZeroMemory(&dcb, sizeof(DCB)); */
	/* dcb.DCBlength = sizeof(DCB); */
	/* GetCommState(serial->hComm, &dcb); */
	/* dcb.fBinary = TRUE; */
	/* SetCommState(serial->hComm, &dcb); */
	WINPR_ASSERT(irp->FileId == 0);
	irp->FileId = irp->devman->id_sequence++; /* FIXME: why not ((WINPR_COMM*)hComm)->fd? */
	irp->IoStatus = STATUS_SUCCESS;
	WLog_Print(serial->log, WLOG_DEBUG, "%s (DeviceId: %" PRIu32 ", FileId: %" PRIu32 ") created.",
	           serial->device.name, irp->device->id, irp->FileId);

	DWORD BytesReturned = 0;
	if (!CommDeviceIoControl(serial->hComm, IOCTL_SERIAL_RESET_DEVICE, NULL, 0, NULL, 0,
	                         &BytesReturned, NULL))
		goto error_handle;

error_handle:
	Stream_Write_UINT32(irp->output, irp->FileId); /* FileId (4 bytes) */
	Stream_Write_UINT8(irp->output, 0);            /* Information (1 byte) */
	return CHANNEL_RC_OK;
}

static UINT serial_process_irp_close(SERIAL_DEVICE* serial, IRP* irp)
{
	WINPR_ASSERT(serial);
	WINPR_ASSERT(irp);

	if (!Stream_CheckAndLogRequiredLengthWLog(serial->log, irp->input, 32))
		return ERROR_INVALID_DATA;

	Stream_Seek(irp->input, 32); /* Padding (32 bytes) */

	close_terminated_irp_thread_handles(serial, TRUE);

	if (!CloseHandle(serial->hComm))
	{
		WLog_Print(serial->log, WLOG_WARN, "CloseHandle failure: %s (%" PRIu32 ") closed.",
		           serial->device.name, irp->device->id);
		irp->IoStatus = STATUS_UNSUCCESSFUL;
		goto error_handle;
	}

	WLog_Print(serial->log, WLOG_DEBUG, "%s (DeviceId: %" PRIu32 ", FileId: %" PRIu32 ") closed.",
	           serial->device.name, irp->device->id, irp->FileId);
	irp->IoStatus = STATUS_SUCCESS;
error_handle:
	serial->hComm = NULL;
	Stream_Zero(irp->output, 5); /* Padding (5 bytes) */
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT serial_process_irp_read(SERIAL_DEVICE* serial, IRP* irp)
{
	UINT32 Length = 0;
	UINT64 Offset = 0;
	BYTE* buffer = NULL;
	DWORD nbRead = 0;

	WINPR_ASSERT(serial);
	WINPR_ASSERT(irp);

	if (!Stream_CheckAndLogRequiredLengthWLog(serial->log, irp->input, 32))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT32(irp->input, Length); /* Length (4 bytes) */
	Stream_Read_UINT64(irp->input, Offset); /* Offset (8 bytes) */
	(void)Offset; /* [MS-RDPESP] 3.2.5.1.4 Processing a Server Read Request Message
	               * ignored */
	Stream_Seek(irp->input, 20);            /* Padding (20 bytes) */
	buffer = (BYTE*)calloc(Length, sizeof(BYTE));

	if (buffer == NULL)
	{
		irp->IoStatus = STATUS_NO_MEMORY;
		goto error_handle;
	}

	/* MS-RDPESP 3.2.5.1.4: If the Offset field is not set to 0, the value MUST be ignored
	 * WINPR_ASSERT(Offset == 0);
	 */
	WLog_Print(serial->log, WLOG_DEBUG, "reading %" PRIu32 " bytes from %s", Length,
	           serial->device.name);

	/* FIXME: CommReadFile to be replaced by ReadFile */
	if (CommReadFile(serial->hComm, buffer, Length, &nbRead, NULL))
	{
		irp->IoStatus = STATUS_SUCCESS;
	}
	else
	{
		WLog_Print(serial->log, WLOG_DEBUG,
		           "read failure to %s, nbRead=%" PRIu32 ", last-error: 0x%08" PRIX32 "",
		           serial->device.name, nbRead, GetLastError());
		irp->IoStatus = GetLastErrorToIoStatus(serial);
	}

	WLog_Print(serial->log, WLOG_DEBUG, "%" PRIu32 " bytes read from %s", nbRead,
	           serial->device.name);
error_handle:
	Stream_Write_UINT32(irp->output, nbRead); /* Length (4 bytes) */

	if (nbRead > 0)
	{
		if (!Stream_EnsureRemainingCapacity(irp->output, nbRead))
		{
			WLog_Print(serial->log, WLOG_ERROR, "Stream_EnsureRemainingCapacity failed!");
			free(buffer);
			return CHANNEL_RC_NO_MEMORY;
		}

		Stream_Write(irp->output, buffer, nbRead); /* ReadData */
	}

	free(buffer);
	return CHANNEL_RC_OK;
}

static UINT serial_process_irp_write(SERIAL_DEVICE* serial, IRP* irp)
{
	UINT32 Length = 0;
	UINT64 Offset = 0;
	DWORD nbWritten = 0;

	WINPR_ASSERT(serial);
	WINPR_ASSERT(irp);

	if (!Stream_CheckAndLogRequiredLengthWLog(serial->log, irp->input, 32))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT32(irp->input, Length); /* Length (4 bytes) */
	Stream_Read_UINT64(irp->input, Offset); /* Offset (8 bytes) */
	(void)Offset; /* [MS-RDPESP] 3.2.5.1.4 Processing a Server Read Request Message
	               * ignored */
	if (!Stream_SafeSeek(irp->input, 20))   /* Padding (20 bytes) */
		return ERROR_INVALID_DATA;

	/* MS-RDPESP 3.2.5.1.5: The Offset field is ignored
	 * WINPR_ASSERT(Offset == 0);
	 *
	 * Using a serial printer, noticed though this field could be
	 * set.
	 */
	WLog_Print(serial->log, WLOG_DEBUG, "writing %" PRIu32 " bytes to %s", Length,
	           serial->device.name);

	const void* ptr = Stream_ConstPointer(irp->input);
	if (!Stream_SafeSeek(irp->input, Length))
		return ERROR_INVALID_DATA;
	/* FIXME: CommWriteFile to be replaced by WriteFile */
	if (CommWriteFile(serial->hComm, ptr, Length, &nbWritten, NULL))
	{
		irp->IoStatus = STATUS_SUCCESS;
	}
	else
	{
		WLog_Print(serial->log, WLOG_DEBUG,
		           "write failure to %s, nbWritten=%" PRIu32 ", last-error: 0x%08" PRIX32 "",
		           serial->device.name, nbWritten, GetLastError());
		irp->IoStatus = GetLastErrorToIoStatus(serial);
	}

	WLog_Print(serial->log, WLOG_DEBUG, "%" PRIu32 " bytes written to %s", nbWritten,
	           serial->device.name);
	Stream_Write_UINT32(irp->output, nbWritten); /* Length (4 bytes) */
	Stream_Write_UINT8(irp->output, 0);          /* Padding (1 byte) */
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT serial_process_irp_device_control(SERIAL_DEVICE* serial, IRP* irp)
{
	UINT32 IoControlCode = 0;
	UINT32 InputBufferLength = 0;
	BYTE* InputBuffer = NULL;
	UINT32 OutputBufferLength = 0;
	BYTE* OutputBuffer = NULL;
	DWORD BytesReturned = 0;

	WINPR_ASSERT(serial);
	WINPR_ASSERT(irp);

	if (!Stream_CheckAndLogRequiredLengthWLog(serial->log, irp->input, 32))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT32(irp->input, OutputBufferLength); /* OutputBufferLength (4 bytes) */
	Stream_Read_UINT32(irp->input, InputBufferLength);  /* InputBufferLength (4 bytes) */
	Stream_Read_UINT32(irp->input, IoControlCode);      /* IoControlCode (4 bytes) */
	Stream_Seek(irp->input, 20);                        /* Padding (20 bytes) */

	if (!Stream_CheckAndLogRequiredLengthWLog(serial->log, irp->input, InputBufferLength))
		return ERROR_INVALID_DATA;

	OutputBuffer = (BYTE*)calloc(OutputBufferLength, sizeof(BYTE));

	if (OutputBuffer == NULL)
	{
		irp->IoStatus = STATUS_NO_MEMORY;
		goto error_handle;
	}

	InputBuffer = (BYTE*)calloc(InputBufferLength, sizeof(BYTE));

	if (InputBuffer == NULL)
	{
		irp->IoStatus = STATUS_NO_MEMORY;
		goto error_handle;
	}

	Stream_Read(irp->input, InputBuffer, InputBufferLength);
	WLog_Print(serial->log, WLOG_DEBUG,
	           "CommDeviceIoControl: CompletionId=%" PRIu32 ", IoControlCode=[0x%" PRIX32 "] %s",
	           irp->CompletionId, IoControlCode, _comm_serial_ioctl_name(IoControlCode));

	/* FIXME: CommDeviceIoControl to be replaced by DeviceIoControl() */
	if (CommDeviceIoControl(serial->hComm, IoControlCode, InputBuffer, InputBufferLength,
	                        OutputBuffer, OutputBufferLength, &BytesReturned, NULL))
	{
		/* WLog_Print(serial->log, WLOG_DEBUG, "CommDeviceIoControl: CompletionId=%"PRIu32",
		 * IoControlCode=[0x%"PRIX32"] %s done", irp->CompletionId, IoControlCode,
		 * _comm_serial_ioctl_name(IoControlCode)); */
		irp->IoStatus = STATUS_SUCCESS;
	}
	else
	{
		WLog_Print(serial->log, WLOG_DEBUG,
		           "CommDeviceIoControl failure: IoControlCode=[0x%" PRIX32
		           "] %s, last-error: 0x%08" PRIX32 "",
		           IoControlCode, _comm_serial_ioctl_name(IoControlCode), GetLastError());
		irp->IoStatus = GetLastErrorToIoStatus(serial);
	}

error_handle:
	/* FIXME: find out whether it's required or not to get
	 * BytesReturned == OutputBufferLength when
	 * CommDeviceIoControl returns FALSE */
	WINPR_ASSERT(OutputBufferLength == BytesReturned);
	Stream_Write_UINT32(irp->output, BytesReturned); /* OutputBufferLength (4 bytes) */

	if (BytesReturned > 0)
	{
		if (!Stream_EnsureRemainingCapacity(irp->output, BytesReturned))
		{
			WLog_Print(serial->log, WLOG_ERROR, "Stream_EnsureRemainingCapacity failed!");
			free(InputBuffer);
			free(OutputBuffer);
			return CHANNEL_RC_NO_MEMORY;
		}

		Stream_Write(irp->output, OutputBuffer, BytesReturned); /* OutputBuffer */
	}

	/* FIXME: Why at least Windows 2008R2 gets lost with this
	 * extra byte and likely on a IOCTL_SERIAL_SET_BAUD_RATE? The
	 * extra byte is well required according MS-RDPEFS
	 * 2.2.1.5.5 */
	/* else */
	/* { */
	/* 	Stream_Write_UINT8(irp->output, 0); /\* Padding (1 byte) *\/ */
	/* } */
	free(InputBuffer);
	free(OutputBuffer);
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT serial_process_irp(SERIAL_DEVICE* serial, IRP* irp)
{
	UINT error = CHANNEL_RC_OK;

	WINPR_ASSERT(serial);
	WINPR_ASSERT(irp);

	WLog_Print(serial->log, WLOG_DEBUG, "IRP MajorFunction: %s, MinorFunction: 0x%08" PRIX32 "\n",
	           rdpdr_irp_string(irp->MajorFunction), irp->MinorFunction);

	switch (irp->MajorFunction)
	{
		case IRP_MJ_CREATE:
			error = serial_process_irp_create(serial, irp);
			break;

		case IRP_MJ_CLOSE:
			error = serial_process_irp_close(serial, irp);
			break;

		case IRP_MJ_READ:
			error = serial_process_irp_read(serial, irp);
			break;

		case IRP_MJ_WRITE:
			error = serial_process_irp_write(serial, irp);
			break;

		case IRP_MJ_DEVICE_CONTROL:
			error = serial_process_irp_device_control(serial, irp);
			break;

		default:
			irp->IoStatus = STATUS_NOT_SUPPORTED;
			break;
	}

	DWORD level = WLOG_TRACE;
	if (error)
		level = WLOG_WARN;

	WLog_Print(serial->log, level,
	           "[%s|0x%08" PRIx32 "] completed with %s [0x%08" PRIx32 "] (IoStatus %s [0x%08" PRIx32
	           "])",
	           rdpdr_irp_string(irp->MajorFunction), irp->MajorFunction, WTSErrorToString(error),
	           error, NtStatus2Tag(irp->IoStatus), irp->IoStatus);

	return error;
}

static DWORD WINAPI irp_thread_func(LPVOID arg)
{
	IRP_THREAD_DATA* data = (IRP_THREAD_DATA*)arg;
	UINT error = 0;

	WINPR_ASSERT(data);
	WINPR_ASSERT(data->serial);
	WINPR_ASSERT(data->irp);

	/* blocks until the end of the request */
	if ((error = serial_process_irp(data->serial, data->irp)))
	{
		WLog_Print(data->serial->log, WLOG_ERROR,
		           "serial_process_irp failed with error %" PRIu32 "", error);
		goto error_out;
	}

	EnterCriticalSection(&data->serial->TerminatingIrpThreadsLock);
	WINPR_ASSERT(data->irp->Complete);
	error = data->irp->Complete(data->irp);
	LeaveCriticalSection(&data->serial->TerminatingIrpThreadsLock);
error_out:

	if (error && data->serial->rdpcontext)
		setChannelError(data->serial->rdpcontext, error, "irp_thread_func reported an error");

	if (error)
		data->irp->Discard(data->irp);

	/* NB: At this point, the server might already being reusing
	 * the CompletionId whereas the thread is not yet
	 * terminated */
	free(data);
	ExitThread(error);
	return error;
}

static void close_unterminated_irp_thread(wListDictionary* list, wLog* log, ULONG_PTR id)
{
	WINPR_ASSERT(list);
	HANDLE self = _GetCurrentThread();
	HANDLE cirpThread = ListDictionary_GetItemValue(list, (void*)id);
	if (self == cirpThread)
		WLog_Print(log, WLOG_DEBUG, "Skipping termination of own IRP thread");
	else
		ListDictionary_Remove(list, (void*)id);
}

static void close_terminated_irp_thread(wListDictionary* list, wLog* log, ULONG_PTR id)
{
	WINPR_ASSERT(list);

	HANDLE cirpThread = ListDictionary_GetItemValue(list, (void*)id);
	/* FIXME: not quite sure a zero timeout is a good thing to check whether a thread is
	 * still alive or not */
	const DWORD waitResult = WaitForSingleObject(cirpThread, 0);

	if (waitResult == WAIT_OBJECT_0)
		ListDictionary_Remove(list, (void*)id);
	else if (waitResult != WAIT_TIMEOUT)
	{
		/* unexpected thread state */
		WLog_Print(log, WLOG_WARN, "WaitForSingleObject, got an unexpected result=0x%" PRIX32 "\n",
		           waitResult);
	}
}

void close_terminated_irp_thread_handles(SERIAL_DEVICE* serial, BOOL forceClose)
{
	WINPR_ASSERT(serial);

	EnterCriticalSection(&serial->TerminatingIrpThreadsLock);

	ULONG_PTR* ids = NULL;
	const size_t nbIds = ListDictionary_GetKeys(serial->IrpThreads, &ids);

	for (size_t i = 0; i < nbIds; i++)
	{
		ULONG_PTR id = ids[i];
		if (forceClose)
			close_unterminated_irp_thread(serial->IrpThreads, serial->log, id);
		else
			close_terminated_irp_thread(serial->IrpThreads, serial->log, id);
	}

	free(ids);

	LeaveCriticalSection(&serial->TerminatingIrpThreadsLock);
}

static void create_irp_thread(SERIAL_DEVICE* serial, IRP* irp)
{
	IRP_THREAD_DATA* data = NULL;
	HANDLE irpThread = NULL;
	HANDLE previousIrpThread = NULL;
	uintptr_t key = 0;

	WINPR_ASSERT(serial);
	WINPR_ASSERT(irp);

	close_terminated_irp_thread_handles(serial, FALSE);

	/* NB: At this point and thanks to the synchronization we're
	 * sure that the incoming IRP uses well a recycled
	 * CompletionId or the server sent again an IRP already posted
	 * which didn't get yet a response (this later server behavior
	 * at least observed with IOCTL_SERIAL_WAIT_ON_MASK and
	 * mstsc.exe).
	 *
	 * FIXME: behavior documented somewhere? behavior not yet
	 * observed with FreeRDP).
	 */
	key = irp->CompletionId + 1ull;
	previousIrpThread = ListDictionary_GetItemValue(serial->IrpThreads, (void*)key);

	if (previousIrpThread)
	{
		/* Thread still alived <=> Request still pending */
		WLog_Print(serial->log, WLOG_DEBUG,
		           "IRP recall: IRP with the CompletionId=%" PRIu32 " not yet completed!",
		           irp->CompletionId);
		WINPR_ASSERT(FALSE); /* unimplemented */
		/* TODO: WINPR_ASSERTs that previousIrpThread handles well
		 * the same request by checking more details. Need an
		 * access to the IRP object used by previousIrpThread
		 */
		/* TODO: taking over the pending IRP or sending a kind
		 * of wake up signal to accelerate the pending
		 * request
		 *
		 * To be considered:
		 *   if (IoControlCode == IOCTL_SERIAL_WAIT_ON_MASK) {
		 *       pComm->PendingEvents |= SERIAL_EV_FREERDP_*;
		 *   }
		 */
		irp->Discard(irp);
		return;
	}

	if (ListDictionary_Count(serial->IrpThreads) >= MAX_IRP_THREADS)
	{
		WLog_Print(serial->log, WLOG_WARN,
		           "Number of IRP threads threshold reached: %" PRIuz ", keep on anyway",
		           ListDictionary_Count(serial->IrpThreads));
		WINPR_ASSERT(FALSE); /* unimplemented */
		                     /* TODO: MAX_IRP_THREADS has been thought to avoid a
		                      * flooding of pending requests. Use
		                      * WaitForMultipleObjects() when available in winpr
		                      * for threads.
		                      */
	}

	/* error_handle to be used ... */
	data = (IRP_THREAD_DATA*)calloc(1, sizeof(IRP_THREAD_DATA));

	if (data == NULL)
	{
		WLog_Print(serial->log, WLOG_WARN, "Could not allocate a new IRP_THREAD_DATA.");
		goto error_handle;
	}

	data->serial = serial;
	data->irp = irp;
	/* data freed by irp_thread_func */
	irpThread = CreateThread(NULL, 0, irp_thread_func, (void*)data, CREATE_SUSPENDED, NULL);

	if (irpThread == INVALID_HANDLE_VALUE)
	{
		WLog_Print(serial->log, WLOG_WARN, "Could not allocate a new IRP thread.");
		goto error_handle;
	}

	key = irp->CompletionId + 1ull;

	if (!ListDictionary_Add(serial->IrpThreads, (void*)key, irpThread))
	{
		WLog_Print(serial->log, WLOG_ERROR, "ListDictionary_Add failed!");
		goto error_handle;
	}

	ResumeThread(irpThread);

	return;
error_handle:
	if (irpThread)
		(void)CloseHandle(irpThread);
	irp->IoStatus = STATUS_NO_MEMORY;
	WINPR_ASSERT(irp->Complete);
	irp->Complete(irp);
	free(data);
}

static DWORD WINAPI serial_thread_func(LPVOID arg)
{
	IRP* irp = NULL;
	wMessage message = { 0 };
	SERIAL_DEVICE* serial = (SERIAL_DEVICE*)arg;
	UINT error = CHANNEL_RC_OK;

	WINPR_ASSERT(serial);

	while (1)
	{
		if (!MessageQueue_Wait(serial->MainIrpQueue))
		{
			WLog_Print(serial->log, WLOG_ERROR, "MessageQueue_Wait failed!");
			error = ERROR_INTERNAL_ERROR;
			break;
		}

		if (!MessageQueue_Peek(serial->MainIrpQueue, &message, TRUE))
		{
			WLog_Print(serial->log, WLOG_ERROR, "MessageQueue_Peek failed!");
			error = ERROR_INTERNAL_ERROR;
			break;
		}

		if (message.id == WMQ_QUIT)
			break;

		irp = (IRP*)message.wParam;

		if (irp)
			create_irp_thread(serial, irp);
	}

	ListDictionary_Clear(serial->IrpThreads);
	if (error && serial->rdpcontext)
		setChannelError(serial->rdpcontext, error, "serial_thread_func reported an error");

	ExitThread(error);
	return error;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT serial_irp_request(DEVICE* device, IRP* irp)
{
	SERIAL_DEVICE* serial = (SERIAL_DEVICE*)device;
	WINPR_ASSERT(irp != NULL);
	WINPR_ASSERT(serial);

	if (irp == NULL)
		return CHANNEL_RC_OK;

	/* NB: ENABLE_ASYNCIO is set, (MS-RDPEFS 2.2.2.7.2) this
	 * allows the server to send multiple simultaneous read or
	 * write requests.
	 */

	if (!MessageQueue_Post(serial->MainIrpQueue, NULL, 0, (void*)irp, NULL))
	{
		WLog_Print(serial->log, WLOG_ERROR, "MessageQueue_Post failed!");
		return ERROR_INTERNAL_ERROR;
	}

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT serial_free(DEVICE* device)
{
	UINT error = 0;
	SERIAL_DEVICE* serial = (SERIAL_DEVICE*)device;
	if (!serial)
		return CHANNEL_RC_OK;

	WLog_Print(serial->log, WLOG_DEBUG, "freeing");
	if (serial->MainIrpQueue)
		MessageQueue_PostQuit(serial->MainIrpQueue, 0);

	if (serial->MainThread)
	{
		if (WaitForSingleObject(serial->MainThread, INFINITE) == WAIT_FAILED)
		{
			error = GetLastError();
			WLog_Print(serial->log, WLOG_ERROR,
			           "WaitForSingleObject failed with error %" PRIu32 "!", error);
		}
		(void)CloseHandle(serial->MainThread);
	}

	if (serial->hComm)
		(void)CloseHandle(serial->hComm);

	/* Clean up resources */
	Stream_Free(serial->device.data, TRUE);
	MessageQueue_Free(serial->MainIrpQueue);
	ListDictionary_Free(serial->IrpThreads);
	DeleteCriticalSection(&serial->TerminatingIrpThreadsLock);
	free(serial);
	return CHANNEL_RC_OK;
}

static void serial_message_free(void* obj)
{
	wMessage* msg = obj;
	if (!msg)
		return;
	if (msg->id != 0)
		return;

	IRP* irp = (IRP*)msg->wParam;
	if (!irp)
		return;
	WINPR_ASSERT(irp->Discard);
	irp->Discard(irp);
}

static void irp_thread_close(void* arg)
{
	HANDLE hdl = arg;
	if (hdl)
	{
		HANDLE thz = _GetCurrentThread();
		if (thz == hdl)
			WLog_WARN(TAG, "closing self, ignoring...");
		else
		{
			(void)TerminateThread(hdl, 0);
			(void)WaitForSingleObject(hdl, INFINITE);
			(void)CloseHandle(hdl);
		}
	}
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
FREERDP_ENTRY_POINT(
    UINT VCAPITYPE serial_DeviceServiceEntry(PDEVICE_SERVICE_ENTRY_POINTS pEntryPoints))
{
	size_t len = 0;
	SERIAL_DEVICE* serial = NULL;
	UINT error = CHANNEL_RC_OK;

	WINPR_ASSERT(pEntryPoints);

	RDPDR_SERIAL* device = (RDPDR_SERIAL*)pEntryPoints->device;
	WINPR_ASSERT(device);

	wLog* log = WLog_Get(TAG);
	const char* name = device->device.Name;
	const char* path = device->Path;
	const char* driver = device->Driver;

	if (!name || (name[0] == '*'))
	{
		/* TODO: implement auto detection of serial ports */
		WLog_Print(log, WLOG_WARN,
		           "Serial port autodetection not implemented, nothing will be redirected!");
		return CHANNEL_RC_OK;
	}

	if ((name && name[0]) && (path && path[0]))
	{
		WLog_Print(log, WLOG_DEBUG, "Defining %s as %s", name, path);

		if (!DefineCommDevice(name /* eg: COM1 */, path /* eg: /dev/ttyS0 */))
		{
			DWORD status = GetLastError();
			WLog_Print(log, WLOG_ERROR, "DefineCommDevice failed with %08" PRIx32, status);
			return ERROR_INTERNAL_ERROR;
		}

		serial = (SERIAL_DEVICE*)calloc(1, sizeof(SERIAL_DEVICE));

		if (!serial)
		{
			WLog_Print(log, WLOG_ERROR, "calloc failed!");
			return CHANNEL_RC_NO_MEMORY;
		}

		serial->log = log;
		serial->device.type = RDPDR_DTYP_SERIAL;
		serial->device.name = name;
		serial->device.IRPRequest = serial_irp_request;
		serial->device.Free = serial_free;
		serial->rdpcontext = pEntryPoints->rdpcontext;
		len = strlen(name);
		serial->device.data = Stream_New(NULL, len + 1);

		if (!serial->device.data)
		{
			WLog_Print(serial->log, WLOG_ERROR, "calloc failed!");
			error = CHANNEL_RC_NO_MEMORY;
			goto error_out;
		}

		for (size_t i = 0; i <= len; i++)
			Stream_Write_INT8(serial->device.data, name[i] < 0 ? '_' : name[i]);

		if (driver != NULL)
		{
			if (_stricmp(driver, "Serial") == 0)
				serial->ServerSerialDriverId = SerialDriverSerialSys;
			else if (_stricmp(driver, "SerCx") == 0)
				serial->ServerSerialDriverId = SerialDriverSerCxSys;
			else if (_stricmp(driver, "SerCx2") == 0)
				serial->ServerSerialDriverId = SerialDriverSerCx2Sys;
			else
			{
				WLog_Print(serial->log, WLOG_WARN, "Unknown server's serial driver: %s.", driver);
				WLog_Print(serial->log, WLOG_WARN,
				           "Valid options are: 'Serial' (default), 'SerCx' and 'SerCx2'");
				goto error_out;
			}
		}
		else
		{
			/* default driver */
			serial->ServerSerialDriverId = SerialDriverSerialSys;
		}

		if (device->Permissive != NULL)
		{
			if (_stricmp(device->Permissive, "permissive") == 0)
			{
				serial->permissive = TRUE;
			}
			else
			{
				WLog_Print(serial->log, WLOG_WARN, "Unknown flag: %s", device->Permissive);
				goto error_out;
			}
		}

		WLog_Print(serial->log, WLOG_DEBUG, "Server's serial driver: %s (id: %d)", driver,
		           serial->ServerSerialDriverId);

		serial->MainIrpQueue = MessageQueue_New(NULL);

		if (!serial->MainIrpQueue)
		{
			WLog_Print(serial->log, WLOG_ERROR, "MessageQueue_New failed!");
			error = CHANNEL_RC_NO_MEMORY;
			goto error_out;
		}

		{
			wObject* obj = MessageQueue_Object(serial->MainIrpQueue);
			WINPR_ASSERT(obj);
			obj->fnObjectFree = serial_message_free;
		}

		/* IrpThreads content only modified by create_irp_thread() */
		serial->IrpThreads = ListDictionary_New(FALSE);

		if (!serial->IrpThreads)
		{
			WLog_Print(serial->log, WLOG_ERROR, "ListDictionary_New failed!");
			error = CHANNEL_RC_NO_MEMORY;
			goto error_out;
		}

		{
			wObject* obj = ListDictionary_ValueObject(serial->IrpThreads);
			WINPR_ASSERT(obj);
			obj->fnObjectFree = irp_thread_close;
		}

		InitializeCriticalSection(&serial->TerminatingIrpThreadsLock);

		error = pEntryPoints->RegisterDevice(pEntryPoints->devman, &serial->device);
		if (error != CHANNEL_RC_OK)
		{
			WLog_Print(serial->log, WLOG_ERROR,
			           "EntryPoints->RegisterDevice failed with error %" PRIu32 "!", error);
			goto error_out;
		}

		serial->MainThread = CreateThread(NULL, 0, serial_thread_func, serial, 0, NULL);
		if (!serial->MainThread)
		{
			WLog_Print(serial->log, WLOG_ERROR, "CreateThread failed!");
			error = ERROR_INTERNAL_ERROR;
			goto error_out;
		}
	}

	return error;
error_out:
	if (serial)
		serial_free(&serial->device);
	return error;
}
