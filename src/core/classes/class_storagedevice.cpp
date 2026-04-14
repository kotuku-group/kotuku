/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
StorageDevice: Queries the meta data of file system volumes.

The StorageDevice class returns the meta data of file system volumes.  A reference to an existing volume is required
in the #Volume field in order to make a successful analysis.  If the volume name cannot be resolved,
initialisation will fail.

Following initialisation, all meta fields describing the volume are readable for further information.
-END-

*********************************************************************************************************************/

#define PRV_FILESYSTEM
#include "../defs.h"

static ERR STORAGE_Free(extStorageDevice *);
static ERR STORAGE_Init(extStorageDevice *);

//********************************************************************************************************************

static ERR STORAGE_Free(extStorageDevice *Self)
{
   if (Self->Volume) { FreeResource(Self->Volume); Self->Volume = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR STORAGE_Init(extStorageDevice *Self)
{
   pf::Log log;

   if (!Self->Volume) return log.warning(ERR::FieldNotSet);

   const virtual_drive *vd = get_fs(Self->Volume);

   if (vd->is_virtual()) Self->DeviceFlags |= DEVICE::SOFTWARE;

   Self->BytesFree  = -1;
   Self->BytesUsed  = 0;
   Self->DeviceSize = -1;

   if (vd->GetDeviceInfo) return vd->GetDeviceInfo(Self->Volume, Self);
   else return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
BytesFree: Total amount of storage space that is available, measured in bytes.

-FIELD-
BytesUsed: Total amount of storage space in use.

The total amount of storage space used by the device is indicated in this field.

Please note that storage usage is typically measured in terms of blocks.  For instance a block size of 512 bytes will
mean that this field will be a multiple of 512.  Two files of 1 byte each on such a file system would take up 1024
bytes of space and not 2.

-FIELD-
DeviceID: A unique ID for the mounted device (platform dependent, `NULL` if unavailable).

If a volume expresses a unique device identifier such as a factory serial number, it will be readable from this field.

*********************************************************************************************************************/

static ERR GET_DeviceID(extStorageDevice *Self, STRING *Value)
{
   if (Self->DeviceID) {
      *Value = Self->DeviceID;
      return ERR::Okay;
   }
   else {
      *Value = nullptr;
      return ERR::FieldNotSet;
   }
}

/*********************************************************************************************************************
-FIELD-
DeviceSize: The storage size of the device in bytes, without accounting for the file system format.

This field indicates the storage size of the device.  It does not reflect the available space determined by
the device's file system, which will typically be measurably smaller than this value.

-FIELD-
DeviceFlags: These read-only flags identify the type of device and its features.
Lookup: DEVICE

-FIELD-
Volume: The volume name of the device to query.

Set the Volume field prior to initialisation for that volume to be queried by the object.  The standard volume string
format is `name:`, but omitting the colon or defining complete file system paths when writing this field is also
acceptable.  Any characters following a colon will be stripped automatically with no ongoing functional impact.
-END-

*********************************************************************************************************************/

static ERR GET_Volume(extStorageDevice *Self, STRING *Value)
{
   if (Self->Volume) {
      *Value = Self->Volume;
      return ERR::Okay;
   }
   else {
      *Value = nullptr;
      return ERR::FieldNotSet;
   }
}

static ERR SET_Volume(extStorageDevice *Self, CSTRING Value)
{
   pf::Log log;

   if (Self->initialised()) return log.warning(ERR::Immutable);

   if ((Value) and (*Value)) {
      int len;
      for (len=0; (Value[len]) and (Value[len] != ':'); len++);

      if (Self->Volume) FreeResource(Self->Volume);

      if (AllocMemory(len+2, MEM::STRING|MEM::NO_CLEAR, (APTR *)&Self->Volume, nullptr) IS ERR::Okay) {
         copymem(Value, Self->Volume, len);
         Self->Volume[len] = ':';
         Self->Volume[len+1] = 0;
         return ERR::Okay;
      }
      else return log.warning(ERR::AllocMemory);
   }
   else return ERR::Okay;
}

//********************************************************************************************************************

static const FieldDef clDeviceFlags[] = {
   { "CompactDisc",  DEVICE::COMPACT_DISC },
   { "HardDisk",     DEVICE::HARD_DISK },
   { "FloppyDisk",   DEVICE::FLOPPY_DISK },
   { "Read",         DEVICE::READ },
   { "Write",        DEVICE::WRITE },
   { "Removable",    DEVICE::REMOVABLE },
   { "Software",     DEVICE::SOFTWARE },
   { "Network",      DEVICE::NETWORK },
   { "Tape",         DEVICE::TAPE },
   { "Printer",      DEVICE::PRINTER },
   { "Scanner",      DEVICE::SCANNER },
   { "Temporary",    DEVICE::TEMPORARY },
   { "Memory",       DEVICE::MEMORY },
   { "Modem",        DEVICE::MODEM },
   { "USB",          DEVICE::USB },
   { nullptr, 0 }
};

static const FieldArray clFields[] = {
   { "DeviceFlags", FDF_INT64|FDF_R, nullptr, nullptr, &clDeviceFlags },
   { "DeviceSize",  FDF_INT64|FDF_R },
   { "BytesFree",   FDF_INT64|FDF_R },
   { "BytesUsed",   FDF_INT64|FDF_R },
   // Virtual fields
   { "DeviceID",    FDF_STRING|FDF_R, GET_DeviceID },
   { "Volume",      FDF_STRING|FDF_RI, GET_Volume, SET_Volume },
    END_FIELD
};

static const ActionArray clActions[] = {
   { AC::Free, STORAGE_Free },
   { AC::Init, STORAGE_Init },
   { AC::NIL, nullptr }
};

//********************************************************************************************************************

extern ERR add_storage_class(void)
{
   glStorageClass = extMetaClass::create::global(
      fl::BaseClassID(CLASSID::STORAGEDEVICE),
      fl::ClassVersion(VER_STORAGEDEVICE),
      fl::Name("StorageDevice"),
      fl::Category(CCF::SYSTEM),
      fl::Actions(clActions),
      fl::Fields(clFields),
      fl::Size(sizeof(extStorageDevice)),
      fl::Path("modules:core"));

   return glStorageClass ? ERR::Okay : ERR::AddClass;
}
