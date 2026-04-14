/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
MetaClass: The MetaClass is used to manage all classes supported by the system core.

The MetaClass is responsible for managing the construction of new classes.  Each MetaClass object remains active for
the lifetime of the running program, and provides the benefit of run-time introspection so that detailed class
information can be retrieved at any time.  This enables scripting languages such as Tiri to interact with all
class types without needing active knowledge of their capabilities.

A number of additional functions are available in the Core for the purpose of class management and introspection.
~FindClass() allows all registered classes to be scanned.  ~CheckAction() verifies if a specific action is
supported by a class.  ~FindField() confirms if a specific field is declared in the class specification.

Classes are typically declared in modules, but if a class is intended to be private for an executable then declaring
it within the application code is feasible also.

Further discussion on classes and their technical aspects can be found in the Kotuku Wiki.

-END-

*********************************************************************************************************************/

#include "../defs.h"

static ERR OBJECT_GetClass(OBJECTPTR, extMetaClass **);
static ERR OBJECT_GetClassID(OBJECTPTR, CLASSID *);
static ERR OBJECT_GetName(OBJECTPTR, STRING *);
static ERR OBJECT_GetOwner(OBJECTPTR, OBJECTID *);
static ERR OBJECT_GetID(OBJECTPTR, OBJECTID *);
static ERR OBJECT_SetOwner(OBJECTPTR, OBJECTID);
static ERR OBJECT_SetName(OBJECTPTR, CSTRING);

static void field_setup(extMetaClass *);
static void sort_class_fields(extMetaClass *, std::vector<Field> &);

static void add_field(extMetaClass *, std::vector<Field> &, const FieldArray &, uint16_t &);
static void register_fields(std::vector<Field> &);
static Field * lookup_id_byclass(extMetaClass *, uint32_t, extMetaClass **);

//********************************************************************************************************************
// The MetaClass is the focal point of the OO design model.  Because classes are treated like objects, they must point
// back to a controlling class definition - this it.  See NewObject() for the management code for this data.

static ERR GET_ActionTable(extMetaClass *, ActionEntry **, int *);
static ERR GET_Fields(extMetaClass *, const FieldArray **, int *);
static ERR GET_Location(extMetaClass *, CSTRING *);
static ERR GET_Methods(extMetaClass *, const MethodEntry **, int *);
static ERR GET_Module(extMetaClass *, CSTRING *);
static ERR GET_Objects(extMetaClass *, OBJECTID **, int *);
static ERR GET_RootModule(extMetaClass *, class RootModule **);
static ERR GET_Dictionary(extMetaClass *, struct Field **, int *);
static ERR GET_SubFields(extMetaClass *, const FieldArray **, int *);

static ERR SET_Actions(extMetaClass *, const ActionArray *);
static ERR SET_Fields(extMetaClass *, const FieldArray *, int);
static ERR SET_Methods(extMetaClass *, const MethodEntry *, int);

static ERR GET_ClassName(extMetaClass *Self, CSTRING *Value)
{
   *Value = Self->ClassName;
   return ERR::Okay;
}

static ERR SET_ClassName(extMetaClass *Self, CSTRING Value)
{
   Self->ClassName = Value;
   return ERR::Okay;
}

static const FieldDef CategoryTable[] = {
   { "Command",  CCF::COMMAND },
   { "Filesystem", CCF::FILESYSTEM },
   { "Graphics", CCF::GRAPHICS }, { "GUI",        CCF::GUI },
   { "IO",       CCF::IO },       { "System",     CCF::SYSTEM },
   { "Tool",     CCF::TOOL },     { "Data",       CCF::DATA },
   { "Audio",    CCF::AUDIO },    { "Misc",       CCF::MISC },
   { "Network",  CCF::NETWORK },  { "Multimedia", CCF::MULTIMEDIA },
   { nullptr, 0 }
};

static const std::vector<Field> glMetaFieldsPreset = {
   // If you adjust this table, remember to adjust the index numbers and the byte offsets into the structure.
   { 0, nullptr, nullptr, writeval_default, "ClassVersion",    FID_ClassVersion, sizeof(Object), 0, FDF_DOUBLE|FDF_RI },
   { MAXINT("FieldArray"), (ERR (*)(APTR, APTR))GET_Fields, (APTR)SET_Fields, writeval_default, "Fields", FID_Fields, sizeof(Object)+8, 1, FDF_ARRAY|FD_STRUCT|FDF_RI },
   { MAXINT("Field"),      (ERR (*)(APTR, APTR))GET_Dictionary, nullptr, writeval_default, "Dictionary", FID_Dictionary, sizeof(Object)+8+(sizeof(APTR)*1), 2, FDF_ARRAY|FD_STRUCT|FDF_R },
   { 0, nullptr, nullptr, writeval_default, "ClassName",       FID_ClassName,       sizeof(Object)+8+(sizeof(APTR)*2),  3,  FDF_STRING|FDF_RI },
   { 0, nullptr, nullptr, writeval_default, "FileExtension",   FID_FileExtension,   sizeof(Object)+8+(sizeof(APTR)*3),  4,  FDF_STRING|FDF_RI },
   { 0, nullptr, nullptr, writeval_default, "FileDescription", FID_FileDescription, sizeof(Object)+8+(sizeof(APTR)*4),  5,  FDF_STRING|FDF_RI },
   { 0, nullptr, nullptr, writeval_default, "FileHeader",      FID_FileHeader,      sizeof(Object)+8+(sizeof(APTR)*5),  6,  FDF_STRING|FDF_RI },
   { 0, nullptr, nullptr, writeval_default, "Path",            FID_Path,            sizeof(Object)+8+(sizeof(APTR)*6),  7,  FDF_STRING|FDF_RI },
   { 0, nullptr, nullptr, writeval_default, "Icon",            FID_Icon,            sizeof(Object)+8+(sizeof(APTR)*7),  8,  FDF_STRING|FDF_RI },
   { 0, nullptr, nullptr, writeval_default, "Size",            FID_Size,            sizeof(Object)+8+(sizeof(APTR)*8),  9,  FDF_INT|FDF_RI },
   { 0, nullptr, nullptr, writeval_default, "Flags",           FID_Flags,           sizeof(Object)+12+(sizeof(APTR)*8), 10, FDF_INT|FDF_RI },
   { 0, nullptr, nullptr, writeval_default, "ClassID",         FID_ClassID,         sizeof(Object)+16+(sizeof(APTR)*8), 11, FDF_INT|FDF_UNSIGNED|FDF_RI },
   { 0, nullptr, nullptr, writeval_default, "BaseClassID",     FID_BaseClassID,     sizeof(Object)+20+(sizeof(APTR)*8), 12, FDF_INT|FDF_UNSIGNED|FDF_RI },
   { 0, nullptr, nullptr, writeval_default, "OpenCount",       FID_OpenCount,       sizeof(Object)+24+(sizeof(APTR)*8), 13, FDF_INT|FDF_R },
   { MAXINT(&CategoryTable), nullptr, nullptr, writeval_default, "Category",  FID_Category,        sizeof(Object)+28+(sizeof(APTR)*8), 14, FDF_INT|FDF_LOOKUP|FDF_RI },
   // Virtual fields
   { MAXINT("MethodEntry"), (ERR (*)(APTR, APTR))GET_Methods, (APTR)SET_Methods, writeval_default, "Methods", FID_Methods, sizeof(Object), 15, FDF_ARRAY|FD_STRUCT|FDF_RI },
   { 0, nullptr, (APTR)SET_Actions,               writeval_default,   "Actions",           FID_Actions,         sizeof(Object), 16, FDF_POINTER|FDF_I },
   { 0, (ERR (*)(APTR, APTR))GET_ActionTable, 0,  writeval_default,   "ActionTable",       FID_ActionTable,     sizeof(Object), 17, FDF_ARRAY|FDF_POINTER|FDF_R },
   { 0, (ERR (*)(APTR, APTR))GET_Location, 0,     writeval_default,   "Location",          FID_Location,        sizeof(Object), 18, FDF_STRING|FDF_R },
   { 0, (ERR (*)(APTR, APTR))GET_ClassName, (APTR)SET_ClassName, writeval_default, "Name", FID_Name,            sizeof(Object), 19, FDF_STRING|FDF_SYSTEM|FDF_RI },
   { 0, (ERR (*)(APTR, APTR))GET_Module, 0,       writeval_default,   "Module",            FID_Module,          sizeof(Object), 20, FDF_STRING|FDF_R },
   { 0, (ERR (*)(APTR, APTR))GET_Objects, 0,      writeval_default,   "Objects",           FID_Objects,         sizeof(Object), 21, FDF_ARRAY|FDF_INT|FDF_ALLOC|FDF_R },
   { MAXINT("FieldArray"), (ERR (*)(APTR, APTR))GET_SubFields, 0, writeval_default, "SubFields", FID_SubFields, sizeof(Object), 22, FDF_ARRAY|FD_STRUCT|FDF_SYSTEM|FDF_R },
   { MAXINT(CLASSID::ROOTMODULE), (ERR (*)(APTR, APTR))GET_RootModule, 0, writeval_default, "RootModule", FID_RootModule, sizeof(Object), 23, FDF_OBJECT|FDF_R },
   { 0, (ERR (*)(APTR, APTR))OBJECT_GetID, 0,     writeval_default,   "ID",                FID_ID,              sizeof(Object), 24, FDF_INT|FDF_SYSTEM|FDF_R },
   { 0, 0, 0, nullptr, "", 0, 0, 0,  0 }
};

static const FieldArray glMetaFields[] = {
   { "ClassVersion",    FDF_DOUBLE|FDF_RI },
   { "Fields",          FDF_ARRAY|FD_STRUCT|FDF_RI, GET_Fields, SET_Fields, "FieldArray" },
   { "Dictionary",      FDF_ARRAY|FD_STRUCT|FDF_R, GET_Dictionary, nullptr, "Field" },
   { "ClassName",       FDF_STRING|FDF_RI },
   { "FileExtension",   FDF_STRING|FDF_RI },
   { "FileDescription", FDF_STRING|FDF_RI },
   { "FileHeader",      FDF_STRING|FDF_RI },
   { "Path",            FDF_STRING|FDF_RI },
   { "Size",            FDF_INT|FDF_RI },
   { "Flags",           FDF_INT|FDF_RI },
   { "ClassID",         FDF_INT|FDF_UNSIGNED|FDF_RI },
   { "BaseClassID",     FDF_INT|FDF_UNSIGNED|FDF_RI },
   { "OpenCount",       FDF_INT|FDF_R },
   { "Category",        FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, nullptr, &CategoryTable },
   // Virtual fields
   { "Methods",         FDF_ARRAY|FD_STRUCT|FDF_RI, GET_Methods, SET_Methods, "MethodEntry" },
   { "Actions",         FDF_POINTER|FDF_I },
   { "ActionTable",     FDF_ARRAY|FDF_POINTER|FDF_R, GET_ActionTable },
   { "Location",        FDF_STRING|FDF_R },
   { "Name",            FDF_STRING|FDF_SYSTEM|FDF_RI, GET_ClassName, SET_ClassName },
   { "Module",          FDF_STRING|FDF_R, GET_Module },
   { "Objects",         FDF_ARRAY|FDF_INT|FDF_ALLOC|FDF_R, GET_Objects },
   { "SubFields",       FDF_ARRAY|FD_STRUCT|FDF_SYSTEM|FDF_R, GET_SubFields, nullptr, "FieldArray" },
   { "RootModule",      FDF_OBJECT|FDF_R, GET_RootModule, nullptr, CLASSID::ROOTMODULE },
   { "ID",              FDF_INT|FDF_SYSTEM|FDF_R, OBJECT_GetID },
   END_FIELD
};

extern "C" ERR CLASS_FindField(extMetaClass *, struct mc::FindField *);
extern "C" ERR CLASS_Free(extMetaClass *);
extern "C" ERR CLASS_Init(extMetaClass *);
extern "C" ERR CLASS_NewPlacement(extMetaClass *);

FDEF argsFindField[] = { { "ID", FD_INT }, { "Field:Field", FD_RESULT|FD_PTR|FD_STRUCT }, { "Source", FD_RESULT|FD_OBJECTPTR }, { 0, 0 } };

extMetaClass glMetaClass;

//********************************************************************************************************************
// Standard signal action, applicable to all classes

static ERR DEFAULT_Signal(OBJECTPTR Object, APTR Void)
{
   Object->setFlag(NF::SIGNALLED);
   return ERR::Okay;
}

//********************************************************************************************************************

void init_metaclass(void)
{
   glMetaClass.Object::Class   = &glMetaClass;
   glMetaClass.Object::UID     = 123;
   glMetaClass.setFlag(NF::INITIALISED);

   glMetaClass.ClassVersion       = 1;
   glMetaClass.Fields             = glMetaFields;
   glMetaClass.ClassName          = "MetaClass";
   glMetaClass.Size               = sizeof(extMetaClass);
   glMetaClass.ClassID            = CLASSID::METACLASS;
   glMetaClass.BaseClassID        = CLASSID::METACLASS;
   glMetaClass.Category           = CCF::SYSTEM;
   glMetaClass.FieldLookup        = glMetaFieldsPreset;
   glMetaClass.OriginalFieldTotal = std::ssize(glMetaFields)-1;
   glMetaClass.Local[0]           = 0xff;

   glMetaClass.Methods.resize(2);
   glMetaClass.Methods[1] = { AC(-1), (APTR)CLASS_FindField, "FindField", argsFindField, sizeof(struct mc::FindField) };

   glMetaClass.ActionTable[int(AC::Free)].PerformAction = (ERR (*)(OBJECTPTR, APTR))CLASS_Free;
   glMetaClass.ActionTable[int(AC::Init)].PerformAction = (ERR (*)(OBJECTPTR, APTR))CLASS_Init;
   glMetaClass.ActionTable[int(AC::NewPlacement)].PerformAction = (ERR (*)(OBJECTPTR, APTR))CLASS_NewPlacement;
   glMetaClass.ActionTable[int(AC::Signal)].PerformAction = &DEFAULT_Signal;

   sort_class_fields(&glMetaClass, glMetaClass.FieldLookup);

   glMetaClass.BaseCeiling = glMetaClass.FieldLookup.size();

   glClassMap[CLASSID::METACLASS] = &glMetaClass;
}

/*********************************************************************************************************************

-METHOD-
FindField: Search a class definition for a specific field.

This method checks if a class has defined a given field by scanning its blueprint for a matching `ID`.  If found,
a direct pointer to the `Field` struct will be returned to the client.

In some classes the field might not be present in the main class spec, but does appear in an integral class.  In that
case, a reference to the class will be returned in the `Source` parameter.

-INPUT-
int ID: The field ID to search for.  Field names can be converted to ID's by using the `fieldhash()` function.
&struct(*Field) Field: Pointer to the !Field if discovered, otherwise `NULL`.
&obj(MetaClass) Source: Pointer to the class that is associated with the `Field` (which can match the caller), or `NULL` if the field was not found.

-RESULT-
Okay
NullArgs
Search

-END-

*********************************************************************************************************************/

ERR CLASS_FindField(extMetaClass *Self, struct mc::FindField *Args)
{
   if ((!Args) or (!Args->ID)) return ERR::NullArgs;

   extMetaClass *src = nullptr;
   Args->Field = lookup_id_byclass(Self, Args->ID, &src);
   Args->Source = src;
   if (Args->Field) return ERR::Okay;
   else return ERR::Search;
}

//********************************************************************************************************************

ERR CLASS_Free(extMetaClass *Self)
{
   if (Self->ClassID != CLASSID::NIL) glClassMap.erase(Self->ClassID);
   if (Self->Location) { FreeResource(Self->Location); Self->Location = nullptr; }

#ifndef KOTUKU_STATIC
   if (!Self->SubClasses.empty()) {
      // Sanity check - if a base has sub-classes present then there is an issue that requires resolution.
      // Note that for static builds there is no way to control termination order, so these controls are disabled.
      pf::Log log;
      log.warning("Out-of-order termination: Base-class %s has %d active sub-classes.", Self->ClassName, int(Self->SubClasses.size()));
   }

   if (Self->Base) {
      std::erase_if(Self->Base->SubClasses, [Self](extMetaClass *Cmp) { return Cmp IS Self; });
   }
#endif

   Self->~extMetaClass();
   return ERR::Okay;
}

//********************************************************************************************************************

ERR CLASS_Init(extMetaClass *Self)
{
   pf::Log log;

   if (!Self->ClassName) return log.warning(ERR::MissingClassName);

   auto class_hash = CLASSID(strihash(Self->ClassName));

   if (Self->BaseClassID != CLASSID::NIL) {
      if (class_hash != Self->BaseClassID) Self->ClassID = class_hash;
      else if (Self->ClassID IS CLASSID::NIL) Self->ClassID = Self->BaseClassID;
   }
   else if (Self->BaseClassID IS CLASSID::NIL) {
      Self->ClassID = Self->BaseClassID = class_hash;
   }

   if (Self->BaseClassID IS Self->ClassID) { // Base class
      if (!Self->Size) Self->Size = sizeof(Object);
      else if (Self->Size < (int)sizeof(Object)) { // Object size not specified
         log.warning("Size of %d is not valid.", Self->Size);
         return ERR::FieldNotSet;
      }
   }
   else {
      // If this is a sub-class, find the base class.  Note that FindClass() will automatically initialise the base if
      // there is a reference for it, so if it returns NULL then it is obvious that the base class is not installed on the
      // user's system.

      if (auto base = (extMetaClass *)FindClass(Self->BaseClassID)) {
         log.trace("Using base class $%.8x (%s) for %s", Self->BaseClassID, base->ClassName, Self->ClassName);
         if (!Self->FileDescription) Self->FileDescription = base->FileDescription;
         if (!Self->FileExtension)   Self->FileExtension   = base->FileExtension;
         if (!Self->ClassVersion)    Self->ClassVersion    = base->ClassVersion;

         // If over-riding field definitions have been specified by the sub-class, move them to the SubFields pointer.

         if (Self->Fields) Self->SubFields = Self->Fields;
         Self->Fields = base->Fields;
         Self->OriginalFieldTotal = base->OriginalFieldTotal;

         Self->Flags |= base->Flags;

         // In tightly controlled configurations, a sub-class can define a structure that is larger than the base
         // class.  Vector filter effects are one example.

         if (!Self->Size) Self->Size = base->Size;
         Self->Base = base;

         // Note: Sub-classes can override methods and also define custom methods independent of the base class.

         if (Self->Methods.size() < base->Methods.size()) Self->Methods.resize(base->Methods.size());

         // Copy method info from the base-class, but leave the sub-class' function pointers if defined.

         for (unsigned i=0; i < base->Methods.size(); i++) {
            Self->Methods[i].MethodID = base->Methods[i].MethodID;
            Self->Methods[i].Name = base->Methods[i].Name;
            Self->Methods[i].Args = base->Methods[i].Args;
            Self->Methods[i].Size = base->Methods[i].Size;
            if (!Self->Methods[i].Routine) Self->Methods[i].Routine = base->Methods[i].Routine;
         }

         base->SubClasses.push_back(Self);
      }
      else return log.warning(ERR::MissingClass);
   }

   field_setup(Self);

   glClassMap[Self->ClassID] = Self;

   // Record the name of the module that owns this class.

   for (auto it=tlContext.rbegin(); it != tlContext.rend()-1; it++) {
      if (it->obj->classID() IS CLASSID::ROOTMODULE) {
         Self->Root = (RootModule *)it->obj;
         break;
      }
   }

   bool save = false;
   if (!glClassDB.contains(Self->ClassID)) {
      save = fs_initialised ? true : false;

      glClassDB[Self->ClassID] = [Self]() {
         #ifdef __ANDROID__
            // On Android, all libraries are stored in the libs/ folder with no sub-folder hierarchy.  Because of this,
            // we rewrite the path to fit the Android system.

            if (Self->Path) {
               int i = StrLength(Self->Path);
               while ((i > 0) and (Self->Path[i] != '/') and (Self->Path[i] != '\\') and (Self->Path[i] != ':')) i--;
               if (i > 0) i++; // Skip folder separator.

               return ClassRecord(Self, make_optional(Self->Path.substr(i)));
            }
            else return ClassRecord(Self);
         #else
            return ClassRecord(Self);
         #endif
      }();
   }

#ifndef KOTUKU_STATIC
   if (save) {
      // Saving is only necessary if this is a new dictionary entry.
      if (auto lock = std::unique_lock{glmClassDB, 3s}) {
         static bool write_attempted = false;
         if ((!glClassFile) and (!write_attempted)) {
            write_attempted = true;
            auto flags = FL::WRITE;
            if (AnalysePath(glClassBinPath, nullptr) != ERR::Okay) flags |= FL::NEW;

            auto file = objFile::create::untracked(fl::Name("class_dict_output"), fl::Path(glClassBinPath), fl::Flags(flags),
               fl::Permissions(PERMIT::USER_READ|PERMIT::USER_WRITE|PERMIT::GROUP_READ|PERMIT::GROUP_WRITE|PERMIT::OTHERS_READ));

            if (!file) return ERR::File;
            glClassFile = file;

            int hdr = CLASSDB_HEADER;
            glClassFile->write(&hdr, sizeof(hdr));
         }

         if (glClassFile) {
            glClassFile->seekEnd(0);
            glClassDB[Self->ClassID].write(glClassFile);
         }
      }
      else return log.warning(ERR::SystemLocked);
   }
#endif

   return ERR::Okay;
}

//********************************************************************************************************************

ERR CLASS_NewPlacement(extMetaClass *Self)
{
   new (Self) extMetaClass;
   Self->Local[0] = 0xff;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Actions: Defines the actions supported by the class.

Action routines are associated with a new class by referring to a list of !ActionArray items in this field.
Each provided action ID must be paired with its associated routine.

Internally, the provided list is copied to the #ActionTable and the original values will serve no further purpose.

The following example shows a list of !ActionArray structures borrowed from the @Picture class.  Note that such lists
can also be auto-generated using our IDL scripts - an approach that we strongly recommend.

<pre>
ActionArray clActions[] = {
   { AC::Free,          PIC_Free },
   { AC::NewObject,     PIC_NewObject },
   { AC::Init,          PIC_Init },
   { AC::Query,         PIC_Query },
   { AC::Read,          PIC_Read },
   { AC::SaveToObject,  PIC_SaveToObject },
   { AC::Seek,          PIC_Seek },
   { AC::Write,         PIC_Write },
   { AC::NIL, nullptr }
};
</pre>

Note: Never refer to method ID's in an action list - the #Methods field is provided for this purpose.

*********************************************************************************************************************/

static ERR SET_Actions(extMetaClass *Self, const ActionArray *Actions)
{
   if (!Actions) return ERR::NullArgs;

   Self->ActionTable[int(AC::Signal)].PerformAction = &DEFAULT_Signal;

   for (auto i=0; Actions[i].ActionCode != AC::NIL; i++) {
      auto code = Actions[i].ActionCode;
      if ((code < AC::END) and (code > AC::NIL)) {
         Self->ActionTable[int(code)].PerformAction = (ERR (*)(OBJECTPTR, APTR))Actions[i].Routine;
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ActionTable: This field can be read to retrieve a MetaClass object's internal action table.

This field retrieves the internal action table of a class. The action table is arranged into a jump
table of action routines, with each routine pointing directly to the object support functions.  The size of the
jump table is defined by the global constant `AC::END`.  The table is sorted by action ID.

It is possible to check if an action is supported by a class by looking up its index within the ActionTable, for
example `Routine[AC::Read]`.  Calling an action routine directly from client code is an illegal operation.

*********************************************************************************************************************/

static ERR GET_ActionTable(extMetaClass *Self, ActionEntry **Value, int *Elements)
{
   *Value = Self->ActionTable;
   *Elements = int(AC::END);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
BaseClassID: Specifies the base class ID of a class object.

The BaseClassID must always be set prior to the initialisation of a new MetaClass object unless #ClassName is
considered sufficient.  If the BaseClassID is zero then the hash of #ClassName is computed and set as the
value of BaseClassID.

When defining a sub-class, it is required that the BaseClassID refers to the class that is being supported.

-FIELD-
ClassID: Specifies the ID of a class object.

The ClassID uniquely identifies a class.  If this value differs from the BaseClassID, then the class is determined
to be a sub-class.  The ClassID value always reflects the `strihash()` computation of the #ClassName, and is
automatically set on initialisation if not already set by the client.

-FIELD-
Category: The system category that a class belongs to.
Lookup: CCF

When designing a new class it is recommended that a suitable category is chosen and declared in this field.
It is acceptable for a class to be a member of multiple categories by combining category flags together.

-FIELD-
ClassName: The name of the represented class.

This field specifies the name of the represented class.  Characters should be limited to those in the range of A-Z
and written in camel case, e.g. `KeyValue` and not `Keyvalue` or `KEYVALUE`.  Setting this field value is compulsory
prior to initialisation.

-FIELD-
ClassVersion: The version number of the class.

This field value must reflect the version of the class structure.  Legal version numbers start from 1.0 and ascend.
Revision numbers can be used to indicate bug-fixes or very minor changes.

If declaring a sub-class then this value can be `0`, but base classes must set a value here.

-FIELD-
Dictionary: Returns a field lookup table sorted by field IDs.

Following initialisation of the MetaClass, the Dictionary can be read to retrieve the internal field lookup table.
For base-classes, the client can use the binary search technique to find fields by their ID.  For sub-classes, use
linear scanning.

*********************************************************************************************************************/

static ERR GET_Dictionary(extMetaClass *Self, struct Field **Value, int *Elements)
{
   if (Self->initialised()) {
      *Value    = Self->FieldLookup.data();
      *Elements = Self->FieldLookup.size();
      return ERR::Okay;
   }
   else return ERR::NotInitialised;
}

/*********************************************************************************************************************

-FIELD-
Fields: Points to a !FieldArray that describes the class' object structure.

This field points to an array that describes the structure of objects that will be generated for this class.  It is
compulsory that base classes define this array.  Sub-classes will inherit the structure of their base, but they may
set Fields with additional virtual fields if desired.

Refer to the Kotuku Wiki on class development for more information.

*********************************************************************************************************************/

static ERR GET_Fields(extMetaClass *Self, const FieldArray **Fields, int *Elements)
{
   *Fields = Self->Fields;
   *Elements = Self->OriginalFieldTotal;
   return ERR::Okay;
}

static ERR SET_Fields(extMetaClass *Self, const FieldArray *Fields, int Elements)
{
   if (!Fields) return ERR::NullArgs;

   Self->Fields = Fields;
   if (Elements > 0) {
      if (!Fields[Elements-1].Name) Elements--; // Make an adjustment in case the last entry is a null terminator.
      Self->OriginalFieldTotal = Elements;
   }
   else {
      int i;
      for (i=0; Fields[i].Name; i++);
      Self->OriginalFieldTotal = i;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FileDescription: Describes the file type represented by the class.

This field allows you to specify a description of the class' file type, if the class is designed to be file related.
This setting can be important, as it helps to distinguish your class from the other file based classes.  Always
make sure that your file description is short, descriptive and unique.  A file description such as `JPEG` is not
acceptable, but `JPEG Picture` would be appropriate.

-FIELD-
FileExtension: Describes the file extension represented by the class.

This field describes the file extension/s that the class will recognise.  For example: `*.wav|*.wave|*.snd`.

Use of the vertical bar allows more than one file extension to be supported by the class. The file extension that
is preferred for saving data must come first.

-FIELD-
FileHeader: Defines a string expression that will allow relevant file data to be matched to the class.

If a class supports file data, then the FileHeader field is used to identify the types of data that the class supports.
This feature is used by routines that need to analyse files and determine which classes support them.

For example, the JPEG class supports files that start with a 32-bit token to identify them as JPEG.  Declaring a
FileHeader expression to match these tokens will allow the FileView feature to detect JPEG files and display an
appropriate icon for each JPEG entry in the file list.

The expression format is `[Offset:Value]...`

The offset is an integer that identifies the byte position at which the given Value will be present.  The Value is
expressed as a hexadecimal number if it is prefixed with a dollar sign $, otherwise the Value is treated as
case-sensitive text.  Multiple expressions can be specified in sequence if there is more than one comparison to be
made - so `[0:$ff][8:$fe]` would require $ff at offset 0 and $fe at offset 8 in order to generate a match.

In some cases, a series of unrelated token sequences may need to be used to match against files.  This is true for the
JPEG class, which supports three different tokens all at offset 0 for identification.  Each matching sequence must be
separated with an OR symbol | as demonstrated in this example for the JPEG header: `[0:$ffd8ffe0]|[0:$ffd8ffe1]|[0:$ffd8fffe]`.

-FIELD-
Flags: Optional flag settings.

-FIELD-
Icon: Associates an icon with the file data for this class.

Files that belong to a class can be associated with an icon that is declared in this field.  The icon string format is
`folder/icon`.  Valid icons are available in the icon database.

-FIELD-
Location: Returns the path from which the class binary is loaded.

The path from which the class binary was loaded is readable from this field.  The path may not necessarily include the
file extension of the source binary.

*********************************************************************************************************************/

static ERR GET_Location(extMetaClass *Self, CSTRING *Value)
{
   if (Self->Path) { *Value = Self->Path; return ERR::Okay; }
   if (Self->Location) { *Value = Self->Location; return ERR::Okay; }

   if (Self->ClassID != CLASSID::NIL) {
      if (glClassDB.contains(Self->ClassID)) {
         Self->Location = strclone(glClassDB[Self->ClassID].Path);
      }
   }
   else if (glClassDB.contains(Self->BaseClassID)) {
      Self->Location = strclone(glClassDB[Self->BaseClassID].Path);
   }

   if ((*Value = Self->Location)) return ERR::Okay;
   else return ERR::MissingPath;
}

/*********************************************************************************************************************

-FIELD-
Methods: Set this field to define the methods supported by the class.

If a class design will include support for methods, create a !MethodEntry list and set this field prior to
initialisation.  The array must provide information on each method's ID, name, parameters, and structure size.

Note that method lists can be auto-generated using our IDL scripts - an approach that we strongly recommend.

*********************************************************************************************************************/

static ERR GET_Methods(extMetaClass *Self, const MethodEntry **Methods, int *Elements)
{
   *Methods = Self->Methods.data();
   *Elements = Self->Methods.size();
   return ERR::Okay;
}

static ERR SET_Methods(extMetaClass *Self, const MethodEntry *Methods, int Elements)
{
   pf::Log log;

   Self->Methods.clear();
   if (!Methods) return ERR::Okay;

   // Search for the method with the lowest ID number

   int lowest = 0;
   for (int i=0; Methods[i].MethodID != AC::NIL; i++) {
      if (int(Methods[i].MethodID) < lowest) lowest = int(Methods[i].MethodID);
   }

   // Generate the method array.  Note that the first entry that we put in the array will
   // be NULL because methods start at -1, not zero.

   if (lowest < 0) {
      log.msg("Detected %d methods in class %s.", -lowest, Self->ClassName ? Self->ClassName : (STRING)"Unnamed");
      Self->Methods.resize((-lowest) + 1);
      for (unsigned i=0; Methods[i].MethodID != AC::NIL; i++) {
         const int lk = -int(Methods[i].MethodID);
         Self->Methods[lk].MethodID = Methods[i].MethodID;
         Self->Methods[lk].Routine  = Methods[i].Routine;
         Self->Methods[lk].Size     = Methods[i].Size;
         Self->Methods[lk].Name     = Methods[i].Name;
         Self->Methods[lk].Args     = Methods[i].Args;
      }

      // NOTE: If this is a sub-class, the initialisation process will add the base-class methods to the list.
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Module: The name of the module binary that initialised the class.

*********************************************************************************************************************/

static ERR GET_Module(extMetaClass *Self, CSTRING *Value)
{
   if (!Self->initialised()) return ERR::NotInitialised;

   if (Self->Root) {
      *Value = Self->Root->LibraryName.c_str();
      return ERR::Okay;
   }
   else {
      *Value = "core";
      return ERR::Okay;
   }
}

/*********************************************************************************************************************

-FIELD-
Objects: Returns an allocated list of all objects that belong to this class.

This field will compile a list of all objects that belong to the class.  The list is sorted with the oldest
object appearing first.

The resulting array must be terminated with ~FreeResource() after use.

*********************************************************************************************************************/

static ERR GET_Objects(extMetaClass *Self, OBJECTID **Array, int *Elements)
{
   pf::Log log;
   std::list<OBJECTID> objlist;

   if (auto lock = std::unique_lock{glmMemory}) {
      for (const auto & [ id, mem ] : glPrivateMemory) {
         OBJECTPTR object;
         if (((mem.Flags & MEM::OBJECT) != MEM::NIL) and (object = (OBJECTPTR)mem.Address)) {
            if (Self->classID() IS object->classID()) {
               objlist.push_back(object->UID);
            }
         }
      }
   }
   else return log.warning(ERR::SystemLocked);

   if (!objlist.size()) {
      *Array = nullptr;
      *Elements = 0;
      return ERR::Okay;
   }

   objlist.sort([](const OBJECTID &a, const OBJECTID &b) { return (a < b); });

   OBJECTID *result;
   if (AllocMemory(sizeof(OBJECTID) * objlist.size(), MEM::NO_CLEAR, (APTR *)&result, nullptr) IS ERR::Okay) {
      int i = 0;
      for (const auto & id : objlist) result[i++] = id;
      *Array = result;
      *Elements = objlist.size();
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
}

/*********************************************************************************************************************

-FIELD-
OpenCount: The total number of active objects that are linked back to the MetaClass.

This field reveals the total number of objects that reference the class.  This figure will fluctuate over
time as new objects are created and old ones are destroyed.  When the OpenCount reaches zero, the Core may flush the
@Module that the class is related to, so long as no more programs are using it or any other classes created by
the module.

-FIELD-
Path: The path to the module binary that represents the class.

The Path field must be set on initialisation and refers to the default location of the class' module binary, for
example `modules:display`. For reasons of platform portability, the file extension must not be specified at the
end of the file name.

-FIELD-
RootModule: Returns a direct reference to the RootModule object that hosts the class.

*********************************************************************************************************************/

static ERR GET_RootModule(extMetaClass *Self, class RootModule **Value)
{
   *Value = Self->Root;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Size: The total size of the object structure represented by the MetaClass.

This field value must indicate the byte size of the objects that will be created from the MetaClass.  For example, the
@Picture class defines this value as `sizeof(objPicture)`.

If the size is not explicitly defined, the initialisation process will determine the structure size by
evaluating the field definitions that have been provided.
-END-

*********************************************************************************************************************/

static ERR GET_SubFields(extMetaClass *Self, const FieldArray **Fields, int *Elements)
{
   if (Self->SubFields) {
      int i;
      for (i=0; Self->SubFields[i].Name; i++);
      *Fields = Self->SubFields;
      *Elements = i;
   }
   else {
      *Fields = nullptr;
      *Elements = 0;
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Build the FieldLookup table, which is sorted by FieldID.  For base-classes this is straight-forward.
//
// For sub-classes the FieldLookup table is inherited from the base, then it may be modified by the sub-class
// blueprint.  Furthermore if the sub-class defines additional fields, these are appended to the FieldLookup table so
// that there is no interference with the order of fields at the base-class level.  This arrangement is important
// because some optimisations rely on the field order being consistent between base-class and sub-class
// implementations.

static void field_setup(extMetaClass *Class)
{
   if (Class->Base) { // This is a sub-class.
      Class->FieldLookup = Class->Base->FieldLookup;
      Class->BaseCeiling = Class->FieldLookup.size();

      if (Class->SubFields) {
         std::vector<Field> subFields;
         uint16_t offset = 0;
         for (unsigned i=0; Class->SubFields[i].Name; i++) {
            bool found = false;
            auto hash = fieldhash(Class->SubFields[i].Name);
            for (unsigned j=0; j < Class->FieldLookup.size(); j++) {
               if (Class->FieldLookup[j].FieldID IS hash) {
                  if (Class->SubFields[i].GetField) {
                     Class->FieldLookup[j].GetValue = (ERR (*)(APTR, APTR))Class->SubFields[i].GetField;
                     Class->FieldLookup[j].Flags |= FDF_R;
                  }

                  if (Class->SubFields[i].SetField) {
                     Class->FieldLookup[j].SetValue = Class->SubFields[i].SetField;
                     if (Class->FieldLookup[j].Flags & (FDF_W|FDF_I));
                     else Class->FieldLookup[j].Flags |= FDF_W;
                  }

                  optimise_write_field(Class->FieldLookup[j]);

                  found = true;
                  break;
               }
            }

            if (!found) add_field(Class, subFields, Class->SubFields[i], offset);
         }

         if (glLogLevel >= 2) register_fields(subFields);

         sort_class_fields(Class, subFields);

         Class->FieldLookup.insert(Class->FieldLookup.end(), subFields.begin(), subFields.end());
      }
   }
   else {
      bool name_field   = true;
      bool owner_field  = true;
      auto class_fields = Class->Fields;
      uint16_t offset   = sizeof(Object);
      for (unsigned i=0; class_fields[i].Name; i++) {
         add_field(Class, Class->FieldLookup, class_fields[i], offset);

         if (Class->FieldLookup[i].FieldID IS FID_Name) name_field = false;
         else if (Class->FieldLookup[i].FieldID IS FID_Owner) owner_field = false;
      }

      // Add mandatory system fields that haven't already been defined.

      if (name_field) {
         Class->FieldLookup.push_back({
            .Arg        = 0,
            .GetValue   = (ERR (*)(APTR, APTR))&OBJECT_GetName,
            .SetValue   = (APTR)&OBJECT_SetName,
            .WriteValue = &writeval_default,
            .Name       = "Name",
            .FieldID    = FID_Name,
            .Offset     = 0,
            .Index      = 0,
            .Flags      = FDF_STRING|FDF_RW|FDF_SYSTEM
         });
      }

      if (owner_field) {
         Class->FieldLookup.push_back({
            .Arg        = 0,
            .GetValue   = (ERR (*)(APTR, APTR))&OBJECT_GetOwner,
            .SetValue   = (APTR)&OBJECT_SetOwner,
            .WriteValue = &writeval_default,
            .Name       = "Owner",
            .FieldID    = FID_Owner,
            .Offset     = 0,
            .Index      = 0,
            .Flags      = FDF_OBJECTID|FDF_RW|FDF_SYSTEM
         });
      }

      Class->FieldLookup.push_back({
         .Arg        = 0,
         .GetValue   = (ERR (*)(APTR, APTR))&OBJECT_GetID,
         .SetValue   = nullptr,
         .WriteValue = &writeval_default,
         .Name       = "ID",
         .FieldID    = FID_ID,
         .Offset     = 0,
         .Index      = 0,
         .Flags      = FDF_INT|FDF_R|FDF_SYSTEM
      });

      // Add the Class field.  This is provided primarily to help scripting languages like Tiri.

      Class->FieldLookup.push_back({
         .Arg        = 0,
         .GetValue   = (ERR (*)(APTR, APTR))&OBJECT_GetClass,
         .SetValue   = nullptr,
         .WriteValue = &writeval_default,
         .Name       = "Class",
         .FieldID    = FID_Class,
         .Offset     = 0,
         .Index      = 0,
         .Flags      = FDF_OBJECT|FDF_POINTER|FDF_R|FDF_SYSTEM
      });

      // Add the ClassID field

      Class->FieldLookup.push_back({
         .Arg        = 0,
         .GetValue   = (ERR (*)(APTR, APTR))&OBJECT_GetClassID,
         .SetValue   = nullptr,
         .WriteValue = &writeval_default,
         .Name       = "ClassID",
         .FieldID    = FID_ClassID,
         .Offset     = 0,
         .Index      = 0,
         .Flags      = FDF_INT|FDF_UNSIGNED|FDF_R|FDF_SYSTEM
      });

      if (glLogLevel >= 2) register_fields(Class->FieldLookup);

      // Build a list of local objects before we do the sort

      std::vector<uint32_t> local(std::ssize(Class->Local));

      uint8_t int_count = 0;
      if ((Class->Flags & CLF::INHERIT_LOCAL) != CLF::NIL) {
         for (unsigned i=0; i < Class->FieldLookup.size(); i++) {
            if (Class->FieldLookup[i].Flags & FD_LOCAL) {
               Class->Local[int_count] = i;
               local[int_count++] = Class->FieldLookup[i].FieldID;
               if (int_count >= std::ssize(Class->Local)-1) break;
            }
         }
      }
      Class->Local[int_count] = 0xff;

      sort_class_fields(Class, Class->FieldLookup);

      // Repair local indexes

      for (unsigned i=0; i < int_count; i++) {
         for (unsigned j=0; j < Class->FieldLookup.size(); j++) {
            if (local[i] IS Class->FieldLookup[j].FieldID) {
               Class->Local[i] = j;
               break;
            }
         }
      }

      Class->BaseCeiling = Class->FieldLookup.size();
   }

   Class->Dictionary = Class->FieldLookup.data(); // This client-accessible lookup reference aids optimisation.
}

//********************************************************************************************************************
// Register a hashed field ID and its corresponding name.  Use FieldName() to retrieve field names from the store.

static void register_fields(std::vector<Field> &Fields)
{
   if (auto lock = std::unique_lock{glmFieldKeys, 1s}) {
      for (auto &field : Fields) {
         if (!glFields.contains(field.FieldID)) glFields[field.FieldID] = field.Name;
      }
   }
}

//********************************************************************************************************************

static void add_field(extMetaClass *Class, std::vector<Field> &Fields, const FieldArray &Source, uint16_t &Offset)
{
   pf::Log log(__FUNCTION__);

   auto &field = Fields.emplace_back(
      Source.Arg,
      (ERR (*)(APTR, APTR))Source.GetField,
      Source.SetField,
      writeval_default,
      Source.Name,
      fieldhash(Source.Name),
      Offset,
      0,
      Source.Flags
   );

   if (field.Flags & FD_VIRTUAL); // No offset will be added for virtual fields
   else if (field.Flags & FD_RGB) Offset += sizeof(int8_t) * 4;
   else if (field.Flags & (FD_POINTER|FD_ARRAY)) {
      #ifdef _LP64
         if (Offset & 0x7) { // Check for mis-alignment
            Offset = (Offset + 7) & (~0x7);
            if (((field.Flags & FDF_R) and (!field.GetValue)) or
                ((field.Flags & FDF_W) and (!field.SetValue))) {
               log.warning("Misaligned 64-bit pointer '%s' in class '%s'.", field.Name, Class->ClassName);
            }
         }
      #endif
      Offset += sizeof(APTR);
   }
   else if (field.Flags & FD_INT) Offset += sizeof(int);
   else if (field.Flags & FD_BYTE) Offset += sizeof(int8_t);
   else if (field.Flags & FD_FUNCTION) Offset += sizeof(FUNCTION);
   else if (field.Flags & (FD_DOUBLE|FD_INT64)) {
      if (Offset & 0x7) {
         if (((field.Flags & FDF_R) and (!field.GetValue)) or
             ((field.Flags & FDF_W) and (!field.SetValue))) {
            log.warning("Misaligned 64-bit field '%s' in class '%s'.", field.Name, Class->ClassName);
         }
      }
      Offset += 8;
   }
   else log.warning("%s field \"%s\"/%d has an invalid flag setting.", Class->ClassName, field.Name, field.FieldID);

   optimise_write_field(field);
}

//********************************************************************************************************************
// Sort the field table by FID and compute the field indexes.

static void sort_class_fields(extMetaClass *Class, std::vector<Field> &Fields)
{
   std::sort(Fields.begin(), Fields.end(),
      [](const Field &a, const Field &b ) { return a.FieldID < b.FieldID; }
   );

   for (unsigned i=0; i < Fields.size(); i++) Fields[i].Index = i;
}

//********************************************************************************************************************
// These are pre-defined fields that are applied to each class' object.

static ERR OBJECT_GetClass(OBJECTPTR Self, extMetaClass **Value)
{
   *Value = Self->ExtClass;
   return ERR::Okay;
}

static ERR OBJECT_GetID(OBJECTPTR Self, OBJECTID *Value)
{
   *Value = Self->UID;
   return ERR::Okay;
}

static ERR OBJECT_GetClassID(OBJECTPTR Self, CLASSID *Value)
{
   *Value = Self->classID();
   return ERR::Okay;
}

static ERR OBJECT_GetOwner(OBJECTPTR Self, OBJECTID *OwnerID)
{
   *OwnerID = Self->ownerID();
   return ERR::Okay;
}

static ERR OBJECT_SetOwner(OBJECTPTR Self, OBJECTID OwnerID)
{
   pf::Log log;

   if (OwnerID) {
      ScopedObjectLock new_owner(OwnerID);
      if (new_owner.granted()) {
         SetOwner(Self, *new_owner);
         return ERR::Okay;
      }
      else return log.warning(ERR::ExclusiveDenied);
   }
   else return log.warning(ERR::NullArgs);
}

static ERR OBJECT_GetName(OBJECTPTR Self, STRING *Name)
{
   *Name = Self->Name;
   return ERR::Okay;
}

static ERR OBJECT_SetName(OBJECTPTR Self, CSTRING Name)
{
   if (!Name) return SetName(Self, "");
   else return SetName(Self, Name);
}

//********************************************************************************************************************
// [Refer to register_class() to see how classes are recognised]
//
// If the classes.bin file is missing or incomplete, this code will scan for every module installed in the system and
// initialise it so that all classes can be registered in the class database.

#ifndef KOTUKU_STATIC
void scan_classes(void)
{
   pf::Log log("Core");

   log.branch("Scanning for available classes.");

   glClassDB.clear();
   DeleteFile(glClassBinPath, nullptr);

   DirInfo *dir;
   int total = 0;
   if (OpenDir("modules:", RDF::QUALIFY, &dir) IS ERR::Okay) {
      while (ScanDir(dir) IS ERR::Okay) {
         FileInfo *list = dir->Info;

         if ((list->Flags & RDF::FILE) != RDF::NIL) {
            #ifdef __ANDROID__
               if (pf::startswith("libshim.", list->Name)) continue;
               if (pf::startswith("libcore.", list->Name)) continue;
            #else
               if (pf::startswith("core.", list->Name)) continue;
            #endif

            auto modules = std::string("modules:") + list->Name;

            log.msg("Loading module for class scan: %s", modules.c_str());

            objModule::create mod = { fl::Name(modules), fl::Flags(MOF::SYSTEM_PROBE) };

            total++;
         }

         // For every 16 modules loaded, run an expunge.  This keeps memory usage down, and on Android
         // is essential because there is a library limit.

         if ((total & 0x1f) IS 0x10) Expunge(FALSE);
      }
      FreeResource(dir);
   }

   log.msg("Class scan complete - processed %d modules.", total);
}
#endif

//********************************************************************************************************************
// Lookup the fields declared by a MetaClass, as opposed to the fields of the MetaClass itself.

static Field * lookup_id_byclass(extMetaClass *Class, uint32_t FieldID, extMetaClass **Result)
{
   auto &field = Class->FieldLookup;

   int floor = 0;
   int ceiling = Class->BaseCeiling;
   while (floor < ceiling) {
      int i = (floor + ceiling)>>1;
      if (field[i].FieldID < FieldID) floor = i + 1;
      else if (field[i].FieldID > FieldID) ceiling = i;
      else {
         *Result = Class;
         return &field[i];
      }
   }

   // Sub-class fields (located in the upper register of FieldLookup)

   if (Class->BaseCeiling < Class->FieldLookup.size()) {
      unsigned floor = Class->BaseCeiling;
      unsigned ceiling = Class->FieldLookup.size();
      while (floor < ceiling) {
         int i = (floor + ceiling)>>1;
         if (field[i].FieldID < FieldID) floor = i + 1;
         else if (field[i].FieldID > FieldID) ceiling = i;
         else {
            *Result = Class;
            return &field[i];
         }
      }
   }

   for (unsigned i=0; Class->Local[i] != 0xff; i++) {
      auto &field = Class->FieldLookup[Class->Local[i]];
      if (field.Arg) {
         if (auto child_class = (extMetaClass *)FindClass(CLASSID(field.Arg))) {
            *Result = child_class;
            if (auto child_field = lookup_id_byclass(child_class, FieldID, Result)) return child_field;
            *Result = nullptr;
         }
      }
   }

   return nullptr;
}
