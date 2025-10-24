
// Execute transformation

static ERR XSLT_Activate(objXSLT *Template, APTR Void)
{
   return ERR::NoSupport;
}
 // Reset all state and cached data

static ERR XSLT_Clear(objXSLT *Template, APTR Void)
{
   return ERR::NoSupport;
}

// Loads the stylesheet and validates it.  Can fail if the stylesheet is invalid.  Can compile the stylesheet if necessary

static ERR XSLT_Init(objXSLT *Template, APTR Void)
{
   return ERR::NoSupport;
}

// Get a parameter key-value

static ERR XSLT_GetKey(objXSLT *Template, struct acGetKey *Args)
{
   return ERR::NoSupport;
}

// Reload stylesheet if modified

static ERR XSLT_Refresh(objXSLT *Template, APTR Void)
{
   return ERR::NoSupport;
}

// Clear errors and reset state

static ERR XSLT_Reset(objXSLT *Template, APTR Void)
{
   return ERR::NoSupport;
}

// Set a parameter key-value

static ERR XSLT_SetKey(objXSLT *Template, struct acSetKey *Args)
{
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-METHOD-
RegisterFunction: Registers a custom extension function for use within XSLT stylesheets.

-INPUT-
cstr Namespace: The namespace URI for the function.
cstr Name: The local name of the function.
ptr(func) Function: Pointer to the function implementation.

-END-

*********************************************************************************************************************/

static ERR XSLT_RegisterFunction(objXSLT *Template, CSTRING Namespace, CSTRING Name, FUNCTION *Function)
{
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-METHOD-
DeregisterFunction: Deregisters a previously registered custom extension function.

-INPUT-
cstr Namespace: The namespace URI for the function.
cstr Name: The local name of the function.

-END-

*********************************************************************************************************************/

static ERR XSLT_DeregisterFunction(objXSLT *Template, CSTRING Namespace, CSTRING Name)
{
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-FIELD-
TemplateList: Returns a list of template names defined in the stylesheet.

!ptr(array(cstr)) Templates

-END-

*********************************************************************************************************************/

static ERR XSLT_GET_TemplateList(objXSLT *Template, CSTRING **Templates)
{
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-FIELD-
VariableList: Returns a list of variable names defined in the stylesheet.

ptr(array(cstr)) Templates:

-END-

*********************************************************************************************************************/

static ERR XSLT_GET_VariableList(objXSLT *Template, CSTRING **Variables)
{
   return ERR::NoSupport;
}

// Utility Methods

/*********************************************************************************************************************
-METHOD-
ResolveIncludes: Resolves all xsl:include and xsl:import directives in the stylesheet

-ERRORS-

-END-
*********************************************************************************************************************/

static ERR XSLT_ResolveIncludes(objXSLT *Template)
{
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-FIELD-
OutputProperties: Retrieves the output properties defined in the stylesheet

struct OutputProperties *Props

-END-

*********************************************************************************************************************/

static ERR XSLT_GetOutputProperties(struct OutputProperties *Props)
{
   return ERR::NoSupport;
}

//********************************************************************************************************************

#include "xslt_class_def.c"

static const FieldArray clFields[] = {
   { "Stylesheet",     FDF_STRING|FDF_RW, nullptr, SET_Styleshet },
   { "Source",         FDF_STRING|FDF_RW, nullptr, SET_Source },
   { "Output",         FDF_OBJECT|FDF_RW, nullptr, SET_Output },
   { "OutputEncoding", FDF_STRING|FDF_RW, nullptr, SET_OutputEncoding },
   { "ErrorMsg",       FDF_STRING|FDF_RW },
   { "BaseURI",        FDF_STRING|FDF_RW, nullptr, SET_BaseURI },
   { "XML",            FDF_OBJECT|FDF_RW, nullptr, SET_XML },
   { "StylesheetXML",  FDF_OBJECT|FDF_R },
   { "Version",        FDF_DOUBLE|FDF_RW },
   { "OutputMethod",   FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clXSLTOutputMethods },
   { "IndentLevel",    FDF_INT|FDF_RW },
   { "WarningCount",   FDF_INT|FDF_R },
   // Virtual fields
   { "TemplateList",   FDF_ARRAY|FDF_STRING|FDF_R, XSLT_GetTemplateList },
   { "VariableList",   FDF_ARRAY | FDF_STRING | FDF_R, XSLT_GetVariableList },

   // Virtual fields
   { "ErrorMsg",   FDF_STRING|FDF_R, GET_ErrorMsg },
   { "ReadOnly",   FDF_INT|FDF_RI, GET_ReadOnly, SET_ReadOnly },
   { "Src",        FDF_STRING|FDF_SYNONYM|FDF_RW, GET_Path, SET_Path },
   { "Statement",  FDF_STRING|FDF_ALLOC|FDF_RW, GET_Statement, SET_Statement },
   { "Tags",       FDF_ARRAY|FDF_STRUCT|FDF_R, GET_Tags, nullptr, "XSLTTag" },
   END_FIELD
};

static ERR add_xslt_class(void)
{
   clXSLT = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::XSLT),
      fl::ClassVersion(VER_XSLT),
      fl::Name("XSLT"),
      fl::FileExtension("*.xslt"),
      fl::FileDescription("Extensible Stylesheet Language Transformations (XSLT)"),
      fl::Icon("filetypes/xml"),
      fl::Category(CCF::DATA),
      fl::Actions(clXSLTActions),
      fl::Methods(clXSLTMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(extXSLT)),
      fl::Path(MOD_PATH));

   return clXSLT ? ERR::Okay : ERR::AddClass;
}
