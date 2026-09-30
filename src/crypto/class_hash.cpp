/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Hash: Computes a message digest or HMAC incrementally.

Hash objects process data in independently bounded chunks, allowing messages larger than the one-shot Crypto function
limit of 1 MiB.  Select #Algorithm before initialisation.  To compute an HMAC, also set #Key before initialisation;
leaving the key unset selects an ordinary digest.  An explicitly supplied empty key selects HMAC with an empty key.

MD5 and SHA-1 are available only for compatibility with existing formats and protocols.  They are not suitable for
security purposes.  Use SHA-256 or SHA-512 for new designs.

Calling #Digest() does not change the running state, so more data may be appended afterwards.  Call #Reset() to begin
a new message while retaining the selected algorithm and key.

-END-

*********************************************************************************************************************/

#define PRV_HASH

class extHash : public objHash {
public:
   std::vector<int8_t> KeyData;
   std::unique_ptr<crypto_backend::HashContext> Context;
   bool KeySet = false;

   extHash(objMetaClass *ClassPtr, OBJECTID ObjectID) : objHash(ClassPtr, ObjectID) { }
   ~extHash() {
      volatile int8_t *key = KeyData.data();
      for (size_t i=0; i < KeyData.size(); i++) key[i] = 0;
   }
};

/*********************************************************************************************************************

-METHOD-
Update: Adds a chunk of data to the running digest or HMAC.

Each `Data` chunk may contain up to 1 MiB.  Call this method repeatedly to process a larger message.

-INPUT-
array(char) Data: The next data chunk, up to 1 MiB.

-ERRORS-
Okay
Args: `Data` exceeds 1 MiB.
NullArgs: No method arguments were supplied.
NotInitialised: The object has not been initialised.
Failed: The cryptography provider failed.

-END-

*********************************************************************************************************************/

static ERR HASH_Update(extHash *Self, struct hsh::Update *Args)
{
   if (not Self->Context) return ERR::NotInitialised;
   if (not Args) return ERR::NullArgs;
   if (Args->Data.size() > MAX_CRYPTO_INPUT) return ERR::Args;
   return Self->Context->update(Args->Data);
}

/*********************************************************************************************************************

-METHOD-
Digest: Copies the current digest or HMAC to an output array.

`Output` must be exactly 16 bytes for MD5, 20 for SHA-1, 32 for SHA-256 or 64 for SHA-512.  This method does not change
the running state.

-INPUT-
^array(char) Output: Receives the binary digest.

-ERRORS-
Okay
Args: `Output` has the wrong length.
NullArgs: No method arguments were supplied.
NotInitialised: The object has not been initialised.
Failed: The cryptography provider failed.

-END-

*********************************************************************************************************************/

static ERR HASH_Digest(extHash *Self, struct hsh::Digest *Args)
{
   if (not Self->Context) return ERR::NotInitialised;
   if (not Args) return ERR::NullArgs;
   if (Args->Output.size() != crypto_backend::digest_size(Self->Algorithm)) return ERR::Args;
   return Self->Context->digest(Args->Output);
}

/*********************************************************************************************************************

-ACTION-
Reset: Starts a new digest or HMAC with the same algorithm and key.

-ERRORS-
Okay
NotInitialised: The object has not been initialised.
Failed: The cryptography provider failed.

-END-

*********************************************************************************************************************/

static ERR HASH_Reset(extHash *Self)
{
   if (not Self->Context) return ERR::NotInitialised;
   return Self->Context->reset();
}

/*********************************************************************************************************************

-ACTION-
Init: Initialises the hashing context.

-ERRORS-
Okay
FieldNotSet: #Algorithm has not been set.
NoSupport: #Algorithm is not supported.
AllocMemory: The platform hashing context could not be allocated.

-END-

*********************************************************************************************************************/

static ERR HASH_Init(extHash *Self)
{
   if (Self->Algorithm IS HASH::NIL) return ERR::FieldNotSet;
   if (not crypto_backend::digest_size(Self->Algorithm)) return ERR::NoSupport;

   Self->Context = crypto_backend::make_hash_context(Self->Algorithm, Self->KeyData, Self->KeySet);
   return Self->Context ? ERR::Okay : ERR::AllocMemory;
}

/*********************************************************************************************************************

-FIELD-
Key: Selects HMAC mode and provides its authentication key.

Set this write-only field before initialisation to select HMAC.  The key may be empty and may contain null bytes.
Leaving the field unset selects an ordinary message digest.  The key is copied into private object storage and cleared
when the object is destroyed.

-END-

*********************************************************************************************************************/

static ERR SET_Key(extHash *Self, const std::span<const int8_t> &Value)
{
   if (Self->initialised()) return ERR::NoFieldAccess;
   if (Value.size() > MAX_CRYPTO_INPUT) return ERR::Args;
   Self->KeyData.assign(Value.begin(), Value.end());
   Self->KeySet = true;
   return ERR::Okay;
}

//********************************************************************************************************************

#include "class_hash_def.c"

//********************************************************************************************************************

static const FieldArray clHashFields[] = {
   { "Algorithm", FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, nullptr, &clHashAlgorithm },
   { "Key",       FDF_VIRTUAL|FDF_ARRAY|FDF_BYTE|FDF_W|FDF_INIT|FDF_PURE, nullptr, SET_Key },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_hash(void)
{
   clHash = objMetaClass::create::global(
      fl::ClassVersion(VER_HASH),
      fl::Name("Hash"),
      fl::Category(CCF::DATA),
      fl::Actions(clHashActions),
      fl::Methods(clHashMethods),
      fl::Fields(clHashFields),
      fl::Size(sizeof(extHash)),
      fl::Path(MOD_PATH));

   return clHash ? ERR::Okay : ERR::AddClass;
}
