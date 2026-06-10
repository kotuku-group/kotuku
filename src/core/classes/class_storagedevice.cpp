/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
StorageDevice: Queries metadata for file system volumes.

The StorageDevice class returns metadata for mounted file system volumes.  Set #Volume before initialisation to
identify the volume to query.  If the volume name cannot be resolved, initialisation will fail.

After initialisation, the read-only fields describe the resolved volume.  Values that depend on host or device support
can remain unavailable; unknown byte counts are reported as `-1`, and #DeviceID is empty if no identifier is available.
-END-

*********************************************************************************************************************/

#define PRV_FILESYSTEM
#include "../defs.h"

//********************************************************************************************************************

static ERR STORAGEDEVICE_Init(objStorageDevice *Self)
{
   kt::Log log;

   if (Self->Volume.empty()) return log.warning(ERR::FieldNotSet);

   auto vd = get_fs(Self->Volume);

   if (vd.is_virtual()) Self->DeviceFlags |= DEVICE::SOFTWARE;

   Self->BytesFree  = -1;
   Self->BytesUsed  = 0;
   Self->DeviceSize = -1;

   if (vd.GetDeviceInfo) return vd.GetDeviceInfo(Self->Volume, Self);
   else return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
BytesFree: Amount of storage space available to the current user, measured in bytes.

The available storage space reported by the host file system is indicated in this field.  A value of `-1` indicates
that the amount could not be determined for the selected volume.

-FIELD-
BytesUsed: Amount of storage space in use, measured in bytes.

The amount of storage space used on the volume is indicated in this field.  A value of `0` can indicate that usage
could not be determined for the selected volume.

Please note that storage usage is typically measured in terms of blocks.  For instance, a block size of 512 bytes will
mean that this field is a multiple of 512.  Two files of 1 byte each on such a file system would take up 1024 bytes of
space and not 2.

-FIELD-
DeviceID: Unique device identifier for the mounted volume, when available.

If a volume exposes a unique device identifier such as a factory serial number, it will be readable from this field.
The field is empty when the selected volume or file system driver does not provide an identifier.

-FIELD-
DeviceSize: Total storage capacity of the resolved volume, measured in bytes.

This field indicates the total byte capacity reported by the host file system for the selected volume.  It does not
reflect the remaining writable space; use #BytesFree for that value.  A value of `-1` indicates that capacity could not
be determined.

-FIELD-
DeviceFlags: These read-only flags identify the type of device and its features.
Lookup: DEVICE

-FIELD-
Volume: The volume name of the device to query.

Set the Volume field prior to initialisation so that the object can query the selected volume.  The standard volume
string format is `name:`, but omitting the colon or defining complete file system paths when writing this field is also
acceptable.  Any characters following a colon are stripped automatically, and the stored value is normalised to include
the trailing colon.
-END-

*********************************************************************************************************************/

static ERR SET_Volume(objStorageDevice *Self, const std::string_view &Value)
{
   kt::Log log;

   if (Self->initialised()) return log.warning(ERR::Immutable);

   if (not Value.empty()) {
      auto len = Value.find(':');
      if (len IS std::string_view::npos) len = Value.size();

      Self->Volume.assign(Value.substr(0, len));
      Self->Volume.push_back(':');
   }

   return ERR::Okay;
}

//********************************************************************************************************************

#include "class_storagedevice_def.cpp"

static const FieldArray clFields[] = {
   { "DeviceFlags", FDF_INT64|FDF_R, nullptr, nullptr, &clStorageDeviceDeviceFlags },
   { "DeviceSize",  FDF_INT64|FDF_R },
   { "BytesFree",   FDF_INT64|FDF_R },
   { "BytesUsed",   FDF_INT64|FDF_R },
   { "DeviceID",    FDF_CPPSTRING|FDF_R|FDF_PURE },
   { "Volume",      FDF_CPPSTRING|FDF_RI|FDF_PURE, nullptr, SET_Volume },
    END_FIELD
};

//********************************************************************************************************************

extern ERR add_storage_class(void)
{
   glStorageClass = extMetaClass::create::global(
      fl::BaseClassID(CLASSID::STORAGEDEVICE),
      fl::ClassVersion(VER_STORAGEDEVICE),
      fl::Name("StorageDevice"),
      fl::Category(CCF::SYSTEM),
      fl::Actions(clStorageDeviceActions),
      fl::Fields(clFields),
      fl::Size(sizeof(objStorageDevice)),
      fl::Path("modules:core"));

   return glStorageClass ? ERR::Okay : ERR::AddClass;
}
