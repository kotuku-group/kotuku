/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
XML: Parses, queries and updates XML document structures.

The XML class creates and maintains parsed XML data as a hierarchy of !XTag structures.  It accepts both strictly
well-formed XML and the module's default relaxed parsing mode, depending on the #Flags supplied by the caller.  Parsed
documents can be queried with XPath/XQuery expressions, modified with XML methods and validated against an XML Schema
loaded with #LoadSchema().

XML data can be loaded from a file path, from an in-memory XML statement or from another object that supports the Read
action.  Set #Path to load from the file system, #Statement to parse an XML string, or #Source to read from another
object.  When #Path or #Statement is changed after initialisation, the object clears the previous document and parses
the new source.

Parsed content is available through #Tags.  Each !XTag stores the element name, attributes, child tags, content nodes,
line number and namespace ID.  C++ callers can traverse the hierarchy directly through `kt::vector&lt;XTag&gt;`; Tiri
callers should cache #Tags only while the XML object remains unchanged because reading the field copies the structure.

Use the XML object's methods when changing the tree so that modification timestamps and derived state remain
consistent.  Direct read access to !XTag is appropriate for traversal and high-volume inspection.

<b>Not supported:</b> DTD validation is not implemented.  The parser records DOCTYPE declarations and selected entity,
notation and identifier data, but it does not load or validate external DTDs.  Use XML Schema (XSD) validation instead.

-END-

*********************************************************************************************************************/

// Use in search() calls to report the first matching tag.  Expects an int reference in the Meta parameter.

struct matching_tag_opt {
   int tag_id = 0;
   std::string attrib;
   FUNCTION *callback = nullptr;
};

static ERR save_matching_tag(extXML *Self, int TagID, CSTRING Attrib, matching_tag_opt &Result)
{
   Result.tag_id = TagID;
   if (Attrib) Result.attrib.assign(Attrib);
   return ERR::Terminate; // We only want the first match
}

//********************************************************************************************************************

static ERR find_all_tags(extXML *Self, int TagID, CSTRING Attrib, matching_tag_opt &Options)
{
   if (not Options.tag_id) Options.tag_id = TagID; // Store the first matching tag ID

   ERR callback_error = ERR::Okay;
   if (Options.callback->isC()) {
      auto routine = (ERR (*)(extXML *, int, CSTRING, APTR))Options.callback->Routine;
      callback_error = routine(Self, TagID, Attrib, Options.callback->Meta);
   }
   else if (Options.callback->isScript()) {
      if (sc::Call(*Options.callback, std::to_array<ScriptArg>({
         { "XML",  Self, FD_OBJECTPTR },
         { "Tag",  TagID },
         { "Attrib", Attrib }
      }), callback_error) != ERR::Okay) return ERR::Terminate;
   }
   else return ERR::InvalidValue;

   return callback_error;
}

/*********************************************************************************************************************
-ACTION-
Clear: Completely clears all XML data and resets the object to its initial state.

The Clear action removes parsed XML content, namespace base URI mappings, DOCTYPE information, entity data, notation
data and parser status.  The object remains usable and can receive new XML data afterwards.

-ERRORS-
Okay

-TAGS-
mutates-object
-END-
*********************************************************************************************************************/

static ERR XML_Clear(extXML *Self)
{
   Self->Path.clear();
   Self->Tags.clear();
   Self->BaseURIMap.clear();
   Self->DocType.clear();
   Self->PublicID.clear();
   Self->SystemID.clear();
   Self->Entities.clear();
   Self->ParameterEntities.clear();
   Self->Notations.clear();
   Self->LineNo = 1;
   Self->ParseError = ERR::Okay;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
DataFeed: Processes and integrates external XML data into the object's document structure.

DataFeed accepts XML or text data and parses it into the object's tag hierarchy.  If the object has no existing tags,
the parsed data becomes the document structure.  If tags already exist, the new data is parsed into a temporary
hierarchy and appended to the root-level tag list.

The action is rejected when `XMF::READ_ONLY` is enabled.  Parse failures are returned to the caller; when appending to an
existing document, the existing tag hierarchy is left unchanged if the new data cannot be parsed.

Example:

<code>
local xml = obj.new('xml')
local err = xml.acDataFeed(nil, DATA_XML, '<first>First element</first>')
</code>

-ERRORS-
Okay
NullArgs
ReadOnly
InvalidData
Syntax
Failed
UnbalancedXML

-TAGS-
mutates-object, copies-input

-END-

*********************************************************************************************************************/

static ERR XML_DataFeed(extXML *Self, struct acDataFeed *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   if ((Args->Datatype IS DATA::XML) or (Args->Datatype IS DATA::TEXT)) {
      if ((Self->Flags & XMF::READ_ONLY) != XMF::NIL) return log.warning(ERR::ReadOnly);

      if (Self->Tags.empty()) {
         if (auto error = txt_to_xml(Self, Self->Tags, std::string_view((char *)Args->Buffer, Args->Size)); error != ERR::Okay) {
            return log.warning(error);
         }
      }
      else {
         TAGS tags;
         if (auto error = txt_to_xml(Self, tags, std::string_view((char *)Args->Buffer, Args->Size)); error != ERR::Okay) {
            return log.warning(error);
         }

         Self->Tags.insert(Self->Tags.end(), tags.begin(), tags.end());
      }

      Self->modified();
   }
   else return log.warning(ERR::InvalidData);

   return ERR::Okay;
}


/*********************************************************************************************************************

-METHOD-
Evaluate: Run an XQuery expression against the XML data.

The Evaluate method allows the execution of XPath 2.0+ and XQuery 1.0+ expressions against the data contained within
the XML object.  This is a lazy execution method that compiles and evaluates the provided expression in a single
step, returning the result as a string.  For more complex scenarios or repeated evaluations, consider using the
Compile and Evaluate functions in the XPath module.

-INPUT-
strview Statement: An XQuery expression to evaluate.
^&string Result: The evaluation result is returned here.

-ERRORS-
Okay
NullArgs
FieldNotSet
NewObject

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR XML_Evaluate(extXML *Self, struct xml::Evaluate *Args)
{
   kt::Log log;

   if ((not Args) or Args->Statement.empty() or (not Args->Result)) return log.warning(ERR::NullArgs);
   Args->Result->clear();

   log.branch("");

   objXQuery *xq;
   if (!NewObject(CLASSID::XQUERY, NF::NIL, (OBJECTPTR *)&xq)) {
      xq->set(FID_Statement, Args->Statement);
      if (auto error = xq->init(); !error) {
         if (error = xq->evaluate(Self, 0, XEF::NIL); !error) {
            std::string_view result;
            if (!xq->get(FID_ResultString, result)) Args->Result->assign(result);
            else error = ERR::FieldNotSet;
            FreeResource(xq);
            return error;
         }
         else {
            std::string_view sv;
            if (!xq->get(FID_ErrorMsg, sv)) Self->ErrorMsg.assign(sv);
            FreeResource(xq);
            return error;
         }
      }
      else {
         std::string_view sv;
         if (!xq->get(FID_ErrorMsg, sv)) Self->ErrorMsg.assign(sv);
         FreeResource(xq);
         return error;
      }
   }
   else return log.warning(ERR::NewObject);
}

/*********************************************************************************************************************

-METHOD-
Filter: Filters the XML data structure to retain only a specific tag and its descendants.

The Filter method provides a mechanism for reducing large XML documents to a specific subtree, permanently removing
all content that exists outside the targeted element and its children.  This operation is particularly valuable for
performance optimisation when working with large documents where only a specific section is relevant.

The filtering process locates the first element matched by the supplied XPath/XQuery expression.  The XML object is
then replaced by a new root-level structure containing that tag and its descendants.  Sibling tags, parent elements and
unrelated branches are discarded.

-INPUT-
strview XPath: XPath/XQuery expression that identifies the target tag to retain.

-ERRORS-
Okay: The XML structure now contains only the first matched subtree.
NullArgs: The XPath parameter was empty.
Search: No matching tag could be found for the specified XPath expression.
NewObject
Terminate

-TAGS-
mutates-object

-END-

*********************************************************************************************************************/

static ERR XML_Filter(extXML *Self, struct xml::Filter *Args)
{
   kt::Log log;

   if ((not Args) or Args->XPath.empty()) return log.warning(ERR::NullArgs);

   objXQuery *xq;
   if (!NewObject(CLASSID::XQUERY, NF::NIL, (OBJECTPTR *)&xq)) {
      xq->set(FID_Statement, Args->XPath);
      if (auto error = xq->init(); !error) {
         matching_tag_opt opt;
         auto callback = C_FUNCTION(save_matching_tag, &opt);
         if (error = xq->search(Self, callback, 0, XEF::NIL); error IS ERR::Terminate) {
            if (opt.tag_id) {
               auto tag = Self->getTag(opt.tag_id);
               auto new_tags = TAGS(tag, tag + 1);
               Self->Tags = std::move(new_tags);
               Self->modified();
            }
            FreeResource(xq);
            return ERR::Okay;
         }
         else {
            if (!error) { // Nothing found
               error = ERR::Search;
               Self->ErrorMsg = "No matching tag found";
            }
            else {
               std::string_view str;
               if (!xq->get(FID_ErrorMsg, str)) Self->ErrorMsg = str;
            }
            FreeResource(xq);
            return error;
         }
      }
      else {
         std::string_view sv;
         if (!xq->get(FID_ErrorMsg, sv)) Self->ErrorMsg.assign(sv);
         FreeResource(xq);
         return error;
      }
   }
   else return log.warning(ERR::NewObject);
}

/*********************************************************************************************************************

-METHOD-
Search: Searches for XML elements using XPath and XQuery expressions with optional callback processing.

The Search method provides the primary mechanism for locating XML elements within the document structure using
XPath and XQuery expressions.  The method supports both single-result queries and comprehensive tree traversal
with callback-based processing for complex operations.

When no callback function is provided, Search returns the first matching element and terminates the search
immediately.  This is optimal for simple queries where only the first occurrence is required.

When a callback function is specified, Search calls it for each matching element until the query completes, the
callback returns an error, or the callback returns `ERR::Terminate`.

The C++ prototype for Callback is `ERR Function(*XML, int TagID, CSTRING Attrib, APTR Meta)`.

The callback should return `ERR::Okay` to continue processing, or `ERR::Terminate` to halt the search immediately.
Other callback errors are returned to the caller.

Note: If an error occurs, check the #ErrorMsg field for a custom error message containing further details.

-INPUT-
strview Expression: A valid XQuery expression.
ptr(func) Callback: Optional pointer to a callback function for processing multiple matches.
&int Result: UID of the first matching tag.  When Callback is defined, this is the first matching tag processed.

-ERRORS-
Okay: A matching tag was found (or callback processing completed successfully).
NullArgs: The Expression was empty.
NoData: The XML document contains no data to search.
Search: No matching tag could be found for the specified expression.
NewObject
Terminate

-TAGS-
mutates-object, callback-inlines

-END-

*********************************************************************************************************************/

static ERR XML_Search(extXML *Self, struct xml::Search *Args)
{
   kt::Log log;

   Self->ErrorMsg.clear();

   if ((not Args) or Args->Expression.empty()) return ERR::NullArgs;
   if ((Self->Flags & XMF::LOG_ALL) != XMF::NIL) {
      log.msg("Expression: %.*s", int(Args->Expression.size()), Args->Expression.data());
   }
   if (Self->Tags.empty()) return ERR::NoData;

   objXQuery *xq;
   if (!NewObject(CLASSID::XQUERY, NF::NIL, (OBJECTPTR *)&xq)) {
      xq->set(FID_Statement, Args->Expression);

      if (auto error = xq->init(); !error) {
         matching_tag_opt opt;

         FUNCTION callback;
         if ((Args->Callback) and (Args->Callback->defined())) {
            opt.callback = Args->Callback;
            callback = C_FUNCTION(find_all_tags, &opt);
            error = xq->search((objXML *)Self, callback, 0, XEF::NIL);
         }
         else {
            callback = C_FUNCTION(save_matching_tag, &opt);
            error = xq->search((objXML *)Self, callback, 0, XEF::NIL);
            if (error IS ERR::Terminate) error = ERR::Okay; // Terminate means a match was accepted
            else if (!error) error = ERR::Search; // Nothing found
         }

         if (!error) {
            FreeResource(xq);

            if ((Self->Flags & XMF::LOG_ALL) != XMF::NIL) log.msg("Found tag %d, Attrib: %s", opt.tag_id, opt.attrib.c_str());
            Args->Result = opt.tag_id;
            return ERR::Okay;
         }
         else {
            std::string_view sv;
            if ((!xq->get(FID_ErrorMsg, sv)) and (not sv.empty())) Self->ErrorMsg.assign(sv);
            FreeResource(xq);
            if ((Args->Callback) and (error IS ERR::Search)) return ERR::Okay;
            else return error;
         }
      }
      else {
         std::string_view sv;
         if (!xq->get(FID_ErrorMsg, sv)) Self->ErrorMsg.assign(sv);
         FreeResource(xq);
         return error;
      }
   }
   else return ERR::NewObject;
}

/*********************************************************************************************************************

-METHOD-
GetAttrib: Retrieves the value of a specific XML attribute from a tagged element.

The GetAttrib method provides efficient access to individual attribute values within XML elements.  Given a tag
identifier and attribute name, the method performs a case-insensitive search through the element's attribute
collection and returns the corresponding value.

When a specific attribute name is provided, the method searches through all attributes of the target tag.  The search
is case-insensitive to accommodate XML documents with varying capitalisation conventions.

When the attribute name is empty, the method returns the tag name itself, providing convenient access to
element names without requiring separate API calls.

For applications requiring frequent attribute access or high-performance scenarios, C++ developers should consider
direct access to the !XMLAttrib structure array.  This bypasses the method call overhead and provides immediate
access to all attributes simultaneously.

The method performs a linear search through the attribute collection, so performance scales with the number of
attributes per element.  For elements with many attributes, caching frequently accessed values may improve performance.

-INPUT-
int Index: The unique identifier of the XML tag to search.
strview Attrib: The name of the attribute to retrieve.  If empty, the element's tag name is returned instead.
^&string Value: Receives the attribute value.  It is cleared if the specified attribute does not exist.

-ERRORS-
Okay: The attribute was successfully found and its value returned.
NullArgs: Required arguments were not specified correctly.
NotFound: Either the specified tag Index does not exist, or the named attribute was not found within the tag.

-TAGS-
pure-query, case-insensitive

-END-

*********************************************************************************************************************/

static ERR XML_GetAttrib(extXML *Self, struct xml::GetAttrib *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Value)) return log.warning(ERR::NullArgs);
   Args->Value->clear();

   auto tag = Self->getTag(Args->Index);
   if (not tag) return log.warning(ERR::NotFound);

   if (Args->Attrib.empty()) {
      Args->Value->assign(tag->Attribs[0].Name);
      return ERR::Okay;
   }

   for (auto &attrib : tag->Attribs) {
      if (kt::iequals(Args->Attrib, attrib.Name)) {
         Args->Value->assign(attrib.Value);
         log.trace("Attrib %.*s = %s", int(Args->Attrib.size()), Args->Attrib.data(), Args->Value->c_str());
         return ERR::Okay;
      }
   }

   if ((Self->Flags & XMF::LOG_ALL) != XMF::NIL) {
      log.msg("Attrib %.*s not found in tag %d", int(Args->Attrib.size()), Args->Attrib.data(), Args->Index);
   }
   return ERR::NotFound;
}

/*********************************************************************************************************************

-METHOD-
GetContent: Extracts the immediate text content of an XML element, excluding nested tags.

The GetContent method extracts only the immediate content-node children of the specified element.  Text contained
inside nested elements is not included.

Consider the following XML structure:

<pre>
&lt;body&gt;
  Hello
  &lt;bold&gt;emphasis&lt;/bold&gt;
  world!
&lt;/body&gt;
</pre>

The GetContent method would extract `Hello world!` and deliberately exclude `emphasis` since it is contained within the
nested `&lt;bold&gt;` element.

For scenarios requiring serialised XML rather than immediate text content, use #Serialise().  To inspect nested text
content, traverse the child !XTag hierarchy directly.

It is recommended that C++ programs bypass this method and access the !XMLAttrib structure directly.

-INPUT-
int Index: The unique identifier of the XML element from which to extract content.
^&string Buffer: Receives the extracted content string.

-ERRORS-
Okay: The content string was successfully extracted and copied to the buffer.
NullArgs: Either the Buffer parameter was NULL or other required arguments were missing.
NotFound: The tag identified by Index does not exist in the XML structure.

-TAGS-
pure-query

-END-

*********************************************************************************************************************/

static ERR XML_GetContent(extXML *Self, struct xml::GetContent *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Buffer)) return log.warning(ERR::NullArgs);
   Args->Buffer->clear();

   if (auto tag = Self->getTag(Args->Index)) {
      if (not tag->Children.empty()) {
         for (auto &scan : tag->Children) {
            if (scan.Attribs.empty()) continue; // Sanity check (there should always be at least 1 attribute)

            if (scan.Attribs[0].isContent()) {
               Args->Buffer->append(scan.Attribs[0].Value);
            }
         }
      }

      return ERR::Okay;
   }
   else return log.warning(ERR::NotFound);
}

/*********************************************************************************************************************

-METHOD-
GetEntity: Retrieves the value of a parsed entity declaration.

This method returns the expanded value associated with a general entity parsed from the document's DOCTYPE declaration.
Entity names are case-sensitive and must match exactly as declared.

-INPUT-
strview Name: The name of the entity to retrieve.  This must correspond to a parsed entity declaration.
^&string Value: Receives the resolved entity value on success.

-ERRORS-
Okay: The entity was found and its value returned.
NullArgs: Either the Name was empty or Value parameter was NULL.
Search: No matching entity could be found for the specified name.

-TAGS-
pure-query, case-sensitive

-END-

*********************************************************************************************************************/

static ERR XML_GetEntity(extXML *Self, struct xml::GetEntity *Args)
{
   kt::Log log;

   if ((not Args) or Args->Name.empty() or (not Args->Value)) return log.warning(ERR::NullArgs);
   Args->Value->clear();

   auto it = Self->Entities.find(Args->Name);
   if (it IS Self->Entities.end()) return log.warning(ERR::Search);

   Args->Value->assign(it->second);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetNamespaceURI: Retrieve the namespace URI for a given namespace UID.

This method retrieves the original namespace URI string for a given namespace UID.

-INPUT-
uint NamespaceID: The UID of the namespace.
^&string Result: Receives the namespace URI.

-ERRORS-
Okay: The namespace URI was successfully retrieved.
NullArgs: Required arguments were not specified correctly.
Search: No namespace found for the specified UID.

-TAGS-
pure-query

-END-

*********************************************************************************************************************/

static ERR XML_GetNamespaceURI(extXML *Self, struct xml::GetNamespaceURI *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Result)) return log.warning(ERR::NullArgs);
   Args->Result->clear();

   auto uri = Self->getNamespaceURI(Args->NamespaceID);
   if (not uri) return log.warning(ERR::Search);

   Args->Result->assign(*uri);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetNotation: Retrieves information about a parsed notation declaration.

Returns the system or public identifier captured for a notation declaration inside the
document type definition.  If both public and system identifiers were provided they are
returned as a single string separated by a single space.

-INPUT-
strview Name: The notation name to look up.
^&string Value: Receives the notation descriptor on success.

-ERRORS-
Okay: The notation was found and its descriptor returned.
NullArgs: Either the Name was empty or Value parameter was NULL.
Search: No matching notation could be found for the specified name.

-TAGS-
pure-query, case-sensitive

-END-

*********************************************************************************************************************/

static ERR XML_GetNotation(extXML *Self, struct xml::GetNotation *Args)
{
   kt::Log log;

   if ((not Args) or Args->Name.empty() or (not Args->Value)) return log.warning(ERR::NullArgs);
   Args->Value->clear();

   auto it = Self->Notations.find(Args->Name);
   if (it IS Self->Notations.end()) return log.warning(ERR::Search);

   Args->Value->assign(it->second);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetTag: Returns a pointer to the !XTag structure for a given tag index.

This method will return the !XTag structure for a given tag `Index`.  The `Index` is checked to ensure it is valid
prior to retrieval, and an `ERR::NotFound` error will be returned if it is invalid.

-INPUT-
int Index:  The index of the tag that is being retrieved.
&struct(*XTag) Result: The !XTag is returned in this parameter.

-ERRORS-
Okay
NullArgs
NotFound: The Index is not recognised.

-TAGS-
pure-query, object-owns-result

-END-

*********************************************************************************************************************/

static ERR XML_GetTag(extXML *Self, struct xml::GetTag *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   if ((Args->Result = Self->getTag(Args->Index))) return ERR::Okay;
   else return ERR::NotFound;
}

//********************************************************************************************************************

static ERR XML_Init(extXML *Self)
{
   kt::Log log;

   if (Self->isDerived()) return ERR::Okay; // Break here for derived classes to perform initialisation

   // Statement has precedence over the Path because it's sometimes used in conjunction with the Statement when
   // a base path is required for relative URI resolution.

   if (not Self->Statement.empty()) {
      Self->LineNo = 1;
      if ((Self->ParseError = txt_to_xml(Self, Self->Tags, Self->Statement)) != ERR::Okay) {
         // Return NoSupport to defer parsing to other data handlers
         if (Self->ParseError IS ERR::InvalidData) return ERR::NoSupport;

         log.warning("XML parsing error #%d: %s", int(Self->ParseError), GetErrorMsg(Self->ParseError));
      }

      Self->Statement.clear();
      return Self->ParseError;
   }
   else if ((not Self->Path.empty()) or (Self->Source)) {
      if ((Self->Flags & XMF::NEW) != XMF::NIL) {
         return ERR::Okay;
      }
      else if (parse_source(Self) != ERR::Okay) {
         log.warning("XML parsing error: %s [File: %s]", GetErrorMsg(Self->ParseError), not Self->Path.empty() ? Self->Path.c_str() : "Object");
         return Self->ParseError;
      }
      else return ERR::Okay;
   }
   else {
      // NOTE: We do not fail if no data has been loaded into the XML object, the developer may be creating an XML data
      // structure from scratch, or could intend to send us information later.

      if ((Self->Flags & XMF::NEW) IS XMF::NIL) log.msg("Warning: No content given.");
      return ERR::Okay;
   }
}

/*********************************************************************************************************************

-METHOD-
InsertContent: Inserts text content into the XML document structure at specified positions.

The InsertContent method will insert content strings into any position within the XML tree.  A content string
must be provided in the `Content` parameter and the target insertion point is specified in the `Index` parameter.
An insertion point relative to the target index must be specified in the `Where` parameter.  The new tags can be
inserted as a child of the target by using a `Where` value of `XMI::CHILD`.  To insert behind or after the target, use
`XMI::PREV` or `XMI::NEXT`.

To modify existing content, call #SetAttrib() instead.

-INPUT-
int Index: The unique identifier of the target XML element that will serve as the reference point for insertion.
int(XMI) Where: Specifies the insertion position relative to the target element.  Use `PREV` or `NEXT` for sibling insertion, or `CHILD` for child content insertion.
strview Content: The text content to insert.  Special XML characters will be automatically escaped.
&int Result: Pointer to an integer that will receive the unique identifier of the newly created content node.

-ERRORS-
Okay: The content was successfully inserted and a new content node was created.
NullArgs: Required parameters were NULL or not properly specified.
NotFound: The target Index does not correspond to a valid XML element.
ReadOnly: The XML object is in read-only mode and cannot be modified.
Args: The Where parameter specifies an invalid insertion position.

-TAGS-
mutates-object, copies-input

-END-

*********************************************************************************************************************/

static ERR XML_InsertContent(extXML *Self, struct xml::InsertContent *Args)
{
   kt::Log log;

   if ((not Args) or Args->Content.empty()) return log.warning(ERR::NullArgs);
   if ((Self->Flags & XMF::READ_ONLY) != XMF::NIL) return log.warning(ERR::ReadOnly);
   if ((Self->Flags & XMF::LOG_ALL) != XMF::NIL) log.branch("Index: %d, Insert: %d", Args->Index, int(Args->Where));

   auto src = Self->getTag(Args->Index);
   if (not src) return log.warning(ERR::NotFound);

   std::ostringstream buffer;
   output_attribvalue(Args->Content, buffer);
   XTag content(glTagID++, 0, { { "", buffer.str() } });

   if (Args->Where IS XMI::NEXT) {
      CURSOR it;
      if (auto tags = Self->getInsert(src, it)) tags->insert(it, content);
      else return log.warning(ERR::NotFound);
   }
   else if (Args->Where IS XMI::CHILD) {
      src->Children.insert(src->Children.begin(), content);
   }
   else if (Args->Where IS XMI::PREV) {
      CURSOR it;
      if (auto tags = Self->getInsert(src, it)) {
        if (it IS tags->begin()) tags->insert(it, content);
        else tags->insert(it - 1, content);
      }
      else return log.warning(ERR::NotFound);
   }
   else return log.warning(ERR::Args);

   Args->Result = content.ID;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
InsertXML: Parse an XML string and insert it in the XML tree.

The InsertXML() method is used to translate and insert a new set of XML tags into any position within the XML tree.  A
standard XML statement must be provided in the XML parameter and the target insertion point is specified in the Index
parameter.  An insertion point relative to the target index must be specified in the `Where` parameter.  The new tags
can be inserted as a child of the target by using a `Where` value of `XMI::CHILD`.  Use `XMI::CHILD_END` to insert at
the end of the child list.  To insert behind or after the target, use `XMI::PREV` or `XMI::NEXT`.

-INPUT-
int Index: The new data will target the tag specified here.
int(XMI) Where: Use `PREV` or `NEXT` to insert behind or ahead of the target tag.  Use `CHILD` or `CHILD_END` for a child insert.
strview XML: An XML statement to parse.
&int Result: The resulting tag index.

-ERRORS-
Okay: The statement was added successfully.
Args: The Where parameter specifies an invalid insertion position.
NullArgs: Required parameters were NULL or not properly specified.
NotFound: The target Index does not correspond to a valid XML element.
ReadOnly: Changes to the XML data are not permitted.
NoData: The provided XML statement parsed to an empty result.
InvalidData
Syntax
Failed
UnbalancedXML

-TAGS-
mutates-object, copies-input

-END-

*********************************************************************************************************************/

static ERR XML_InsertXML(extXML *Self, struct xml::InsertXML *Args)
{
   kt::Log log;

   if ((not Args) or Args->XML.empty()) return log.warning(ERR::NullArgs);
   if ((Self->Flags & XMF::READ_ONLY) != XMF::NIL) return log.warning(ERR::ReadOnly);
   if ((Self->Flags & XMF::LOG_ALL) != XMF::NIL) {
      log.branch("Index: %d, Where: %d, XML: %.*s", Args->Index, int(Args->Where),
         std::min(40, int(Args->XML.size())), Args->XML.data());
   }

   auto src = Self->getTag(Args->Index);
   if (not src) return log.warning(ERR::NotFound);

   ERR error;
   TAGS insert;
   if ((error = txt_to_xml(Self, insert, Args->XML)) != ERR::Okay) return log.warning(error);
   if (insert.empty()) return ERR::NoData;
   auto result = insert[0].ID;

   XTag *parent_scope = nullptr;
   if ((Args->Where IS XMI::CHILD) or (Args->Where IS XMI::CHILD_END)) parent_scope = src;
   else if (src->ParentID) parent_scope = Self->getTag(src->ParentID);

   if (Args->Where IS XMI::NEXT) {
      CURSOR it;
      if (auto tags = Self->getInsert(src, it)) {
         tags->insert(it + 1, insert.begin(), insert.end());
      }
      else return log.warning(ERR::NotFound);
   }
   else if (Args->Where IS XMI::PREV) {
      CURSOR it;
      if (auto tags = Self->getInsert(src, it)) {
        if (it IS tags->begin()) tags->insert(it, insert.begin(), insert.end());
        else tags->insert(it, insert.begin(), insert.end());
      }
      else return log.warning(ERR::NotFound);
   }
   else if (Args->Where IS XMI::CHILD) {
      src->Children.insert(src->Children.begin(), insert.begin(), insert.end());
   }
   else if (Args->Where IS XMI::CHILD_END) {
      src->Children.insert(src->Children.end(), insert.begin(), insert.end());
   }
   else if (Args->Where IS XMI::END) {
      if (auto tags = Self->getTags(src)) {
         tags->insert(tags->end() - 1, insert.begin(), insert.end());
      }
      else return log.warning(ERR::NotFound);
   }
   else return log.warning(ERR::Args);

   refresh_base_uris_for_insert(Self, insert, parent_scope);

   Args->Result = result;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
InsertXPath: Inserts an XML statement in an XML tree.

The InsertXPath method is used to translate and insert a new set of XML tags into any position within the XML tree.  A
standard XML statement must be provided in the XML parameter and the target insertion point is referenced as a valid
`XPath` location string.  An insertion point relative to the `XPath` target must be specified in the `Where`
parameter.  The new tags can be inserted as a child of the target by using a `Where` value of `XMI::CHILD` or
`XMI::CHILD_END`.  To insert behind or after the target, use `XMI::PREV` or `XMI::NEXT`.

-INPUT-
strview XPath: An XPath string that refers to the target insertion point.
int(XMI) Where: Use `PREV` or `NEXT` to insert behind or ahead of the target tag.  Use `CHILD` for a child insert.
strview XML: XML statement to parse and insert.
&int Result: The index of the new tag is returned here.

-ERRORS-
Okay: The XML statement was successfully inserted at the specified XPath location.
NullArgs: Required parameters were NULL or not properly specified.
Args: The Where parameter specifies an invalid insertion position.
Search: The XPath could not be resolved to a valid location.
NotFound: The target resolved by XPath could not be used as an insertion point.
ReadOnly: The XML object is in read-only mode and cannot be modified.
NoData: The provided XML statement parsed to an empty result.
NewObject
InvalidData
Syntax
Failed
UnbalancedXML
Terminate

-TAGS-
mutates-object, copies-input

-END-

*********************************************************************************************************************/

ERR XML_InsertXPath(extXML *Self, struct xml::InsertXPath *Args)
{
   kt::Log log;

   if ((not Args) or Args->XPath.empty() or Args->XML.empty()) return log.warning(ERR::NullArgs);
   if ((Self->Flags & XMF::READ_ONLY) != XMF::NIL) return log.warning(ERR::ReadOnly);

   log.branch("Insert: %d, XPath: %.*s", int(Args->Where), int(Args->XPath.size()), Args->XPath.data());

   objXQuery *xq;
   if (!NewObject(CLASSID::XQUERY, NF::NIL, (OBJECTPTR *)&xq)) {
      xq->set(FID_Statement, Args->XPath);
      if (auto error = xq->init(); !error) {
         matching_tag_opt opt;
         auto callback = C_FUNCTION(save_matching_tag, &opt);
         if (error = xq->search((objXML *)Self, callback, 0, XEF::NIL); error IS ERR::Terminate) {
            xml::InsertXML insert { .Index = opt.tag_id, .Where = Args->Where, .XML = Args->XML };
            if (error = XML_InsertXML(Self, &insert); !error) {
               Args->Result = insert.Result;
            }
            FreeResource(xq);
            return error;
         }
         else {
            if (!error) {
               error = ERR::Search; // No match found
               Self->ErrorMsg = "XPath did not resolve to a valid location.";
            }
            else {
               std::string_view sv;
               if (!xq->get(FID_ErrorMsg, sv)) Self->ErrorMsg.assign(sv);
            }
            FreeResource(xq);
            return error;
         }
      }
      else {
         std::string_view sv;
         if (!xq->get(FID_ErrorMsg, sv)) Self->ErrorMsg.assign(sv);
         FreeResource(xq);
         return error;
      }
   }
   else return log.warning(ERR::NewObject);
}

/*********************************************************************************************************************

-METHOD-
MoveTags: Move an XML tag group to a new position in the XML tree.

This method is used to move XML tags within the XML tree structure.  It supports the movement of single and groups of
tags from one index to another.  The client must supply the index of the tag that will be moved and the index of the
target tag.  All child tags of the source will be included in the move.

An insertion point relative to the target index must be specified in the `Where` parameter.  The source tag can be
inserted as a child of the destination by using `XMI::CHILD` or `XMI::CHILD_END`.  To insert behind or after the
target, use `XMI::PREV` or `XMI::NEXT`.

-INPUT-
int Index: Index of the source tag to be moved.
int Total: The total number of sibling tags, including the targeted tag, to move from the source index.  Minimum value of 1.
int DestIndex: The destination tag index.
int(XMI) Where: Use `PREV` or `NEXT` to insert behind or ahead of the target tag.  Use `CHILD` or `CHILD_END` for a child insert.

-ERRORS-
Okay: Tags were moved successfully.
Args: Invalid parameter values were provided.
NullArgs: Required parameters were NULL or not properly specified.
NotFound: Either the source or destination tag index does not exist.
ReadOnly: The XML object is in read-only mode and cannot be modified.
SanityCheckFailed: An internal consistency check failed during the move operation.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR XML_MoveTags(extXML *Self, struct xml::MoveTags *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if ((Self->Flags & XMF::READ_ONLY) != XMF::NIL) return log.warning(ERR::ReadOnly);
   if (Args->Total < 1) return log.warning(ERR::Args);
   if (Args->Index IS Args->DestIndex) return ERR::Okay;

   if ((Args->DestIndex > Args->Index) and (Args->DestIndex < Args->Index + Args->Total)) {
      return log.warning(ERR::Args);
   }

   auto dest = Self->getTag(Args->DestIndex);
   if (not dest) return log.warning(ERR::NotFound);

   // Take a copy of the source tags

   CURSOR it;
   auto src_tags = Self->getInsert(Args->Index, it);
   if (not src_tags) return log.warning(ERR::NotFound);

   int total = Args->Total;
   TAGS copy;
   unsigned si;
   for (si=0; si < src_tags->size(); si++) {
      if (src_tags[0][si].ID IS it->ID) {
         if (si + total > src_tags->size()) total = int(src_tags->size() - si);
         copy = TAGS(src_tags->begin() + si, src_tags->begin() + si + total);
         break;
      }
   }

   if (copy.empty()) return log.warning(ERR::SanityCheckFailed);
   if (si >= src_tags->size()) return log.warning(ERR::SanityCheckFailed);

   switch (Args->Where) {
      case XMI::PREV: {
         CURSOR it;
         if (auto target = Self->getInsert(dest, it)) {
            target->insert(it, copy.begin(), copy.end());
         }
         else return log.warning(ERR::NotFound);
         break;
      }

      case XMI::NEXT: {
         CURSOR it;
         if (auto target = Self->getInsert(dest, it)) {
            target->insert(it + 1, copy.begin(), copy.end());
         }
         else return log.warning(ERR::NotFound);
         break;
      }

      case XMI::CHILD:
         dest->Children.insert(dest->Children.begin(), copy.begin(), copy.end());
         break;

      case XMI::CHILD_END:
         dest->Children.insert(dest->Children.end(), copy.begin(), copy.end());
         break;

      default:
         return log.warning(ERR::Args);
   }

   src_tags->erase(src_tags->begin() + si, src_tags->begin() + si + Args->Total);

   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
RegisterNamespace: Register a namespace URI and return its UID.

This method registers a namespace URI and returns a UID that can be used to identify the namespace
efficiently throughout the XML document.

-INPUT-
strview URI: The namespace URI to register. Must not be empty.
&uint Result: Pointer to an integer that will receive the UID for the namespace URI.

-ERRORS-
Okay: The namespace was successfully registered.
NullArgs: Required arguments were not specified correctly.
Args: The URI could not be registered.

-TAGS-
mutates-object, copies-input

-END-

*********************************************************************************************************************/

static ERR XML_RegisterNamespace(extXML *Self, struct xml::RegisterNamespace *Args)
{
   kt::Log log;

   if ((not Args) or Args->URI.empty()) return log.warning(ERR::NullArgs);

   // Register the namespace URI and get its hash
   auto hash = Self->registerNamespace(Args->URI);
   if (not hash) return log.warning(ERR::Args);

   Args->Result = hash;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
RemoveTag: Removes tag(s) from the XML structure.

The RemoveTag method is used to remove one or more tags from an XML structure.  Child tags will automatically be
discarded as a consequence of using this method, in order to maintain a valid XML structure.

This method can delete multiple sibling tags when the `Total` parameter is greater than 1.  Each selected tag and its
children are removed.  The requested range should stay within the available sibling sequence.

Note: Removing tags will destabilise all cached address pointers that have been acquired from the XML object.

-INPUT-
int Index: Reference to the tag that will be removed.
int Total: The total number of sibling tags to delete, including the indicated tag.  A value of one or less removes only the indicated tag and its children.  The requested range should stay within the available sibling sequence.

-ERRORS-
Okay: The tag(s) were successfully removed.
NullArgs: Required parameters were NULL or not properly specified.
NotFound: The specified tag Index does not exist in the XML structure.
ReadOnly: The XML object is in read-only mode and cannot be modified.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR XML_RemoveTag(extXML *Self, struct xml::RemoveTag *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if ((Self->Flags & XMF::READ_ONLY) != XMF::NIL) return log.warning(ERR::ReadOnly);
   if ((Self->Flags & XMF::LOCK_REMOVE) != XMF::NIL) return log.warning(ERR::ReadOnly);

   int count = Args->Total;
   if (count < 1) count = 1;

   if (auto tag = Self->getTag(Args->Index)) {
      if (tag->ParentID) {
         if (auto parent = Self->getTag(tag->ParentID)) {
            for (auto it=parent->Children.begin(); it != parent->Children.end(); it++) {
               if (it->ID IS Args->Index) {
                  parent->Children.erase(it, it + count);
                  Self->modified();
                  return ERR::Okay;
               }
            }
         }
      }
      else { // No parent, access the root level
         for (auto it=Self->Tags.begin(); it != Self->Tags.end(); it++) {
            if (it->ID IS Args->Index) {
               Self->Tags.erase(it, it + count);
               Self->modified();
               return ERR::Okay;
            }
         }
      }
   }

   return log.warning(ERR::NotFound);
}

/*********************************************************************************************************************

-METHOD-
RemoveXPath: Removes tag(s) from the XML structure using an XPath lookup.

The RemoveXPath method is used to remove one or more tags from an XML structure.  Child tags will automatically be
discarded as a consequence of using this method, in order to maintain a valid XML structure.

Individual tag attributes can also be removed if an attribute is referenced at the end of the `XPath`.

The removal routine is repeated until no further match is found or the `Limit` is reached.  No-match completion is
reported as `ERR::Okay`.

This method is volatile and will destabilise any cached address pointers that have been acquired from the XML object.

-INPUT-
strview XPath: An XML path string.
int Limit: The maximum number of matching tags to delete.  A value of one or zero removes only the indicated tag and its children.  A value of -1 removes all matching tags.

-ERRORS-
Okay: The matching tag(s) or attribute(s) were successfully removed.
NullArgs: Required parameters were NULL or not properly specified.
ReadOnly: The XML object is in read-only mode and cannot be modified.
NoData: The XML document contains no data to process.
Terminate

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR XML_RemoveXPath(extXML *Self, struct xml::RemoveXPath *Args)
{
   kt::Log log;

   if ((not Args) or Args->XPath.empty()) return ERR::NullArgs;

   if (Self->Tags.empty()) return ERR::NoData;
   if ((Self->Flags & XMF::READ_ONLY) != XMF::NIL) return log.warning(ERR::ReadOnly);
   if ((Self->Flags & XMF::LOCK_REMOVE) != XMF::NIL) return log.warning(ERR::ReadOnly);

   auto limit = Args->Limit;
   if (limit IS -1) limit = 0x7fffffff;
   else if (not limit) limit = 1;

   objXQuery *xq;
   if (!NewObject(CLASSID::XQUERY, NF::NIL, (OBJECTPTR *)&xq)) {
      xq->set(FID_Statement, Args->XPath);
      if (auto error = xq->init(); !error) {
         while (limit > 0) {
            matching_tag_opt opt;
            auto callback = C_FUNCTION(save_matching_tag, &opt);
            if (xq->search((objXML *)Self, callback, 0, XEF::NIL) != ERR::Terminate) break;
            auto tag = Self->getTag(opt.tag_id);
            if (!tag) break; // Sanity check

            if (not opt.attrib.empty()) { // Remove an attribute
               auto it = std::ranges::find_if(tag->Attribs, [&](const auto& a) {
                  return kt::iequals(opt.attrib, a.Name);
               });
               if (it != tag->Attribs.end()) tag->Attribs.erase(it);
            }
            else if (tag->ParentID) {
               if (auto parent = Self->getTag(tag->ParentID)) {
                  auto it = std::ranges::find_if(parent->Children, [&](const auto& child) {
                     return tag->ID IS child.ID;
                  });
                  if (it != parent->Children.end()) parent->Children.erase(it);
               }
            }
            else {
               auto it = std::ranges::find_if(Self->Tags, [&](const auto& scan) {
                  return tag->ID IS scan.ID;
               });
               if (it != Self->Tags.end()) Self->Tags.erase(it);
            }
            limit--;
         }
      }
      else {
         std::string_view sv;
         if (!xq->get(FID_ErrorMsg, sv)) Self->ErrorMsg.assign(sv);
         FreeResource(xq);
         return error;
      }
      FreeResource(xq);
   }

   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Reset: Clears the information held in an XML object.
-END-
*********************************************************************************************************************/

static ERR XML_Reset(extXML *Self)
{
   return acClear(Self);
}

/*********************************************************************************************************************

-METHOD-
ResolvePrefix: Resolve a namespace prefix to the UID of its namespace URI within a tag's scope.

This method resolves a namespace prefix to its corresponding namespace URI hash by examining namespace declarations
within the specified tag's hierarchical scope.  The resolution process:

<list type="ordered">
<li>Walks up the tag hierarchy from the specified tag to the root.</li>
<li>Examines xmlns:prefix and xmlns attributes in each tag to find the declaration.</li>
<li>Returns the UID of the first matching namespace URI found.</li>
</list>

This approach correctly handles nested namespace scopes and prefix redefinitions.

-INPUT-
strview Prefix: The namespace prefix to resolve.  Use an empty string for the default namespace.
int TagID: The tag ID defining the starting scope for namespace resolution.
&uint Result: Receives the resolved namespace URI hash.

-ERRORS-
Okay: The prefix was successfully resolved.
NullArgs: Required arguments were not specified correctly.
Search: The prefix could not be resolved in any accessible scope, or TagID did not identify a tag.

-TAGS-
pure-query

-END-

*********************************************************************************************************************/

static ERR XML_ResolvePrefix(extXML *Self, struct xml::ResolvePrefix *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   return Self->resolvePrefix(Args->Prefix, Args->TagID, Args->Result);
}

/*********************************************************************************************************************
-ACTION-
SaveToObject: Saves XML data to a storage object (e.g. @File).
-END-
*********************************************************************************************************************/

static ERR XML_SaveToObject(extXML *Self, struct acSaveToObject *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Dest)) return log.warning(ERR::NullArgs);
   if (Self->Tags.size() <= 0) return ERR::Okay;

   log.traceBranch("To: %d", Args->Dest->UID);

   std::string str;
   if (auto error = Self->serialise(0, XMF::READABLE|XMF::INCLUDE_SIBLINGS, str); !error) {
      if (acWrite(Args->Dest, str.c_str(), int(str.size()), nullptr) != ERR::Okay) error = ERR::Write;
      return error;
   }
   else return error;
}

/*********************************************************************************************************************

-METHOD-
Serialise: Serialise part of the XML tree to an XML string.

The Serialise() method will serialise all or part of the XML data tree to a string.

The string will be copied into the caller-provided Result parameter.

-INPUT-
int Index: Index to a source tag for which serialisation will start.  Set to zero to serialise the entire tree.
int(XMF) Flags: Use `INCLUDE_SIBLINGS` to include siblings of the tag found at Index.
^&string Result: The resulting string is returned in this parameter.

-ERRORS-
Okay: The XML string was successfully serialised.
NullArgs: Required parameters were NULL or not properly specified.
NoData: No information has been loaded into the XML object.
NotFound: The specified tag Index does not exist in the XML structure.

-TAGS-
pure-query

-END-

*********************************************************************************************************************/

static ERR XML_Serialise(extXML *Self, struct xml::Serialise *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Result)) return log.warning(ERR::NullArgs);
   Args->Result->clear();
   if (Self->Tags.empty()) return log.warning(ERR::NoData);

   log.traceBranch("Tag: %d", Args->Index);

   std::ostringstream buffer;

   auto tag = Args->Index ? Self->getTag(Args->Index) : &Self->Tags[0];
   if (not tag) return log.warning(ERR::NotFound);

   if ((Args->Flags & XMF::INCLUDE_SIBLINGS) != XMF::NIL) {
      if (auto parent = Self->getTag(tag->ParentID)) {
         auto it = parent->Children.begin();
         for (; it != parent->Children.end(); it++) {
            if (it->ID IS Args->Index) break;
         }

         for (; it != parent->Children.end(); it++) {
            serialise_xml(*it, buffer, Args->Flags);
         }
      }
      else {
         auto it = Self->Tags.begin();
         for (; it != Self->Tags.end(); it++) {
            if (it IS tag) break;
         }

         for (; it != Self->Tags.end(); it++) {
            serialise_xml(*it, buffer, Args->Flags);
         }
      }
   }
   else serialise_xml(*tag, buffer, Args->Flags);

   Args->Result->assign(buffer.str());
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetAttrib: Adds, updates and removes XML attributes.

This method updates existing XML tag attributes, creates new attributes and clears content.

The data for the attribute is defined in the `Name` and `Value` parameters.  Use an empty string if no data is to be
associated with the attribute.  Set `Attrib` to `XMS::REMOVE` to remove an attribute, or to clear element content if
`Name` is empty.

NOTE: The attribute at position 0 declares the name of the tag and should not normally be accompanied with a value
declaration.  However, if the tag represents content within its parent, then the Name must be left empty and the
`Value` string will determine the content.

-INPUT-
int Index: Identifies the tag that is to be updated.
int(XMS) Attrib: Attribute index to update, or `NEW`, `UPDATE`, `UPDATE_ONLY` or `REMOVE`.
strview Name: New attribute name.  If empty during indexed updates, the name is not changed.  If Attrib is `UPDATE`, `UPDATE_ONLY` or `REMOVE`, this name is used to find the attribute.
strview Value: String containing the new value for the attribute.

-ERRORS-
Okay
NullArgs
Args
OutOfRange: The `Attrib` value is outside the tag's attribute array.
Search: The tag identified by `Index` or the attribute identified by `Name` could not be found.
ReadOnly: The XML object is read-only.

-TAGS-
mutates-object, copies-input, case-insensitive
-END-

*********************************************************************************************************************/

static ERR XML_SetAttrib(extXML *Self, struct xml::SetAttrib *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if ((Self->Flags & XMF::READ_ONLY) != XMF::NIL) return log.warning(ERR::ReadOnly);

   log.trace("Tag: %d, Attrib: $%.8x, %.*s = '%.*s'", Args->Index, int(Args->Attrib),
      int(Args->Name.size()), Args->Name.data(), int(Args->Value.size()), Args->Value.data());

   auto tag = Self->getTag(Args->Index);
   if (not tag) return log.warning(ERR::Search);

   auto cmd = Args->Attrib;
   if (cmd IS XMS::REMOVE) {
      if (Args->Name.empty()) {
         if (tag->isContent()) tag->Attribs[0].Value.clear();
         else {
            for (auto &child : tag->Children) {
               if (child.isContent()) child.Attribs[0].Value.clear();
            }
         }
         Self->Modified++;
         return ERR::Okay;
      }

      auto it = std::ranges::find_if(tag->Attribs, [&](const auto& a) {
         return kt::iequals(Args->Name, a.Name);
      });

      if (it != tag->Attribs.end()) {
         tag->Attribs.erase(it);
         Self->Modified++;
         return ERR::Okay;
      }

      return ERR::Search;
   }
   else if ((cmd IS XMS::UPDATE) or (cmd IS XMS::UPDATE_ONLY)) {
      if (Args->Name.empty()) return log.warning(ERR::NullArgs);

      auto it = std::ranges::find_if(tag->Attribs, [&](const auto& a) {
         return kt::iequals(Args->Name, a.Name);
      });

      if (it != tag->Attribs.end()) {
         it->Name.assign(Args->Name);
         it->Value.assign(Args->Value);
         Self->Modified++;
         return ERR::Okay;
      }

      if (cmd IS XMS::UPDATE) {
         tag->Attribs.emplace_back(std::string(Args->Name), std::string(Args->Value));
         Self->Modified++;
         return ERR::Okay;
      }
      else return ERR::Search;
   }
   else if (cmd IS XMS::NEW) {
      if (Args->Name.empty()) return log.warning(ERR::NullArgs);
      tag->Attribs.emplace_back(std::string(Args->Name), std::string(Args->Value));
      Self->Modified++;
      return ERR::Okay;
   }

   // Attribute indexing

   if ((int(Args->Attrib) < 0) or (int(Args->Attrib) >= int(tag->Attribs.size()))) return log.warning(ERR::OutOfRange);

   if (not Args->Name.empty()) tag->Attribs[int(Args->Attrib)].Name.assign(Args->Name);
   tag->Attribs[int(Args->Attrib)].Value.assign(Args->Value);

   Self->Modified++;
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
SetKey: Sets attributes and content in the XML tree using XPaths.

Use SetKey to add tag attributes and content using XPaths.  The XPath is specified in the `Key` parameter and the data
is specified in the `Value` parameter.  If the XPath resolves to an attribute, the attribute is updated or created on
the matched tag.  If the XPath resolves to an element, the element's first content node is updated or a new content
node is created.

It is not possible to add new tags using this action - it is only possible to update existing tags.

Please note that making changes to the XML tree will render all previously obtained tag pointers and indexes invalid.

-INPUT-
strview Key: XPath expression identifying the element or attribute to update.
strview Value: New attribute or content value.

-ERRORS-
Okay
NullArgs
ReadOnly: Changes to the XML structure are not permitted.
Search: Failed to find the tag referenced by the XPath.
SanityCheckFailed
NewObject
Terminate

-TAGS-
mutates-object, copies-input

-END-

*********************************************************************************************************************/

static ERR XML_SetKey(extXML *Self, struct acSetKey *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if ((Self->Flags & XMF::READ_ONLY) != XMF::NIL) return log.warning(ERR::ReadOnly);

   objXQuery *xq;
   if (!NewObject(CLASSID::XQUERY, NF::NIL, (OBJECTPTR *)&xq)) {
      xq->set(FID_Statement, std::string_view(Args->Key));
      if (auto error = xq->init(); !error) {
         matching_tag_opt opt;
         auto callback = C_FUNCTION(save_matching_tag, &opt);
         if (error = xq->search((objXML *)Self, callback, 0, XEF::NIL); error IS ERR::Terminate) {
            auto tag = Self->getTag(opt.tag_id);
            if (not tag) {
               FreeResource(xq);
               return log.warning(ERR::SanityCheckFailed);
            }

            if (not opt.attrib.empty()) { // Updating or adding an attribute
               auto it = std::ranges::find_if(tag->Attribs, [&](const auto& a) {
                  return kt::iequals(opt.attrib, a.Name);
               });

               if (it != tag->Attribs.end()) it->Value = Args->Value; // Modify existing
               else tag->Attribs.emplace_back(std::string(opt.attrib), std::string(Args->Value)); // Add new
               Self->Modified++;
            }
            else if (not tag->Children.empty()) { // Update existing content
               tag->Children[0].Attribs[0].Value = Args->Value;
               Self->Modified++;
            }
            else {
               tag->Children.emplace_back(XTag(glTagID++, 0, {
                  { "", std::string(Args->Value) }
               }));
               Self->modified();
            }
            FreeResource(xq);
            return ERR::Okay;
         }
         else {
            if (!error) {
               Self->ErrorMsg = "XPath did not resolve to a valid location.";
               error = ERR::Search;
            }
            else {
               std::string_view sv;
               if (!xq->get(FID_ErrorMsg, sv)) Self->ErrorMsg.assign(sv);
               FreeResource(xq);
            }
            log.warning("Failed to find '%.*s'", int(Args->Key.size()), Args->Key.data());
            return error;
         }
      }
      else {
         std::string_view sv;
         if (!xq->get(FID_ErrorMsg, sv)) Self->ErrorMsg.assign(sv);
         FreeResource(xq);
         log.msg("Failed to compile '%.*s'", int(Args->Key.size()), Args->Key.data());
         return error;
      }
   }
   else return log.warning(ERR::NewObject);
}

/*********************************************************************************************************************

-METHOD-
SetTagNamespace: Set the namespace for a specific XML tag.

This method assigns a namespace to an XML tag using the namespace's UID.

-INPUT-
int TagID: The unique identifier of the XML tag.
int NamespaceID: The UID of the namespace to assign.

-ERRORS-
Okay: The namespace was successfully assigned to the tag.
NullArgs: Required arguments were not specified correctly.
NotFound: The specified tag was not found.

-TAGS-
mutates-object

-END-

*********************************************************************************************************************/

static ERR XML_SetTagNamespace(extXML *Self, struct xml::SetTagNamespace *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   // Find the tag and assign the namespace
   auto tag = Self->getTag(Args->TagID);
   if (not tag) return log.warning(ERR::NotFound);

   tag->NamespaceID = Args->NamespaceID;
   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Sort: Sorts XML tags to your specifications.

The Sort method is used to sort a single branch of XML tags in ascending or descending order.  An `XPath` is required
that refers to the tag containing each item that will be sorted.  To sort the root level, use an empty `XPath`.

The `Sort` parameter is used to specify a list of sorting instructions.  The format for the `Sort` string is
`Tag:Attrib,Tag:Attrib,...`.  The `Tag` indicates the tag name that should be identified for sorting each node, and
child tags are supported for this purpose.  Wildcard filtering is allowed and a `Tag` value of `*` will match every
tag at the requested `XPath` level.  The optional `Attrib` value names the attribute containing the sort string.  To
sort on content, do not define an `Attrib` value (use the format `Tag,Tag,...`).

-INPUT-
strview XPath: Sort everything under the specified tag, or empty to sort the entire top level.
strview Sort: A sorting instruction string.
int(XSF) Flags: Optional flags.

-ERRORS-
Okay: The XML object was successfully sorted.
NullArgs: Required parameters were NULL or not properly specified.
Search: The provided XPath failed to locate a tag.
ReadOnly: The XML object is in read-only mode and cannot be modified.
NewObject
Terminate

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR XML_Sort(extXML *Self, struct xml::Sort *Args)
{
   kt::Log log;

   if ((not Args) or Args->Sort.empty()) return log.warning(ERR::NullArgs);
   if ((Self->Flags & XMF::READ_ONLY) != XMF::NIL) return log.warning(ERR::ReadOnly);

   CURSOR tag;
   TAGS *branch;
   if (Args->XPath.empty()) {
      branch = &Self->Tags;
      tag = &Self->Tags[0];
      if (not tag) return ERR::Okay;
   }
   else {
      objXQuery *xq;
      if (!NewObject(CLASSID::XQUERY, NF::NIL, (OBJECTPTR *)&xq)) {
         xq->set(FID_Statement, Args->XPath);
         if (auto error = xq->init(); !error) {
            matching_tag_opt opt;
            auto callback = C_FUNCTION(save_matching_tag, &opt.tag_id);
            if (error = xq->search((objXML *)Self, callback, 0, XEF::NIL); error != ERR::Terminate) {
               std::string_view sv;
               if (!xq->get(FID_ErrorMsg, sv)) Self->ErrorMsg.assign(sv);
               FreeResource(xq);
               return log.warning(ERR::Search);
            }

            FreeResource(xq);

            branch = &Self->Map[opt.tag_id]->Children;
         }
         else {
            std::string_view sv;
            if (!xq->get(FID_ErrorMsg, sv)) Self->ErrorMsg.assign(sv);
            FreeResource(xq);
            return log.warning(error);
         }
      }
      else return log.warning(ERR::NewObject);
   }

   if (branch->size() < 2) return ERR::Okay;

   log.traceBranch("Path: %.*s, Tag: %.*s", int(Args->XPath.size()), Args->XPath.data(),
      int(Args->Sort.size()), Args->Sort.data());

   std::vector<std::pair<std::string, std::string>> filters;
   std::string cmd(Args->Sort);
   for (unsigned i=0; i < cmd.size(); ) {
      std::string tag_match, attrib_match;

      // Format is a CSV list of "Tag:Attrib,..."

      auto end = cmd.find_first_of(":,", i);
      if (end IS std::string::npos) end = cmd.size();
      tag_match.assign(cmd, i, end-i);
      i = end;

      if (cmd[i] IS ':') {
         end = cmd.find(',', ++i);
         if (end IS std::string::npos) end = cmd.size();
         attrib_match.assign(cmd, i, end-i);
         i = end;
      }

      filters.push_back(make_pair(tag_match, attrib_match));

      i = cmd.find(",", i); // Next filter
      if (i < cmd.size()) i++; // Skip comma
   }

   // Build a sorting array (extract a sort value for each tag and keep a tag reference).

   struct ListSort {
      XTag *Tag;
      std::string Value;
      ListSort(XTag *pTag, const std::string pValue) : Tag(pTag), Value(pValue) { }
   };

   std::vector<ListSort> list;
   for (auto &scan : branch[0]) {
      std::string sortval;
      for (auto &filter : filters) {
         XTag *tag = nullptr;
         // Check for matching tag name, either at the current tag or in one of the child tags underneath it.
         if (kt::wildcmp(filter.first, scan.Attribs[0].Name)) {
            tag = &scan;
         }
         else {
            auto child_it = std::ranges::find_if(scan.Children, [&](const auto& child) {
               return kt::wildcmp(filter.first, child.Attribs[0].Name);
            });
            if (child_it != scan.Children.end()) tag = &(*child_it);
         }

         if (not tag) break;

         if ((Args->Flags & XSF::CHECK_SORT) != XSF::NIL) { // Give precedence for a 'sort' attribute in the XML tag
            auto attrib_view = tag->Attribs | std::views::drop(1);
            auto attrib_it = std::ranges::find_if(attrib_view, [](const auto& a) {
               return kt::iequals("sort", a.Name);
            });

            if (attrib_it != attrib_view.end()) {
               sortval.append(attrib_it->Value);
               sortval.append("\x01");
            }
            else continue;
         }

         if (filter.second.empty()) { // Use content as the sort data
            for (auto &child : tag->Children | std::views::filter([](const auto& c) { return c.isContent(); })) {
               sortval += child.Attribs[0].Value;
            }
         }
         else { // Extract the sort data from the specified tag attribute
            auto attrib_view = tag->Attribs | std::views::drop(1);
            auto attrib_it = std::ranges::find_if(attrib_view, [&](const auto& a) {
               return kt::wildcmp(filter.second, a.Name);
            });
            if (attrib_it != attrib_view.end()) sortval += attrib_it->Value;
         }

         // Each string in the sort list is separated with a byte value of 0x01

         sortval.append("\x01");
      }

      list.emplace_back(&scan, sortval);
   }

   if ((Args->Flags & XSF::DESC) != XSF::NIL) {
      std::ranges::sort(list, std::ranges::greater{}, &ListSort::Value);
   }
   else {
      std::ranges::sort(list, std::ranges::less{}, &ListSort::Value);
   }

   // Build new tag list for this branch, then apply it.

   TAGS new_branch(branch->size());
   for (auto &rec : list) new_branch.emplace_back(rec.Tag[0]);
   branch[0] = std::move(new_branch);

   Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DocType: Root element name from a parsed DOCTYPE declaration.

-FIELD-
ErrorMsg: A textual description of the last parse error.

This field may provide a textual description of the last parse error that occurred, in conjunction with the most
recently received error code.  Issues parsing malformed XPath expressions may also be reported here.

-FIELD-
Flags: Controls XML parsing behaviour and processing options.

-FIELD-
Path: Set this field if the XML document originates from a file source.

XML documents can be loaded from the file system by specifying a file path in this field.  If set post-initialisation,
all currently loaded data will be cleared and the file will be parsed automatically.

The XML class supports ~Core.LoadFile(), so an XML file can be pre-cached by the program if it is frequently used
during a program's life cycle.

In the case where a #Statement has been defined, setting the Path with a folder reference may be necessary to
establish the base path for relative references in XQuery statements (e.g. for importing XQuery modules).

*********************************************************************************************************************/

static ERR GET_Path(extXML *Self, std::string_view &Value)
{
   Value = Self->Path;
   return ERR::Okay;
}

static ERR SET_Path(extXML *Self, const std::string_view &Value)
{
   if (Self->Source) SET_Source(Self, nullptr);
   if (not Self->Path.empty()) Self->Path.clear();

   if (Value.starts_with("string:")) {
      // If the string: path type is used then we can optimise things by setting the following path string as the
      // statement.

      auto statement = Value;
      statement.remove_prefix(7);
      return SET_Statement(Self, statement);
   }
   else if (not Value.empty()) {
      Self->Path = Value;
      if (Self->initialised()) {
         parse_source(Self);
         return Self->ParseError;
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Modified: A timestamp of when the XML data was last modified.

The Modified field provides an artificial timestamp value of when the XML data was last modified (e.g. by a tag insert
or update).  Storing the current Modified value and making comparisons later makes it easy to determine that a change
has been made.  A rough idea of the total number of change requests can also be calculated by subtracting out the
difference.

-FIELD-
PublicID: Public identifier from a parsed DOCTYPE declaration.

-FIELD-
SystemID: System identifier from a parsed DOCTYPE declaration.

-FIELD-
Source: Set this field if the XML data is to be sourced from another object.

An XML document can be loaded from another object by referencing it here, on the condition that the object's class
supports the Read action.

If set post-initialisation, all currently loaded data will be cleared and the source object will be parsed
automatically.

*********************************************************************************************************************/

static ERR SET_Source(extXML *Self, OBJECTPTR Value)
{
   if (not Self->Path.empty()) Self->Path.clear();
   Self->Statement.clear();

   if (Value) {
      Self->Source = Value;
      if (Self->initialised()) {
         parse_source(Self);
         return Self->ParseError;
      }
   }
   else Self->Source = nullptr;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Statement: XML data is processed through this field.

Set the Statement field to parse an XML formatted data string through the object.  If this field is set after
initialisation then the XML object will clear any existing data first.

Be aware that setting this field with an invalid statement will result in an empty XML object.

Reading the Statement field returns a serialised string of XML data.  By default, all tags are included in the
statement.  The string result is an allocation that must be freed by the caller.

If the statement is an XQuery expression with base-uri references, the #Path field should be set to establish
the base path for relative references.

-END-

*********************************************************************************************************************/

static ERR GET_Statement(extXML *Self, std::string_view &Value)
{
   kt::Log log;

   if (not Self->initialised()) {
      if (CSTRING str = kt::strclone(Self->Statement)) {
         Value = str;
         return ERR::Okay;
      }
      else return ERR::AllocMemory;
   }

   if (Self->Tags.empty()) return ERR::FieldNotSet;

   std::ostringstream buffer;

   if (auto tag = Self->getTag(0)) {
      CURSOR it;
      if (auto tags = Self->getInsert(tag, it)) {
         while (it != tags->end()) {
            serialise_xml(*it, buffer, Self->Flags);
            it++;
         }
      }
      else return log.warning(ERR::NotFound);
   }
   else return log.warning(ERR::NoData); // NB: If there are tags, tag 0 should always exist, so this indicates a parsing issue

   if (CSTRING str = kt::strclone(buffer.str())) {
      Value = str;
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
}

static ERR SET_Statement(extXML *Self, const std::string_view &Value)
{
   Self->Statement.clear();

   if (not Value.empty()) {
      if (Self->initialised()) {
         Self->Tags.clear();
         Self->LineNo = 1;
         Self->ParseError = txt_to_xml(Self, Self->Tags, Value);
         return Self->ParseError;
      }
      else  {
         Self->Statement = Value;
         return ERR::Okay;
      }
   }
   else {
      if (Self->initialised()) {
         bool ro = (Self->Flags & XMF::READ_ONLY) != XMF::NIL;
         Self->Flags &= ~XMF::READ_ONLY;
         acClear(Self);
         if (ro) Self->Flags |= XMF::READ_ONLY;
      }
      return ERR::Okay;
   }
}

/*********************************************************************************************************************

-FIELD-
Tags: Provides direct access to the XML document structure.

The Tags field exposes the complete XML document structure as a hierarchical array of !XTag structures.  This field
becomes available after successful XML parsing and provides the primary interface for reading XML content
programmatically.

Each !XTag will have at least one attribute set in the `Attribs` array.  The first attribute will either reflect the
tag name or a content string if the `Name` is undefined.  The `Children` array provides access to all child elements.

Direct read access to the Tags hierarchy is safe and efficient for traversing the document structure.  However,
modifications should be performed using the XML object's methods (#InsertXML(), #SetAttrib(), #RemoveTag(), etc.) to
maintain internal consistency and trigger appropriate cache invalidation.

NOTE: Tiri will copy this field on read, caching the value is therefore recommended.

*********************************************************************************************************************/

static ERR GET_Tags(extXML *Self, XTag **Values, int *Elements)
{
   *Values = Self->Tags.data();
   *Elements = Self->Tags.size();
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
LoadSchema: Load an XML Schema definition to enable schema-aware validation.

This method parses an XML Schema document and attaches its schema context to the current XML object.  Once loaded,
schema metadata is available for validation and XQuery evaluation routines that utilise schema-aware behaviour.

-INPUT-
strview Path: File system path to the XML Schema (XSD) document.

-ERRORS-
Okay: Schema was successfully loaded and parsed.
NullArgs: The Path argument was not provided.
NoData: The schema document did not contain any parsable definitions.
InvalidData: The schema document did not contain a usable root element.
CreateObject: The file in Path could not be processed as XML content.

-TAGS-
blocking, mutates-object, creates-resource

-END-

*********************************************************************************************************************/

static ERR XML_LoadSchema(extXML *Self, struct xml::LoadSchema *Args)
{
   kt::Log log;

   if ((not Args) or Args->Path.empty()) return log.warning(ERR::NullArgs);

   kt::Create<extXML> schema({ fl::Path(Args->Path), fl::Flags(XMF::WELL_FORMED | XMF::NAMESPACE_AWARE) });
   if (schema.ok()) {
      if (schema->Tags.empty()) return log.warning(ERR::NoData);

      xml::schema::SchemaParser parser(xml::schema::registry());

      // Find the first non-instruction tag

      XTag *root_tag = nullptr;
      for (auto &tag : schema->Tags) {
         if ((tag.Flags & XTF::INSTRUCTION) IS XTF::NIL) { root_tag = &tag; break; }
      }

      if (!root_tag) return log.warning(ERR::InvalidData);

      auto Document = parser.parse(*root_tag);
      if (Document.empty() or (not Document.context)) return log.warning(ERR::NoData);

      Self->Flags |= XMF::HAS_SCHEMA;
      Self->SchemaContext = Document.context;
      return ERR::Okay;
   }
   else return log.warning(ERR::CreateObject);
}

/*********************************************************************************************************************

-METHOD-
ValidateDocument: Validate the XML document against the currently loaded schema.

This method performs structural and simple type validation of the document using the loaded XML Schema.  It returns
`ERR::Okay` when the document conforms to the schema.  Validation failures return an error and populate #ErrorMsg with
the reason where available.

-ERRORS-
Okay: Validation completed successfully.
NoSupport: No schema has been loaded for this XML object.
NoData: The XML document does not contain any parsed tags.
Search: The schema does not define the root element present in the document.
InvalidData: The document structure, namespace or element content failed schema validation.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR XML_ValidateDocument(extXML *Self, void *Args)
{
   kt::Log log;

   Self->ErrorMsg.clear();

   if (not Self->SchemaContext) {
      Self->ErrorMsg = "No schema has been loaded for this document.";
      return log.warning(ERR::NoSupport);
   }

   if (Self->Tags.empty()) {
      Self->ErrorMsg = "XML document has no parsed tags to validate.";
      return log.warning(ERR::NoData);
   }

   if ((Self->Tags[0].Attribs.empty()) or (Self->Tags[0].Attribs[0].Name.empty())) {
      Self->ErrorMsg = "Document root element is unnamed.";
      return log.warning(ERR::InvalidData);
   }

   auto &context = *Self->SchemaContext;
   auto find_descriptor = [&](std::string_view Name) ->
      std::shared_ptr<xml::schema::ElementDescriptor>
   {
      auto iter = context.elements.find(Name);
      if (iter != context.elements.end()) return iter->second;

      auto local = xml::schema::extract_local_name(Name);
      iter = context.elements.find(local);
      if (iter != context.elements.end()) return iter->second;

      if (!context.target_namespace_prefix.empty()) {
         std::string qualified = std::format("{}:{}", context.target_namespace_prefix, local);
         iter = context.elements.find(qualified);
         if (iter != context.elements.end()) return iter->second;
      }

      return nullptr;
   };

   XTag *document_root = nullptr;
   for (auto &tag : Self->Tags) {
      if ((tag.Flags & XTF::INSTRUCTION) IS XTF::NIL) { document_root = &tag; break; }
   }

   if (!document_root) {
      Self->ErrorMsg = "Document does not contain a schema-valid root element.";
      return log.warning(ERR::InvalidData);
   }

   auto descriptor = find_descriptor(document_root->Attribs[0].Name);
   if (!descriptor) {
      Self->ErrorMsg = std::format("Schema does not define root element '{}'.", document_root->Attribs[0].Name);
      return log.warning(ERR::Search);
   }

   std::string schema_namespace = context.target_namespace;
   bool schema_has_namespace = !schema_namespace.empty();

   std::string root_namespace;
   bool root_has_namespace = false;

   auto assign_root_namespace = [&](const std::string &value)
   {
      if (root_has_namespace) return;
      if (value.empty()) return;

      root_namespace = value;
      root_has_namespace = true;
   };

   if (document_root->NamespaceID != 0u) {
      if (auto uri = Self->getNamespaceURI(document_root->NamespaceID)) {
         assign_root_namespace(*uri);
      }
      else {
         Self->ErrorMsg = "Root element namespace is not registered within this document.";
         return log.warning(ERR::InvalidData);
      }
   }

   if (not root_has_namespace) {
      std::string root_prefix;
      if (!document_root->Attribs.empty()) {
         std::string_view qualified_name(document_root->Attribs[0].Name);
         auto colon = qualified_name.find(':');
         if (colon != std::string::npos) {
            root_prefix.assign(qualified_name.begin(), qualified_name.begin() + colon);
         }
      }

      std::string prefix_attribute;
      if (!root_prefix.empty()) {
         prefix_attribute = std::format("xmlns:{}", root_prefix);
      }

      if (!prefix_attribute.empty()) {
         for (size_t index = 1u; index < document_root->Attribs.size(); ++index) {
            const auto &attrib = document_root->Attribs[index];
            if (kt::iequals(attrib.Name, prefix_attribute)) {
               assign_root_namespace(attrib.Value);
               break;
            }
         }
      }

      if (!root_has_namespace) {
         for (size_t index = 1u; index < document_root->Attribs.size(); ++index) {
            const auto &attrib = document_root->Attribs[index];
            if (kt::iequals(attrib.Name, "xmlns")) {
               assign_root_namespace(attrib.Value);
               break;
            }
         }
      }
   }

   if (schema_has_namespace and (not root_has_namespace)) {
      Self->ErrorMsg = std::format("Root element is missing the schema target namespace '{}'.", schema_namespace);
      return log.warning(ERR::Search);
   }

   if ((not schema_has_namespace) and root_has_namespace) {
      Self->ErrorMsg = std::format("Root element namespace '{}' is not expected by the schema.", root_namespace);
      return log.warning(ERR::Search);
   }

   if (schema_has_namespace and !(root_namespace IS schema_namespace)) {
      Self->ErrorMsg = std::format("Root element namespace '{}' does not match schema target namespace '{}'.",
         root_namespace, schema_namespace);
      return log.warning(ERR::Search);
   }

   xml::schema::TypeChecker checker(xml::schema::registry(), Self->SchemaContext.get(), &Self->ErrorMsg);
   checker.clear_error();

   if (checker.validate_element(*document_root, *descriptor)) {
      Self->ErrorMsg.clear();
      return ERR::Okay;
   }

   if (Self->ErrorMsg.empty()) {
      Self->ErrorMsg = checker.last_error();
      if (Self->ErrorMsg.empty()) Self->ErrorMsg = "Schema validation failed.";
   }

   log.warning("%s", Self->ErrorMsg.c_str());
   return ERR::InvalidData;
}

//********************************************************************************************************************

#include "xml_class_def.c"

static const FieldArray clFields[] = {
   { "Path",         FDF_CPPSTRING|FDF_RW, nullptr, SET_Path },
   { "DocType",      FDF_CPPSTRING|FDF_RW },
   { "PublicID",     FDF_CPPSTRING|FDF_RW },
   { "SystemID",     FDF_CPPSTRING|FDF_RW },
   { "ErrorMsg",     FDF_CPPSTRING|FDF_R },
   { "Source",       FDF_OBJECT|FDF_RI },
   { "Flags",        FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clXMLFlags },
   { "Modified",     FDF_INT|FDF_R },
   { "ParseError",   FDF_INT|FD_PRIVATE|FDF_R },
   { "LineNo",       FDF_INT|FD_PRIVATE|FDF_R },
   // Virtual fields
   { "Src",        FDF_VIRTUAL|FDF_CPPSTRING|FDF_SYNONYM|FDF_RW|FDF_PURE, GET_Path, SET_Path },
   { "Statement",  FDF_VIRTUAL|FDF_CPPSTRING|FDF_ALLOC|FDF_RW, GET_Statement, SET_Statement },
   { "Tags",       FDF_VIRTUAL|FDF_ARRAY|FDF_STRUCT|FDF_R|FDF_PURE, GET_Tags, nullptr, "XTag" },
   END_FIELD
};

static ERR add_xml_class(void)
{
   clXML = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::XML),
      fl::ClassVersion(VER_XML),
      fl::Name("XML"),
      fl::FileExtension("xml"),
      fl::FileDescription("eXtensible Markup Language (XML)"),
      fl::Icon("filetypes/xml"),
      fl::Category(CCF::DATA),
      fl::Actions(clXMLActions),
      fl::Methods(clXMLMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(extXML)),
      fl::Path(MOD_PATH));

   return clXML ? ERR::Okay : ERR::AddClass;
}
