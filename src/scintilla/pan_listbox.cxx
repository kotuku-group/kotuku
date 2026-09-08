
/*********************************************************************************************************************
** ListBox class
*/

Scintilla::ListBox::ListBox()
{

}

Scintilla::ListBox::~ListBox()
{

}

/*********************************************************************************************************************
** ListBoxImp class
*/

class ListBoxImp : public Scintilla::ListBox {

public:

   ListBoxImp();
   virtual ~ListBoxImp();

   virtual void SetFont(Scintilla::Font &font);
   virtual void Create(Scintilla::Window &parent, int ctrlID, Scintilla::Point location_, int lineHeight_, bool unicodeMode_);
   virtual void SetAverageCharWidth(int width);
   virtual void SetVisibleRows(int rows);
   virtual int GetVisibleRows() const;
   virtual Scintilla::PRectangle GetDesiredRect();
   virtual int CaretFromEdge();
   virtual void Clear();
   virtual void Append(char *s, int type = -1);
   virtual int Length();
   virtual void Select(int n);
   virtual int GetSelection();
   virtual int Find(const char *prefix);
   virtual void GetValue(int n, char *value, int len);
   virtual void RegisterImage(int type, const char *xpm_data);
	virtual void RegisterRGBAImage(int type, int width, int height, const unsigned char *pixelsImage);
   virtual void ClearRegisteredImages();
   virtual void SetDoubleClickAction(Scintilla::CallBackAction action, void *data);
   virtual void SetList(const char* list, char separator, char typesep);

private:
   OBJECTPTR menu;
   //OBJECTPTR menufont;
};

ListBoxImp::ListBoxImp()
{
   kt::Log log(__FUNCTION__);
   log.branch();

   menu = nullptr;
}

ListBoxImp::~ListBoxImp()
{
   kt::Log log(__FUNCTION__);
   log.branch();

   if (menu) { FreeResource(menu); menu = nullptr; }
}

Scintilla::ListBox * Scintilla::ListBox::Allocate()
{
   ListBoxImp *lb = new ListBoxImp();
   return lb;
}

void ListBoxImp::Create(Scintilla::Window &Window, int, Scintilla::Point, int, bool)
{
   kt::Log log(__FUNCTION__);

   OBJECTID surface_id = getSurfaceID(&Window);

   log.branch("Surface: %d", surface_id);
/*
   if (CreateObject(ID_MENU, 0, &menu,
         FID_Surface|TINT,  surface_id,
         TAGEND) != ERR_Okay) {

      //error
   }

   //struct acDataFeed { OBJECTID ObjectID; LONG DataType; LONG Version;
   //    APTR Buffer; LONG Size; LONG TotalEntries; };
   struct acDataFeed dc_args;

   dc_args.ObjectID = menu->UID;
   dc_args.DataType = DATA_XML;
   dc_args.Buffer = (STRING)"<menu name=\"Edit\">\
      <item text=\"Cut\" icon=\"icons:tools/cut\" qualifier=\"CTRL\" key=\"X\">\
         <action call=\"clipboard\" object=\"[@text]\" %mode=\"1\"/>\
      </item>\
   </menu>";
   dc_args.Size = StrLength((STRING)dc_args.Buffer);
   Action(AC::DataFeed, menu, &dc_args);
   acShow(menu);
*/
}

/****************************************************************************/

void ListBoxImp::RegisterRGBAImage(int type, int width, int height, const unsigned char *pixelsImage)
{

}

/****************************************************************************/

void ListBoxImp::SetFont(Scintilla::Font &scint_font)
{

}

/****************************************************************************/

void ListBoxImp::SetAverageCharWidth(int width)
{

}

/****************************************************************************/

void ListBoxImp::SetVisibleRows(int rows)
{

}

/****************************************************************************/

int ListBoxImp::GetVisibleRows() const
{
   return 0;/* TEMP HACK */
}

/****************************************************************************/

Scintilla::PRectangle ListBoxImp::GetDesiredRect()
{
   return Scintilla::PRectangle(100, 100, 200, 200);
}

/****************************************************************************/

int ListBoxImp::CaretFromEdge()
{
   return 0;/* TEMP HACK */
}

/****************************************************************************/

void ListBoxImp::Clear()
{

}

/****************************************************************************/

void ListBoxImp::Append(char *s, int type)
{

}

/****************************************************************************/

int ListBoxImp::Length()
{
   return 0;/* TEMP HACK */
}

/****************************************************************************/

void ListBoxImp::Select(int n)
{

}

/****************************************************************************/

int ListBoxImp::GetSelection()
{
   return 0;/* TEMP HACK */
}

/****************************************************************************/

int ListBoxImp::Find(const char *prefix)
{
   return 0;/* TEMP HACK */
}

/****************************************************************************/

void ListBoxImp::GetValue(int n, char *value, int len)
{

}

/****************************************************************************/

void ListBoxImp::RegisterImage(int type, const char *xpm_data)
{

}

/****************************************************************************/

void ListBoxImp::ClearRegisteredImages()
{

}

/****************************************************************************/

void ListBoxImp::SetDoubleClickAction(Scintilla::CallBackAction action, void *data)
{

}

/****************************************************************************/

void ListBoxImp::SetList(const char* list, char separator, char typesep)
{

}
