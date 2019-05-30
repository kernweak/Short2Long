#include <ntifs.h>
#include <ntstrsafe.h>
#include <ntddk.h>
#include <windef.h>

BOOL IsRootDirecotry(WCHAR * wszDir)
{
	SIZE_T length = wcslen(wszDir);
	
	// c:
	if((length == 2) && (wszDir[1] == L':'))
		return TRUE;
	//\\??\\c:
	if((length == 6) && 
		(_wcsnicmp(wszDir, L"\\??\\", 4) == 0) &&
		(wszDir[5] == L':'))
		return TRUE;
	//\\DosDevices\\c:
	if((length == 14) && 
		(_wcsnicmp(wszDir, L"\\DosDevices\\", 12) == 0) &&
		(wszDir[13] == L':'))
		return TRUE;
	//\\Device\\HarddiskVolume1
	if((length == 23) &&
		(_wcsnicmp(wszDir, L"\\Device\\HarddiskVolume", 22) == 0))
		return TRUE;
	
	
	return FALSE;
}


BOOL IsDirectorySep(WCHAR ch) 
{
    return (ch == L'\\' || ch == L'/');
}

//C:\\Program\\123456~1
//wszRootdir为:c:\\Program
//wszShortName为：123456~1

BOOL QueryDirectoryForLongName(
					  WCHAR * wszRootDir, 
					  WCHAR * wszShortName, 
					  WCHAR *wszLongName, 
					  ULONG ulSize)
{
	UNICODE_STRING				ustrRootDir		= {0};
	UNICODE_STRING				ustrShortName	= {0};
	UNICODE_STRING				ustrLongName	= {0};
	OBJECT_ATTRIBUTES			oa				= {0};
	IO_STATUS_BLOCK				Iosb			= {0};
	NTSTATUS					ntStatus		= 0;
	HANDLE						hDirHandle		= 0;
	BYTE						*Buffer			= NULL;
	WCHAR						*wszRoot		= NULL;
	PFILE_BOTH_DIR_INFORMATION	pInfo			= NULL;

	RtlZeroMemory(&Iosb, sizeof(IO_STATUS_BLOCK));
	Iosb.Status = STATUS_NO_SUCH_FILE;

	wszRoot = ExAllocatePoolWithTag(PagedPool,
								  MAX_PATH * sizeof(WCHAR),
								  'L2S');
	if(wszRoot == NULL)
	{
		return FALSE;
	}

	RtlZeroMemory(wszRoot, MAX_PATH * sizeof(WCHAR));

	wcsncpy(wszRoot, wszRootDir, MAX_PATH);

	RtlInitUnicodeString(&ustrRootDir, wszRoot);
	RtlInitUnicodeString(&ustrShortName, wszShortName);

	if(IsRootDirecotry(wszRoot))
		RtlAppendUnicodeToString(&ustrRootDir, L"\\");

	InitializeObjectAttributes(&oa,
							   &ustrRootDir,
							   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
							   0, 
							   0);  
	
	ntStatus = ZwCreateFile(&hDirHandle,
							GENERIC_READ | SYNCHRONIZE,
							&oa,
							&Iosb,
							0, 
							FILE_ATTRIBUTE_DIRECTORY, 
							FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 
							FILE_OPEN, 
							FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT , 
							0,
							0);

	if (!NT_SUCCESS(ntStatus)) 
	{ 
		ExFreePool(wszRoot);
		return FALSE;
	}

	ExFreePool(wszRoot);

	Buffer = ExAllocatePoolWithTag(PagedPool,
						  1024,
						  'L2S');
	if(Buffer == NULL)
	{
		ZwClose(hDirHandle);
		return FALSE;
	}

	RtlZeroMemory(Buffer, 1024);

	ntStatus = ZwQueryDirectoryFile(hDirHandle,
								NULL,
								0,
								0,
								&Iosb,
								Buffer,
								1024,
								FileBothDirectoryInformation,
								TRUE,
								&ustrShortName, //传回与 ustrShortName Match的项
								TRUE);

	if (!NT_SUCCESS(ntStatus)) 
	{
		ExFreePool(Buffer);
		ZwClose(hDirHandle);
		return FALSE;
	}

	ZwClose(hDirHandle);

	pInfo = (PFILE_BOTH_DIR_INFORMATION) Buffer;
	
	if(pInfo->FileNameLength == 0)
	{
		ExFreePool(Buffer);
		return FALSE;
	}

	ustrShortName.Length  = (USHORT)pInfo->FileNameLength;
	ustrShortName.MaximumLength = (USHORT)pInfo->FileNameLength;
	ustrShortName.Buffer = pInfo->FileName;	//长名

	if(ulSize < ustrShortName.Length)
	{	
		ExFreePool(Buffer);
		return FALSE;
	}

	ustrLongName.Length = 0;
	ustrLongName.MaximumLength = (USHORT)ulSize;
	ustrLongName.Buffer = wszLongName;

	RtlCopyUnicodeString(&ustrLongName, &ustrShortName);
	ExFreePool(Buffer);
	return TRUE;
}

BOOL QueryLongName(WCHAR * wszFullPath, WCHAR * wszLongName, ULONG size)
{
	BOOL		rtn				= FALSE;
	WCHAR *		pchStart		= wszFullPath;
	WCHAR *		pchEnd			= NULL;
	WCHAR *		wszShortName	= NULL;
	
	//c:\\Program\\Files1~1-->获得Files1~1的长名
	while(*pchStart)
	{
		if(IsDirectorySep(*pchStart))
			pchEnd = pchStart;
		
		pchStart++;
	}
	//wszFullPath=c:\\Program
	//pchEnd = Files~1
	
	if(pchEnd)
	{
		*pchEnd++ = L'\0';
		//c:\\Program\\Files1~1
		//wszFullPath:c:\\Program
		//pchEnd:Files1~1
		wszShortName = pchEnd;
		rtn = QueryDirectoryForLongName(wszFullPath, wszShortName, wszLongName, size);
		*(--pchEnd) = L'\\';
		//wszFullPath=c:\\Program\\Files1~1
	}
	return rtn;
}

//先把根目录拷贝到目标目录中，剩下的找到下一级目录是否含有~，如果有，则开始转化。
//如：c:\\Progam\\a~1\\b~1\hi~1.txt
//pchStart指向目录中前一个\\,pchEnd扫描并指向目录的下一个\\，其中如果发现了~，则是短名，需要转换。
//传c:\\Program\\a~1-->c:\\Progam\\ax
//传c:\\Program\\ax\\b~1-->c:\\Program\\ax\\by
//传c:\\Program\\ax\by\\hi~1.txt-->c:\\Program\\ax\by\\hiz.txt
BOOL ConverShortToLongName(WCHAR *wszLongName, WCHAR *wszShortName, ULONG size)
{
	WCHAR			*szResult		= NULL;
	WCHAR			*pchResult		= NULL;
	WCHAR			*pchStart		= wszShortName;
	INT				Offset			= 0;
  
	szResult = ExAllocatePoolWithTag(PagedPool,
						  sizeof(WCHAR) * (MAX_PATH * 2 + 1),
						  'L2S');

	if(szResult == NULL)
	{
		return FALSE;
	}
	
	RtlZeroMemory(szResult, sizeof(WCHAR) * (MAX_PATH * 2 + 1));
	pchResult = szResult;


	//C:\\x\\-->\\??\\c:
	if (pchStart[0] && pchStart[1] == L':') 
	{
		*pchResult++ = L'\\';
		*pchResult++ = L'?';
		*pchResult++ = L'?';
		*pchResult++ = L'\\';
		*pchResult++ = *pchStart++;
		*pchResult++ = *pchStart++;
		Offset = 4;
	}
	//\\DosDevices\\c:\\xx-->\\??\\c:
	else if (_wcsnicmp(pchStart, L"\\DosDevices\\", 12) == 0)
	{
		RtlStringCbCopyW(pchResult, sizeof(WCHAR) * (MAX_PATH * 2 + 1), L"\\??\\");
		pchResult += 4;
		pchStart += 12;
		while (*pchStart && !IsDirectorySep(*pchStart))
			*pchResult++ = *pchStart++;
		Offset = 4;
	}
	//\\Device\\HarddiskVolume1\\xx-->\\Device\\HarddiskVolume1
	else if (_wcsnicmp(pchStart, L"\\Device\\HardDiskVolume", 22) == 0)
	{
		RtlStringCbCopyW(pchResult, sizeof(WCHAR) * (MAX_PATH * 2 + 1),L"\\Device\\HardDiskVolume");
		pchResult += 22;
		pchStart += 22;
		while (*pchStart && !IsDirectorySep(*pchStart))
			*pchResult++ = *pchStart++;
	}
	//\\??\\c:\\xx-->\\??\\c:
	else if (_wcsnicmp(pchStart, L"\\??\\", 4) == 0)
	{
		RtlStringCbCopyW(pchResult, sizeof(WCHAR) * (MAX_PATH * 2 + 1), L"\\??\\");
		pchResult += 4;
		pchStart += 4;

		while (*pchStart && !IsDirectorySep(*pchStart))
			*pchResult++ = *pchStart++;
	}
	else
	{
		ExFreePool(szResult);
		return FALSE;
	}

	while (IsDirectorySep(*pchStart)) 
	{
		BOOL			bShortName			= FALSE;
		WCHAR			*pchEnd				= NULL;
		WCHAR			*pchReplacePos		= NULL;

		*pchResult++ = *pchStart++;

		pchEnd = pchStart;
		pchReplacePos = pchResult;

		while (*pchEnd && !IsDirectorySep(*pchEnd))
		{
			if(*pchEnd == L'~')
			{
				bShortName = TRUE;
			}

			*pchResult++ = *pchEnd++;
		}

		*pchResult = L'\0';
  
		if(bShortName)
		{
			WCHAR  * szLong = NULL;
			
			szLong = ExAllocatePoolWithTag(PagedPool,
						  sizeof(WCHAR) * MAX_PATH,
						  'L2S');
			if(szLong)
			{
				RtlZeroMemory(szLong,  sizeof(WCHAR) * MAX_PATH);

				if(QueryLongName(szResult, szLong, sizeof(WCHAR) * MAX_PATH))
				{
					RtlStringCbCopyW(pchReplacePos, sizeof(WCHAR) * (MAX_PATH * 2 + 1), szLong);
					pchResult = pchReplacePos + wcslen(pchReplacePos);
				}

				ExFreePool(szLong);
			}
		}

		pchStart = pchEnd;
	}

	wcsncpy(wszLongName, szResult + Offset, size/sizeof(WCHAR));
	ExFreePool(szResult);
	return TRUE;
}

BOOL IsShortNamePath(WCHAR * wszFileName)
{
	WCHAR *p = wszFileName;

	while(*p != L'\0')
	{
		if(*p == L'~')
		{
			return TRUE;
		}
		p++;
	}
	
	return FALSE;
}

VOID DriverUnload(PDRIVER_OBJECT pDriverObject)
{
	DbgPrint("Goodbye!\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegPath)
{
	//dir /x看短名
	WCHAR	wszShortName[MAX_PATH] =L"\\??\\C:\\PROGRA~1\\COMMON~1\\MICROS~1\\VC\\1.txt";
	WCHAR	wszLongName[MAX_PATH] = {0};

	if(ConverShortToLongName(wszLongName, wszShortName, sizeof(wszLongName)))
	{
		DbgPrint("%ws\n", wszLongName);
	}

	pDriverObject->DriverUnload = DriverUnload;
	return STATUS_SUCCESS;
}
