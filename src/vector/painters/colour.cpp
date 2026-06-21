/*********************************************************************************************************************

-CLASS-
VectorColour: This is a stub class for use with the Vector module's DrawPath() function.

VectorColour is a stub class for exclusive use with the Vector module's ~Vector.DrawPath() function, for use in
either the `StrokeStyle` or `FillStyle` parameters.  VectorColour allows the path to be drawn with a solid colour,
as specified in the #Red, #Green, #Blue and #Alpha fields.

-FIELD-
Red: The red component value.

The red component value, measured from 0 to 1.0.  The default is 0.0.

-FIELD-
Green: The green component value.

The green component value, measured from 0 to 1.0.  The default is 0.0.

-FIELD-
Blue: The blue component value.

The blue component value, measured from 0 to 1.0.  The default is 0.0.

-FIELD-
Alpha: The alpha component value.

The alpha component value, measured from 0 to 1.0.  The default is 1.0.
-END-

*********************************************************************************************************************/

static ERR VECTORCOLOUR_NewPlacement(objVectorColour *Self)
{
   new (Self) objVectorColour();
   return ERR::Okay;
}

static const ActionArray clColourActions[] = {
   { AC::NewPlacement, (APTR)VECTORCOLOUR_NewPlacement },
   { AC::NIL, nullptr }
};

static const FieldArray clColourFields[] = {
   { "Red",   FDF_DOUBLE|FDF_RW },
   { "Green", FDF_DOUBLE|FDF_RW },
   { "Blue",  FDF_DOUBLE|FDF_RW },
   { "Alpha", FDF_DOUBLE|FDF_RW },
   END_FIELD
};

//********************************************************************************************************************

ERR init_colour(void)
{
   clVectorColour = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTORCOLOUR),
      fl::Name("VectorColour"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clColourActions),
      fl::Fields(clColourFields),
      fl::Size(sizeof(objVectorColour)),
      fl::Path(MOD_PATH));

   return clVectorColour ? ERR::Okay : ERR::AddClass;
}
