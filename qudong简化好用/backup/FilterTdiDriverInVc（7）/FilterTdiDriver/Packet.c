
#include <ndis.h>
#include <tdikrnl.h>
#include <ntddk.h>
#include <stdlib.h>
#include <stdio.h>
#include "packet.h"

static 	ULONG  ProcessNameOffset=0;

NTSTATUS
DriverEntry(
	IN	PDRIVER_OBJECT		DriverObject,
	IN	PUNICODE_STRING		RegistryPath
)
{

	NTSTATUS	status	= 0;
    ULONG		i;
	PEPROCESS curproc;

	//�õ�����ƫ�ƺ�
	curproc = PsGetCurrentProcess();   
    for( i = 0; i < 3*PAGE_SIZE; i++ )
	{
        if( !strncmp( "System", (PCHAR) curproc + i, strlen("System") ))
		{
            ProcessNameOffset = i;
	        break;
		}
	}

	DBGPRINT("DriverEntry Loading...\n");

	//ж������
	DriverObject->DriverUnload = TDIH_Unload;

	//�ļ��б��ʼ��
    KeInitializeSpinLock(&FileObjectLock);
	InitializeListHead(&FileObjectList);
   
	status = TCPFilter_Attach( DriverObject, RegistryPath );

    status = UDPFilter_Attach( DriverObject, RegistryPath );

	//������Ϣ���ģ��
	DebugPrintInit("FilterTdiDriver");

   for (i=0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
   {
      DriverObject->MajorFunction[i] = TDIH_DefaultDispatch;
   }

   return status;
}

VOID 
TDIH_Unload(
	IN PDRIVER_OBJECT		DriverObject
)
{
   PDEVICE_OBJECT             pDeviceObject;
   PDEVICE_OBJECT             pNextDeviceObject;
   PTDIH_DeviceExtension pTDIH_DeviceExtension;

   pDeviceObject = DriverObject->DeviceObject;

   	//ж����Ϣ���ģ��
	DebugPrintClose();

   while( pDeviceObject != NULL )
   {
      pNextDeviceObject = pDeviceObject->NextDevice;

      pTDIH_DeviceExtension = (PTDIH_DeviceExtension )pDeviceObject->DeviceExtension;

      if( pTDIH_DeviceExtension->NodeType == TDIH_NODE_TYPE_TCP_FILTER_DEVICE )
      {
         TCPFilter_Detach( pDeviceObject );   
      }
      else if( pTDIH_DeviceExtension->NodeType == TDIH_NODE_TYPE_UDP_FILTER_DEVICE )
      {
         UDPFilter_Detach( pDeviceObject );   
      }

      pDeviceObject = pNextDeviceObject;
   }

   //ɾ���¼���������
   ClearEvents();
}

NTSTATUS
TDIH_DefaultDispatch(
    IN PDEVICE_OBJECT		DeviceObject,
    IN PIRP					Irp
)
{
	NTSTATUS                RC = STATUS_SUCCESS;
   PIO_STACK_LOCATION      IrpSp = NULL;
   PTDIH_DeviceExtension   pTDIH_DeviceExtension;
   PDEVICE_OBJECT          pLowerDeviceObject = NULL;
	
   pTDIH_DeviceExtension = (PTDIH_DeviceExtension )(DeviceObject->DeviceExtension);

   IrpSp = IoGetCurrentIrpStackLocation(Irp);
   ASSERT(IrpSp);

   try
   {
      pLowerDeviceObject = pTDIH_DeviceExtension->LowerDeviceObject;

      if (Irp->CurrentLocation == 1)
      {
         ULONG ReturnedInformation = 0;

         RC = STATUS_INVALID_DEVICE_REQUEST;
         Irp->IoStatus.Status = RC;
         Irp->IoStatus.Information = ReturnedInformation;
         IoCompleteRequest(Irp, IO_NO_INCREMENT);

         return( RC );
      }

      IoCopyCurrentIrpStackLocationToNext( Irp );
     
      IoSetCompletionRoutine(
         Irp,
         DefaultDispatchCompletion,
         pTDIH_DeviceExtension,
         TRUE,
         TRUE,
         TRUE
         );
      
	  UTIL_IncrementLargeInteger(
         pTDIH_DeviceExtension->OutstandingIoRequests,
         (unsigned long)1,
         &(pTDIH_DeviceExtension->IoRequestsSpinLock)
         );

      KeClearEvent(&(pTDIH_DeviceExtension->IoInProgressEvent));

      RC = IoCallDriver(pLowerDeviceObject, Irp);
   }

   finally
   {
   }

   return(RC);

}

NTSTATUS
DefaultDispatchCompletion(
	IN	PDEVICE_OBJECT	pDeviceObject,
	IN	PIRP			Irp,
	IN	PVOID			Context
)
{
   PTDIH_DeviceExtension pTDIH_DeviceExtension;
   BOOLEAN           CanDetachProceed = FALSE;
   PDEVICE_OBJECT    pAssociatedDeviceObject = NULL;
   PIO_STACK_LOCATION		IrpStack;

   ////////////////////////////////////////////////
   //ͨ�Ų���
   PTDI_ADDRESS_IP pIPAddress;
   PTRANSPORT_ADDRESS pTransAddr;
   PTDI_ADDRESS_INFO  pTdi_Address_Info;
   ULONG len;
   USHORT port;

   pTDIH_DeviceExtension = (PTDIH_DeviceExtension )(Context);
   ASSERT( pTDIH_DeviceExtension );

   if (Irp->PendingReturned)
   {
      IoMarkIrpPending(Irp);
   }

   pAssociatedDeviceObject = pTDIH_DeviceExtension->pFilterDeviceObject;

   if (pAssociatedDeviceObject != pDeviceObject)
   {
      KdPrint(( "TDIH_DefaultCompletion: Invalid Device Object Pointer\n" ));
      return(STATUS_SUCCESS);
   }

   IrpStack = IoGetCurrentIrpStackLocation(Irp);

   if(IrpStack->MajorFunction==IRP_MJ_INTERNAL_DEVICE_CONTROL
	   &&IrpStack->MinorFunction==TDI_CONNECT)
   {
	   len=sizeof(TDI_ADDRESS_INFO);
	   pTdi_Address_Info=(PTDI_ADDRESS_INFO)ExAllocatePool(NonPagedPool,len);
	   LLT_QueryAddressInfo(IrpStack->FileObject,(PVOID)pTdi_Address_Info,&len);
	   
	   DbgPrint("THE LENGTH OF TDI_ADDRESS_INFO IS :%d",len);
	  
	   pTransAddr=&pTdi_Address_Info->Address;
               
	   if(pTransAddr->Address[0 ].AddressType==TDI_ADDRESS_TYPE_IP)
	   {				       
		   pIPAddress = (PTDI_ADDRESS_IP )(PUCHAR )&pTransAddr->Address[0 ].Address; 
		   DbgPrint("THE LOCAL PORT IS: %d",pIPAddress->sin_port);
		   port=LLT_htons(pIPAddress->sin_port);
           DbgPrint("THE REAL LOCAL PORT IS: %d",port);
	   }
                   		
	   ExFreePool(pTdi_Address_Info);
   }

   UTIL_DecrementLargeInteger(
      pTDIH_DeviceExtension->OutstandingIoRequests,
      (unsigned long)1,
      &(pTDIH_DeviceExtension->IoRequestsSpinLock)
      );

   UTIL_IsLargeIntegerZero(
      CanDetachProceed,
      pTDIH_DeviceExtension->OutstandingIoRequests,
      &(pTDIH_DeviceExtension->IoRequestsSpinLock)
      );

   if (CanDetachProceed)
   {
      KeSetEvent(&(pTDIH_DeviceExtension->IoInProgressEvent), IO_NO_INCREMENT, FALSE);
   }

   return(STATUS_SUCCESS);

}



VOID
TDIH_Create(
   PTDIH_DeviceExtension   pTDIH_DeviceExtension,
   PIRP                    Irp,
   PIO_STACK_LOCATION      IrpStack
   )
{
  
}

VOID
TDIH_CleanUp(
   PTDIH_DeviceExtension   pTDIH_DeviceExtension,
   PIRP                    Irp,
   PIO_STACK_LOCATION      IrpStack
   )
{

}

PFILEOBJECT_NODE
TDIH_GetFileObjectFromList(PFILE_OBJECT pFileObject)
{
	return NULL;
}

NTSTATUS
TCPFilter_Attach(
	IN PDRIVER_OBJECT	DriverObject,
	IN PUNICODE_STRING	RegistryPath
)
{
	NTSTATUS                   status;
   UNICODE_STRING             uniNtNameString;
   PTDIH_DeviceExtension pTDIH_DeviceExtension;
   PDEVICE_OBJECT             pFilterDeviceObject = NULL;
   PDEVICE_OBJECT             pTargetDeviceObject = NULL;
   PFILE_OBJECT               pTargetFileObject = NULL;
   PDEVICE_OBJECT             pLowerDeviceObject = NULL;

   ASSERT( KeGetCurrentIrql() <= DISPATCH_LEVEL );

   RtlInitUnicodeString( &uniNtNameString, DD_TCP_DEVICE_NAME );

   ASSERT( KeGetCurrentIrql() == PASSIVE_LEVEL );
   status = IoGetDeviceObjectPointer(
               &uniNtNameString,
               FILE_READ_ATTRIBUTES,
               &pTargetFileObject,   
               &pTargetDeviceObject
               );

   if( !NT_SUCCESS(status) )
   {
      pTargetFileObject = NULL;
      pTargetDeviceObject = NULL;

      return( status );
   }

   ASSERT( KeGetCurrentIrql() <= DISPATCH_LEVEL );
   RtlInitUnicodeString( &uniNtNameString, TDIH_TCP_DEVICE_NAME );

   ASSERT( KeGetCurrentIrql() == PASSIVE_LEVEL );
   status = IoCreateDevice(
               DriverObject,
               sizeof( TDIH_DeviceExtension ),
               &uniNtNameString,
               pTargetDeviceObject->DeviceType,
               pTargetDeviceObject->Characteristics,
               FALSE,                
               &pFilterDeviceObject
               );

   if( !NT_SUCCESS(status) )
   {
      KdPrint(("PCATDIH: Couldn't create the TCP Filter Device Object\n"));

      ASSERT( KeGetCurrentIrql() == PASSIVE_LEVEL );
      ObDereferenceObject( pTargetFileObject );

      pTargetFileObject = NULL;
      pTargetDeviceObject = NULL;

      return( status );
   }

   pTDIH_DeviceExtension = (PTDIH_DeviceExtension )( pFilterDeviceObject->DeviceExtension );

   TCPFilter_InitDeviceExtension(
		IN	pTDIH_DeviceExtension,
		IN	pFilterDeviceObject,
		IN	pTargetDeviceObject,
		IN	pTargetFileObject,
		IN	pLowerDeviceObject
		);

   KeInitializeSpinLock(&(pTDIH_DeviceExtension->IoRequestsSpinLock));

   KeInitializeEvent(&(pTDIH_DeviceExtension->IoInProgressEvent), NotificationEvent, FALSE);

   pLowerDeviceObject = IoAttachDeviceToDeviceStack(
                           pFilterDeviceObject, 
                           pTargetDeviceObject  
                           );

   if( !pLowerDeviceObject )
   {
      KdPrint(("PCATDIH: Couldn't attach to TCP Device Object\n"));

      IoDeleteDevice( pFilterDeviceObject );

      pFilterDeviceObject = NULL;

      ASSERT( KeGetCurrentIrql() == PASSIVE_LEVEL );
      ObDereferenceObject( pTargetFileObject );

      pTargetFileObject = NULL;
      pTargetDeviceObject = NULL;

      return( status );
   }

   // Initialize the TargetDeviceObject field in the extension.
   pTDIH_DeviceExtension->TargetDeviceObject = pTargetDeviceObject;
   pTDIH_DeviceExtension->TargetFileObject = pTargetFileObject;

   pTDIH_DeviceExtension->LowerDeviceObject = pLowerDeviceObject;

   pTDIH_DeviceExtension->DeviceExtensionFlags |= TDIH_DEV_EXT_ATTACHED;

   if( pLowerDeviceObject != pTargetDeviceObject )
   {
      KdPrint(("PCATDIH: TCP Already Filtered!\n"));
   }

  
   pFilterDeviceObject->Flags |= pTargetDeviceObject->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO);

   return status;
	
}

NTSTATUS
UDPFilter_Attach(
	IN PDRIVER_OBJECT	DriverObject,
	IN PUNICODE_STRING	RegistryPath
)
{
	 NTSTATUS                   status;
   UNICODE_STRING             uniNtNameString;
   PTDIH_DeviceExtension pTDIH_DeviceExtension;
   PDEVICE_OBJECT             pFilterDeviceObject = NULL;
   PDEVICE_OBJECT             pTargetDeviceObject = NULL;
   PFILE_OBJECT               pTargetFileObject = NULL;
   PDEVICE_OBJECT             pLowerDeviceObject = NULL;

   KdPrint(("PCATDIH: UDPFilter_Attach Entry...\n"));

 
   ASSERT( KeGetCurrentIrql() <= DISPATCH_LEVEL );
   RtlInitUnicodeString( &uniNtNameString, DD_UDP_DEVICE_NAME );

   ASSERT( KeGetCurrentIrql() == PASSIVE_LEVEL );
   status = IoGetDeviceObjectPointer(
               &uniNtNameString,
               FILE_READ_ATTRIBUTES,
               &pTargetFileObject,   // Call ObDereferenceObject Eventually...
               &pTargetDeviceObject
               );

   if( !NT_SUCCESS(status) )
   {
      KdPrint(("PCATDIH: Couldn't get the UDP Device Object\n"));

      pTargetFileObject = NULL;
      pTargetDeviceObject = NULL;

      return( status );
   }

  
   ASSERT( KeGetCurrentIrql() <= DISPATCH_LEVEL );
   RtlInitUnicodeString( &uniNtNameString, TDIH_UDP_DEVICE_NAME );

  
   ASSERT( KeGetCurrentIrql() == PASSIVE_LEVEL );
   status = IoCreateDevice(
               DriverObject,
               sizeof( TDIH_DeviceExtension ),
               &uniNtNameString,
               pTargetDeviceObject->DeviceType,
               pTargetDeviceObject->Characteristics,
               FALSE,                 // This isn't an exclusive device
               &pFilterDeviceObject
               );

   if( !NT_SUCCESS(status) )
   {
      KdPrint(("PCATDIH: Couldn't create the UDP Filter Device Object\n"));

//      ASSERT( KeGetCurrentIrql() == PASSIVE_LEVEL );
      ObDereferenceObject( pTargetFileObject );

      pTargetFileObject = NULL;
      pTargetDeviceObject = NULL;

      return( status );
   }

   
   pTDIH_DeviceExtension = (PTDIH_DeviceExtension )( pFilterDeviceObject->DeviceExtension );

   UDPFilter_InitDeviceExtension(
		IN	pTDIH_DeviceExtension,
		IN	pFilterDeviceObject,
		IN	pTargetDeviceObject,
		IN	pTargetFileObject,
		IN	pLowerDeviceObject
		);

   // Initialize the Executive spin lock for this device extension.
   KeInitializeSpinLock(&(pTDIH_DeviceExtension->IoRequestsSpinLock));

   // Initialize the event object used to denote I/O in progress.
   // When set, the event signals that no I/O is currently in progress.
   KeInitializeEvent(&(pTDIH_DeviceExtension->IoInProgressEvent), NotificationEvent, FALSE);

  
   pLowerDeviceObject = IoAttachDeviceToDeviceStack(
                           pFilterDeviceObject, // Source Device (Our Device)
                           pTargetDeviceObject  // Target Device
                           );

   if( !pLowerDeviceObject )
   {
      KdPrint(("PCATDIH: Couldn't attach to UDP Device Object\n"));

      IoDeleteDevice( pFilterDeviceObject );

      pFilterDeviceObject = NULL;

      ASSERT( KeGetCurrentIrql() == PASSIVE_LEVEL );
      ObDereferenceObject( pTargetFileObject );

      pTargetFileObject = NULL;
      pTargetDeviceObject = NULL;

      return( status );
   }

   // Initialize the TargetDeviceObject field in the extension.
   pTDIH_DeviceExtension->TargetDeviceObject = pTargetDeviceObject;
   pTDIH_DeviceExtension->TargetFileObject = pTargetFileObject;

   pTDIH_DeviceExtension->LowerDeviceObject = pLowerDeviceObject;

   pTDIH_DeviceExtension->DeviceExtensionFlags |= TDIH_DEV_EXT_ATTACHED;

   if( pLowerDeviceObject != pTargetDeviceObject )
   {
      KdPrint(("PCATDIH: UDP Already Filtered!\n"));
   }

   pFilterDeviceObject->Flags |= pTargetDeviceObject->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO);

   return status;	
}


NTSTATUS
TCPFilter_InitDeviceExtension(
	IN	PTDIH_DeviceExtension	pTDIH_DeviceExtension,
	IN	PDEVICE_OBJECT			pFilterDeviceObject,
	IN	PDEVICE_OBJECT			pTargetDeviceObject,
	IN	PFILE_OBJECT			pTargetFileObject,
	IN	PDEVICE_OBJECT			pLowerDeviceObject
)
{
	NdisZeroMemory( pTDIH_DeviceExtension, sizeof( TDIH_DeviceExtension ) );
	pTDIH_DeviceExtension->NodeType	= TDIH_NODE_TYPE_TCP_FILTER_DEVICE;
	pTDIH_DeviceExtension->NodeSize	= sizeof( TDIH_DeviceExtension );
	pTDIH_DeviceExtension->pFilterDeviceObject = pFilterDeviceObject;
	KeInitializeSpinLock(&(pTDIH_DeviceExtension->IoRequestsSpinLock));
	KeInitializeEvent(&(pTDIH_DeviceExtension->IoInProgressEvent)
		, NotificationEvent, FALSE);
	pTDIH_DeviceExtension->TargetDeviceObject	= pTargetDeviceObject;
	pTDIH_DeviceExtension->TargetFileObject		= pTargetFileObject;
	pTDIH_DeviceExtension->LowerDeviceObject	= pLowerDeviceObject;
	pTDIH_DeviceExtension->OutstandingIoRequests = 0;
	pTDIH_DeviceExtension->DeviceExtensionFlags |= TDIH_DEV_EXT_ATTACHED;
	return( STATUS_SUCCESS );
}

NTSTATUS
UDPFilter_InitDeviceExtension(
	IN	PTDIH_DeviceExtension	pTDIH_DeviceExtension,
	IN	PDEVICE_OBJECT			pFilterDeviceObject,
	IN	PDEVICE_OBJECT			pTargetDeviceObject,
	IN	PFILE_OBJECT			pTargetFileObject,
	IN	PDEVICE_OBJECT			pLowerDeviceObject
)
{
	NdisZeroMemory( pTDIH_DeviceExtension, sizeof( TDIH_DeviceExtension ) );
	pTDIH_DeviceExtension->NodeType	= TDIH_NODE_TYPE_UDP_FILTER_DEVICE;
	pTDIH_DeviceExtension->NodeSize	= sizeof( TDIH_DeviceExtension );
	pTDIH_DeviceExtension->pFilterDeviceObject = pFilterDeviceObject;
	KeInitializeSpinLock(&(pTDIH_DeviceExtension->IoRequestsSpinLock));
	KeInitializeEvent(&(pTDIH_DeviceExtension->IoInProgressEvent)
		, NotificationEvent, FALSE);
	pTDIH_DeviceExtension->TargetDeviceObject	= pTargetDeviceObject;
	pTDIH_DeviceExtension->TargetFileObject		= pTargetFileObject;
	pTDIH_DeviceExtension->LowerDeviceObject	= pLowerDeviceObject;
	pTDIH_DeviceExtension->OutstandingIoRequests = 0;
	pTDIH_DeviceExtension->DeviceExtensionFlags |= TDIH_DEV_EXT_ATTACHED;
	return( STATUS_SUCCESS );
}

VOID
TCPFilter_Detach(
	IN	PDEVICE_OBJECT pDeviceObject
)
{
	PTDIH_DeviceExtension pTDIH_DeviceExtension;
   BOOLEAN		NoRequestsOutstanding = FALSE;

   pTDIH_DeviceExtension = (PTDIH_DeviceExtension )pDeviceObject->DeviceExtension;

   ASSERT( pTDIH_DeviceExtension );

   try
   {
      try
      {

         while (TRUE)
         {
            UTIL_IsLargeIntegerZero(
               NoRequestsOutstanding,
               pTDIH_DeviceExtension->OutstandingIoRequests,
               &(pTDIH_DeviceExtension->IoRequestsSpinLock)
               );

			if( !NoRequestsOutstanding )
            {
				 KeWaitForSingleObject(
                 (void *)(&(pTDIH_DeviceExtension->IoInProgressEvent)),
                 Executive, KernelMode, FALSE, NULL
                 );

			}
            else
            {
				break;
			}
		}

		if( pTDIH_DeviceExtension->DeviceExtensionFlags & TDIH_DEV_EXT_ATTACHED)
        {
            IoDetachDevice( pTDIH_DeviceExtension->TargetDeviceObject );

            pTDIH_DeviceExtension->DeviceExtensionFlags &= ~(TDIH_DEV_EXT_ATTACHED);
		}

	    pTDIH_DeviceExtension->NodeType = 0;
	    pTDIH_DeviceExtension->NodeSize = 0;

        if( pTDIH_DeviceExtension->TargetFileObject )
        {
           ObDereferenceObject( pTDIH_DeviceExtension->TargetFileObject );
        }

        pTDIH_DeviceExtension->TargetFileObject = NULL;

		IoDeleteDevice( pDeviceObject );

        KdPrint(("PCATDIH: TCPFilter_Detach Finished\n"));
	}
    except (EXCEPTION_EXECUTE_HANDLER)
    {
			
	}
  }
  finally
  {
   
  }

  return;


}

VOID
UDPFilter_Detach(
	IN	PDEVICE_OBJECT pDeviceObject
)
{
	PTDIH_DeviceExtension pTDIH_DeviceExtension;
   BOOLEAN		NoRequestsOutstanding = FALSE;

   pTDIH_DeviceExtension = (PTDIH_DeviceExtension )pDeviceObject->DeviceExtension;

   ASSERT( pTDIH_DeviceExtension );

   try
   {
      try
      {
         // We will wait until all IRP-based I/O requests have been completed.

         while (TRUE)
         {
				// Check if there are requests outstanding
            UTIL_IsLargeIntegerZero(
               NoRequestsOutstanding,
               pTDIH_DeviceExtension->OutstandingIoRequests,
               &(pTDIH_DeviceExtension->IoRequestsSpinLock)
               );

			if( !NoRequestsOutstanding )
            {
					// Drop the resource and go to sleep.

					// Worst case, we will allow a few new I/O requests to slip in ...
					KeWaitForSingleObject(
                  (void *)(&(pTDIH_DeviceExtension->IoInProgressEvent)),
                  Executive, KernelMode, FALSE, NULL
                  );

			}
            else
            {
				break;
			}
		}

			// Detach if attached.
		if( pTDIH_DeviceExtension->DeviceExtensionFlags & TDIH_DEV_EXT_ATTACHED)
        {
            IoDetachDevice( pTDIH_DeviceExtension->TargetDeviceObject );

            pTDIH_DeviceExtension->DeviceExtensionFlags &= ~(TDIH_DEV_EXT_ATTACHED);
		}

			// Delete our device object. But first, take care of the device extension.
	    pTDIH_DeviceExtension->NodeType = 0;
	    pTDIH_DeviceExtension->NodeSize = 0;

        if( pTDIH_DeviceExtension->TargetFileObject )
        {
            ObDereferenceObject( pTDIH_DeviceExtension->TargetFileObject );
        }

        pTDIH_DeviceExtension->TargetFileObject = NULL;

		IoDeleteDevice( pDeviceObject );

        KdPrint(("PCATDIH: UDPFilter_Detach Finished\n"));
	}
    except (EXCEPTION_EXECUTE_HANDLER)
    {
		
	}

   }
   finally
   {
      
   }

   return;

}
//��������ڵ�
void DebugPrintMsg(PEVENT event)
{
	ULONG TimeLen;
	ULONG EventDataLen;

	ULONG ProcessIdLen;
	ULONG ProcessNameLen;
	ULONG addr1Len;
	ULONG addr2Len;
	ULONG addr3Len;
	ULONG addr4Len;
	ULONG OperationLen;
	ULONG PortLen;
	ULONG ResultLen;

	ULONG len;

	LARGE_INTEGER Now,NowLocal;
	TIME_FIELDS NowTF;

	PDEBUGPRINT_EVENT pEvent;
	PUCHAR buffer;

	if(DebugPrintStarted==FALSE || ExitNow==TRUE) return;

	if(event==NULL)return;

	KeQuerySystemTime(&Now);
	RtlTimeToTimeFields(&Now,&NowTF);

	//�õ��¼����峤��
	TimeLen=sizeof(TIME_FIELDS);

	ProcessIdLen=ANSIstrlen(event->ProcessID)+1;
	ProcessNameLen=ANSIstrlen(event->ProcessName)+1;
	addr1Len=ANSIstrlen(event->addr1)+1;
	addr2Len=ANSIstrlen(event->addr2)+1;
	addr3Len=ANSIstrlen(event->addr3)+1;
	addr4Len=ANSIstrlen(event->addr4)+1;
	OperationLen=ANSIstrlen(event->Operation)+1;
	PortLen=ANSIstrlen(event->port)+1;	
	ResultLen=ANSIstrlen(event->SuccOrFail)+1;

	EventDataLen=TimeLen+ProcessIdLen+ProcessNameLen+addr1Len+addr2Len+addr3Len+addr4Len+OperationLen+PortLen+ResultLen;
	len=sizeof(LIST_ENTRY)+sizeof(ULONG)+EventDataLen;

	//�����¼�������
	pEvent=(PDEBUGPRINT_EVENT)ExAllocatePool(NonPagedPool,len);
	if(pEvent!=NULL)
	{
		buffer=(PUCHAR)pEvent->EventData;
		RtlCopyMemory(buffer,&NowTF,TimeLen);
		buffer+=TimeLen;
		RtlCopyMemory(buffer,event->ProcessID,ProcessIdLen);
		buffer+=ProcessIdLen;
		RtlCopyMemory(buffer,event->ProcessName,ProcessNameLen);
		buffer+=ProcessNameLen;
		RtlCopyMemory(buffer,event->addr1,addr1Len);
		buffer+=addr1Len;
		RtlCopyMemory(buffer,event->addr2,addr2Len);
		buffer+=addr2Len;
		RtlCopyMemory(buffer,event->addr3,addr3Len);
		buffer+=addr3Len;
		RtlCopyMemory(buffer,event->addr4,addr4Len);
		buffer+=addr4Len;
        RtlCopyMemory(buffer,event->Operation,OperationLen);
		buffer+=OperationLen;
		RtlCopyMemory(buffer,event->port,PortLen);
		buffer+=PortLen;
		RtlCopyMemory(buffer,event->SuccOrFail,ResultLen);
		
		pEvent->Len=EventDataLen;
		ExInterlockedInsertTailList(&EventList,&pEvent->ListEntry,&EventListLock);
	}
}
	

void DebugPrintInit(char* DriverName)
{
	//��ʼ��ȫ�ֱ���
	HANDLE ThreadHandle;
	NTSTATUS status;

	ExitNow=FALSE;
	DebugPrintStarted=FALSE;
	ThreadObjectPointer=NULL;
	KeInitializeEvent(&ThreadEvent,SynchronizationEvent,FALSE);
	KeInitializeEvent(&ThreadExiting,SynchronizationEvent,FALSE);
	KeInitializeSpinLock(&EventListLock);
	InitializeListHead(&EventList);

	status=PsCreateSystemThread(&ThreadHandle,THREAD_ALL_ACCESS,NULL,NULL,NULL,DebugPrintSystemThread,NULL);
	
	if(!NT_SUCCESS(status))
	{
		return;
	}

	status=ObReferenceObjectByHandle(ThreadHandle,THREAD_ALL_ACCESS,NULL,KernelMode,&ThreadObjectPointer,NULL);
	if(NT_SUCCESS(status))
		ZwClose(ThreadHandle);

}

void DebugPrintClose()
{
	ExitNow=TRUE;
	KeSetEvent(&ThreadEvent,0,FALSE);
	KeWaitForSingleObject(&ThreadExiting,Executive,KernelMode,FALSE,NULL);
}

void DebugPrintSystemThread(IN PVOID Context)
{
	UNICODE_STRING DebugPrintName;
	OBJECT_ATTRIBUTES ObjectAttributes;
	IO_STATUS_BLOCK IoStatus;
	HANDLE DebugPrintDeviceHandle;
	LARGE_INTEGER OneSecondTimeout;
	PDEBUGPRINT_EVENT pEvent;
	PLIST_ENTRY pListEntry;
	ULONG EventDataLen;
	NTSTATUS status;
	//LARGE_INTEGER ByteOffset;

	KeSetPriorityThread(KeGetCurrentThread(),LOW_REALTIME_PRIORITY);

	//�����ļ�
	
	RtlInitUnicodeString(&DebugPrintName,L"\\Device\\PHDDebugPrint");
	
	InitializeObjectAttributes(&ObjectAttributes,&DebugPrintName,OBJ_CASE_INSENSITIVE,NULL,NULL);
	
	DebugPrintDeviceHandle=NULL;
	status=ZwCreateFile(&DebugPrintDeviceHandle,
		GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,
		&ObjectAttributes,
		&IoStatus,
		0,
		FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ|FILE_SHARE_WRITE,
		FILE_OPEN,
		FILE_SYNCHRONOUS_IO_NONALERT,
		NULL,
		0);
	if(!NT_SUCCESS(status)||DebugPrintDeviceHandle==NULL)
		goto exit1;

	//�߳���ѭ�����룬д�ļ�
	
	OneSecondTimeout.QuadPart=-1i64*1000000i64;

	DebugPrintStarted=TRUE;

	while(TRUE)
	{
		KeWaitForSingleObject(&ThreadEvent,Executive,KernelMode,FALSE,&OneSecondTimeout);
		
		while(TRUE)
		{
			pListEntry=ExInterlockedRemoveHeadList(&EventList,&EventListLock);
			if(pListEntry==NULL)
			{
				//DBGPRINT(("WOKAOHAIMEILIANSHANG"));
				break;
			}

			pEvent=
				CONTAINING_RECORD(pListEntry,DEBUGPRINT_EVENT,ListEntry);
			EventDataLen=pEvent->Len;

			status=ZwWriteFile(DebugPrintDeviceHandle,NULL,NULL,NULL,
				&IoStatus,pEvent->EventData,EventDataLen,NULL,NULL);

			ExFreePool(pEvent);
		}

	if(ExitNow)
		break;
	}
exit1:
	if(ThreadObjectPointer!=NULL)
	{
		ObDereferenceObject(&ThreadObjectPointer);
		ThreadObjectPointer=NULL;
	}

	/////////////////////////////////////
	//�д���ȶ
	DebugPrintStarted=FALSE;
	
	ZwClose(DebugPrintDeviceHandle);
	/////////////////////////////////////

	KeSetEvent(&ThreadExiting,0,FALSE);
	
	PsTerminateSystemThread(STATUS_SUCCESS);	
}

void ClearEvents()
{
	PLIST_ENTRY pListEntry;
	PDEBUGPRINT_EVENT pEvent;
	while(TRUE)
	{
		pListEntry=ExInterlockedRemoveHeadList(&EventList,&EventListLock);
		if(pListEntry==NULL)
			break;

	    pEvent=
			CONTAINING_RECORD(pListEntry,DEBUGPRINT_EVENT,ListEntry);
				
		ExFreePool(pEvent);
	}
}


USHORT ANSIstrlen( char* str)
{
	USHORT len = 0;
	for(;*str++!='\0';)
		len++;
	return len;
}

NTSTATUS LLT_QueryAddressInfo(PFILE_OBJECT pFileObject,PVOID pInfoBuffer,PULONG pInfoBufferSize)
{

   NTSTATUS          Status = STATUS_UNSUCCESSFUL;
   IO_STATUS_BLOCK   IoStatusBlock;
   PIRP              pIrp = NULL;
   PMDL              pMdl = NULL;
   PDEVICE_OBJECT    pDeviceObject;

   ASSERT( KeGetCurrentIrql() == PASSIVE_LEVEL );

   pDeviceObject = IoGetRelatedDeviceObject( pFileObject );

   RtlZeroMemory( pInfoBuffer, *pInfoBufferSize );

   //
   // Allocate IRP For The Query
   //
   pIrp = IoAllocateIrp( pDeviceObject->StackSize, FALSE );

   if( !pIrp )
   {
      return( STATUS_INSUFFICIENT_RESOURCES );
   }

   pMdl = LLT_AllocateAndProbeMdl(
            pInfoBuffer,      // Virtual address for MDL construction
            *pInfoBufferSize,  // size of the buffer
            FALSE,
            FALSE,
            NULL
            );

   if( !pMdl )
   {
      IoFreeIrp( pIrp );
      return( STATUS_INSUFFICIENT_RESOURCES );
   }

   TdiBuildQueryInformation(
      pIrp,
      pDeviceObject,
      pFileObject,
      LLT_SimpleTdiRequestComplete,  // Completion routine
      NULL,                            // Completion context
      TDI_QUERY_ADDRESS_INFO,
      pMdl
      );

   //
   // Submit The Query To The Transport
   //
   Status = LLT_MakeSimpleTdiRequest(
               pDeviceObject,
               pIrp
               );

   //
   // Free Allocated Resources
   //
   LLT_UnlockAndFreeMdl(pMdl);

   *pInfoBufferSize = pIrp->IoStatus.Information;

   IoFreeIrp( pIrp );

   return( Status );

}

VOID
LLT_UnlockAndFreeMdl(PMDL pMdl)
{
   MmUnlockPages( pMdl );
   IoFreeMdl( pMdl );
}

PMDL
LLT_AllocateAndProbeMdl(
   PVOID VirtualAddress,
   ULONG Length,
   BOOLEAN SecondaryBuffer,
   BOOLEAN ChargeQuota,
   PIRP Irp OPTIONAL
   )
{
   PMDL pMdl = NULL;

   pMdl = IoAllocateMdl(
            VirtualAddress,
            Length,
            SecondaryBuffer,
            ChargeQuota,
            Irp
            );

   if( !pMdl )
   {
      return( (PMDL )NULL );
   }

   try
   {
      MmProbeAndLockPages( pMdl, KernelMode, IoModifyAccess );
   }
   except( EXCEPTION_EXECUTE_HANDLER )
   {
      IoFreeMdl( pMdl );

      pMdl = NULL;

      return( NULL );
   }

   pMdl->Next = NULL;

   return( pMdl );
}

NTSTATUS
LLT_MakeSimpleTdiRequest(
   IN PDEVICE_OBJECT pDeviceObject,
   IN PIRP pIrp
   )
{
   NTSTATUS Status;
   KEVENT Event;

   ASSERT( KeGetCurrentIrql() == PASSIVE_LEVEL );

   KeInitializeEvent (&Event, NotificationEvent, FALSE);

   IoSetCompletionRoutine(
      pIrp,                         // The IRP
      LLT_SimpleTdiRequestComplete, // The completion routine
      &Event,                       // The completion context
      TRUE,                         // Invoke On Success
      TRUE,                         // Invoke On Error
      TRUE                          // Invoke On Cancel
      );

   //
   // Submit the request
   //
   Status = IoCallDriver( pDeviceObject, pIrp );

   if( !NT_SUCCESS(Status) )
   {
      KdPrint( ("IoCallDriver(pDeviceObject = %lx) returned %lx\n",pDeviceObject,Status));
   }

   if ((Status == STATUS_PENDING) || (Status == STATUS_SUCCESS))
   {
      ASSERT( KeGetCurrentIrql() == PASSIVE_LEVEL );

      Status = KeWaitForSingleObject(
                  &Event,     // Object to wait on.
                  Executive,  // Reason for waiting
                  KernelMode, // Processor mode
                  FALSE,      // Alertable
                  NULL        // Timeout
                  );

      if (!NT_SUCCESS(Status))
      {
         KdPrint(("KS_MakeSimpleTdiRequest could not wait Wait returned %lx\n",Status));
         return Status;
      }

      Status = pIrp->IoStatus.Status;
   }

   return Status;
}

NTSTATUS
LLT_SimpleTdiRequestComplete(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context
    )
{
   if( Context != NULL )
      KeSetEvent((PKEVENT )Context, 0, FALSE);

   return STATUS_MORE_PROCESSING_REQUIRED;
}

USHORT 
LLT_htons( USHORT hostshort )
{
#if BYTE_ORDER == LITTLE_ENDIAN
	PUCHAR	pBuffer;
	USHORT	nResult;

	nResult = 0;
	pBuffer = (PUCHAR )&hostshort;

	nResult = ( (pBuffer[ 0 ] << 8) & 0xFF00 )
		| ( pBuffer[ 1 ] & 0x00FF );

	return( nResult );
#else
   return( hostshort );
#endif
}
