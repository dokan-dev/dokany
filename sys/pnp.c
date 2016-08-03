/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2016 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2007 - 2011 Hiroki Asakawa <info@dokan-dev.net>

  http://dokan-dev.github.io

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 3 of the License, or (at your option) any
later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "dokan.h"

NTSTATUS
DokanDispatchPnp(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  PIO_STACK_LOCATION irpSp;
  NTSTATUS status = STATUS_SUCCESS;

  UNREFERENCED_PARAMETER(DeviceObject);

  __try {
    DDbgPrint("==> DokanPnp\n");

    irpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (irpSp->MinorFunction) {
    case IRP_MN_QUERY_REMOVE_DEVICE:
      DDbgPrint("  IRP_MN_QUERY_REMOVE_DEVICE\n");
      break;
    case IRP_MN_SURPRISE_REMOVAL:
      DDbgPrint("  IRP_MN_SURPRISE_REMOVAL\n");
      break;
    case IRP_MN_REMOVE_DEVICE:
      DDbgPrint("  IRP_MN_REMOVE_DEVICE\n");
      break;
    case IRP_MN_CANCEL_REMOVE_DEVICE:
      DDbgPrint("  IRP_MN_CANCEL_REMOVE_DEVICE\n");
      break;
    case IRP_MN_QUERY_DEVICE_RELATIONS:
      DDbgPrint("  IRP_MN_QUERY_DEVICE_RELATIONS\n");
      status = STATUS_NOT_IMPLEMENTED;
      break;
    default:
      DDbgPrint("   other minnor function %d\n", irpSp->MinorFunction);
      break;
      // IoSkipCurrentIrpStackLocation(Irp);
      // status = IoCallDriver(Vcb->TargetDeviceObject, Irp);
    }
  } __finally {
    DokanCompleteIrpRequest(Irp, status, Irp->IoStatus.Information);

    DDbgPrint("<== DokanPnp\n");
  }

  return status;
}

NTSTATUS
QueryDeviceRelations(__in PDEVICE_OBJECT DeviceObject, __in PIRP Irp) {
  NTSTATUS status = STATUS_SUCCESS;
  PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
  DEVICE_RELATION_TYPE type = irpSp->Parameters.QueryDeviceRelations.Type;
  PDEVICE_RELATIONS DeviceRelations;
  PDokanVCB vcb;

  // QueryDeviceRelations is currently not used
  // because it does not return proper informations and verifier complained
  // We keep it for futur support of pnp

  vcb = DeviceObject->DeviceExtension;

  switch (type) {
  case RemovalRelations:
    DDbgPrint("  QueryDeviceRelations - RemovalRelations\n");
    break;
  case TargetDeviceRelation:

    DDbgPrint("  QueryDeviceRelations - TargetDeviceRelation\n");

    DeviceRelations =
        (PDEVICE_RELATIONS)ExAllocatePool(sizeof(DEVICE_RELATIONS));
    if (!DeviceRelations) {
      DDbgPrint("  can't allocate DeviceRelations\n");
      return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* The PnP manager will remove this when it is done with device */
    ObReferenceObject(DeviceObject);

    DeviceRelations->Count = 1;
    DeviceRelations->Objects[0] = DeviceObject;
    Irp->IoStatus.Information = (ULONG_PTR)DeviceRelations;

    return STATUS_SUCCESS;

  case EjectionRelations:
    DDbgPrint("  QueryDeviceRelations - EjectionRelations\n");
    break;
  case BusRelations:
    DDbgPrint("  QueryDeviceRelations - BusRelations\n");
    break;
  default:
    break;
  }

  return status;
}
