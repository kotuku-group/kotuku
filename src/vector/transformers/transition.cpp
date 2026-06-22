/*********************************************************************************************************************

-CLASS-
VectorTransition: Transitions are used to incrementally apply transforms over distance.

The VectorTransition class is used to gradually transform vector shapes over the length of a path.  This feature is
not SVG compliant, though it can be utilised from SVG files via the 'kotuku:' name space.

The transition is defined as a series of stops and transform instructions, of which at least 2 are required in order to
interpolate the transforms over distance.  The transform strings are defined as per the SVG guidelines for the
transform attribute.

The following example illustrates the use of a transition in SVG:

<pre>
  &lt;defs&gt;
    &lt;kotuku:transition id="hill"&gt;
      &lt;stop offset="0" transform="scale(0.3)"/&gt;
      &lt;stop offset="50%" transform="scale(1.5)"/&gt;
      &lt;stop offset="100%" transform="scale(0.3)"/&gt;
    &lt;/kotuku:transition&gt;
  &lt;/defs&gt;

  &lt;rect fill="#ffffff" width="100%" height="100%"/&gt;
  &lt;text x="3" y="80" font-size="19.6" fill="navy" transition="url(#hill)"&gt;This text is morphed by a transition&lt;/text&gt;
</pre>

Transitions are most effective when used in conjunction with the morph feature in the @Vector class.

-END-

*********************************************************************************************************************/

// Applies the correct transform when given a relative Index position between 0.0 and 1.0

void apply_transition(extVectorTransition *Self, double Index, agg::trans_affine &Transform)
{
   if (Index <= Self->Stops[0].Offset) {
      Transform.multiply(*Self->Stops[0].AGGTransform);
   }
   else if (Index >= Self->Stops[Self->Stops.size()-1].Offset) {
      Transform.multiply(*Self->Stops[Self->Stops.size()-1].AGGTransform);
   }
   else {
      // Interpolate between transforms.

      int left, right;
      for (left=std::ssize(Self->Stops)-1; (left > 0) and (Index < Self->Stops[left].Offset); left--);
      for (right=left+1; (right < std::ssize(Self->Stops)) and (Self->Stops[right].Offset < Index); right++);

      if ((left < right) and (right < std::ssize(Self->Stops))) {
         agg::trans_affine interp;

         // Normalise the index
         double scale = (Index - Self->Stops[left].Offset) / (Self->Stops[right].Offset - Self->Stops[left].Offset);

         interp.sx  = Self->Stops[left].AGGTransform->sx  + ((Self->Stops[right].AGGTransform->sx  - Self->Stops[left].AGGTransform->sx) * scale);
         interp.sy  = Self->Stops[left].AGGTransform->sy  + ((Self->Stops[right].AGGTransform->sy  - Self->Stops[left].AGGTransform->sy) * scale);
         interp.shx = Self->Stops[left].AGGTransform->shx + ((Self->Stops[right].AGGTransform->shx - Self->Stops[left].AGGTransform->shx) * scale);
         interp.shy = Self->Stops[left].AGGTransform->shy + ((Self->Stops[right].AGGTransform->shy - Self->Stops[left].AGGTransform->shy) * scale);
         interp.tx  = Self->Stops[left].AGGTransform->tx  + ((Self->Stops[right].AGGTransform->tx  - Self->Stops[left].AGGTransform->tx) * scale);
         interp.ty  = Self->Stops[left].AGGTransform->ty  + ((Self->Stops[right].AGGTransform->ty  - Self->Stops[left].AGGTransform->ty) * scale);

         Transform.multiply(interp);

         //log.trace("Index: %.2f, Scale: %.2f, Left: %d, Right: %d, TotalStops: %d", Index, scale, left, right, Self->Stops.size());
      }
      else {
         kt::Log log(__FUNCTION__);
         log.warning("Invalid transition.  Index: %.2f, Left: %d, Right: %d, TotalStops: %d", Index, left, right, int(Self->Stops.size()));
      }
   }
}

//********************************************************************************************************************
// Accurately interpolate the transform for Index and apply it to the coordinate (X,Y).

void apply_transition_xy(extVectorTransition *Self, double Index, double *X, double *Y)
{
   if (Index <= Self->Stops[0].Offset) {
      Self->Stops[0].AGGTransform->transform(X, Y);
   }
   else if (Index >= Self->Stops[Self->Stops.size()-1].Offset) {
      Self->Stops[Self->Stops.size()-1].AGGTransform->transform(X, Y);
   }
   else {
      // Interpolate between transforms.

      int left, right;
      for (left=Self->Stops.size()-1; (left > 0) and (Index < Self->Stops[left].Offset); left--);
      for (right=left+1; (right < std::ssize(Self->Stops)) and (Self->Stops[right].Offset < Index); right++);

      if ((left < right) and (right < std::ssize(Self->Stops))) {
         agg::trans_affine interp;

         // Normalise the index
         double scale = (Index - Self->Stops[left].Offset) / (Self->Stops[right].Offset - Self->Stops[left].Offset);

         interp.sx  = Self->Stops[left].AGGTransform->sx  + ((Self->Stops[right].AGGTransform->sx  - Self->Stops[left].AGGTransform->sx) * scale);
         interp.sy  = Self->Stops[left].AGGTransform->sy  + ((Self->Stops[right].AGGTransform->sy  - Self->Stops[left].AGGTransform->sy) * scale);
         interp.shx = Self->Stops[left].AGGTransform->shx + ((Self->Stops[right].AGGTransform->shx - Self->Stops[left].AGGTransform->shx) * scale);
         interp.shy = Self->Stops[left].AGGTransform->shy + ((Self->Stops[right].AGGTransform->shy - Self->Stops[left].AGGTransform->shy) * scale);
         interp.tx  = Self->Stops[left].AGGTransform->tx  + ((Self->Stops[right].AGGTransform->tx  - Self->Stops[left].AGGTransform->tx) * scale);
         interp.ty  = Self->Stops[left].AGGTransform->ty  + ((Self->Stops[right].AGGTransform->ty  - Self->Stops[left].AGGTransform->ty) * scale);

         interp.transform(X, Y);
      }
   }
}

//********************************************************************************************************************

static ERR set_stop_transform(TransitionStop *Stop, std::string_view Commands)
{
   kt::Log log;

   // Empty transforms are permitted - it will result in an identity matrix being created.
   log.traceBranch("%.*s", int(Commands.size()), Commands.data());

   vec::ParseTransform(&Stop->Matrix, Commands);

   auto &m = Stop->Matrix;
   if (Stop->AGGTransform) {
      Stop->AGGTransform->load_all(m.ScaleX, m.ShearY, m.ShearX, m.ScaleY, m.TranslateX, m.TranslateY);
      return ERR::Okay;
   }
   else {
      Stop->AGGTransform.reset(new (std::nothrow) agg::trans_affine(
         m.ScaleX, m.ShearY, m.ShearX, m.ScaleY, m.TranslateX, m.TranslateY));
      if (Stop->AGGTransform) return ERR::Okay;
      else return log.warning(ERR::AllocMemory);
   }
}

//********************************************************************************************************************

static ERR TRANSITION_FreePlacement(extVectorTransition *Self) {
   Self->~extVectorTransition();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR TRANSITION_Init(extVectorTransition *Self)
{
   kt::Log log;
   if (Self->Stops.size() < 2) return log.warning(ERR::FieldNotSet);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR TRANSITION_NewPlacement(extVectorTransition *Self) {
   new (Self) extVectorTransition;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Stops: Defines the transforms that will be used at specific stop points.

A valid transition object must consist of at least two stop points in order to transition from one transform to another.
This is achieved by setting the Stops field with an array of Transition structures that define each stop point with
a transform string.  The Transition structure consists of the following fields:

<struct lookup="Transition"/>

*********************************************************************************************************************/

static ERR TRANSITION_SET_Stops(extVectorTransition *Self, std::span<const Transition> &Value)
{
   kt::Log log;
   const int elements = int(Value.size());
   if (elements >= 2) {
      std::vector<TransitionStop> stops;
      stops.reserve(elements);

      double last_offset = 0;
      for (auto i=0; i < elements; i++) {
         if (Value[i].Offset < last_offset) return log.warning(ERR::InvalidValue); // Offsets must be in incrementing order.
         if ((Value[i].Offset < 0.0) or (Value[i].Offset > 1.0)) return log.warning(ERR::OutOfRange);

         TransitionStop stop;
         stop.Offset = Value[i].Offset;
         if (const auto error = set_stop_transform(&stop, Value[i].Transform); error != ERR::Okay) return error;

         stops.emplace_back(std::move(stop));
         last_offset = Value[i].Offset;
      }

      Self->Stops = std::move(stops);
      Self->Dirty = true;
      Self->modified();
      return ERR::Okay;
   }
   else return log.warning(ERR::DataSize);
}

static const ActionArray clTransitionActions[] = {
   { AC::FreePlacement, TRANSITION_FreePlacement },
   { AC::Init,          TRANSITION_Init },
   { AC::NewPlacement,  TRANSITION_NewPlacement },
   { AC::NIL, nullptr }
};

static const FieldArray clTransitionFields[] = {
   { "Stops", FDF_VIRTUAL|FDF_VECTOR|FDF_STRUCT|FDF_W, nullptr, TRANSITION_SET_Stops, "Transition" },
   END_FIELD
};

ERR init_transition(void) // The transition is a definition type for creating transitions and not drawing.
{
   clVectorTransition = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTORTRANSITION),
      fl::Name("VectorTransition"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clTransitionActions),
      fl::Fields(clTransitionFields),
      fl::Size(sizeof(extVectorTransition)),
      fl::Path(MOD_PATH));

   return clVectorTransition ? ERR::Okay : ERR::AddClass;
}
