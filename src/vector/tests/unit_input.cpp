// Included by scene.cpp to exercise nested input dispatch through the module's unit-test hook.

void vector_input_unit_tests(std::string_view Options, int &Passed, int &Total)
{
   const std::string options(Options);
   OBJECTID scene_id = 0;
   int type = 0;
   double value0 = 0, value1 = 0;
   if (std::sscanf(options.c_str(), "input:%d:%d:%lf:%lf", &scene_id, &type, &value0, &value1) != 4) return;

   Total++;
   kt::ScopedObjectLock<extVectorScene> scene(scene_id);
   if ((not scene.granted()) or (scene.obj->classID() != CLASSID::VECTORSCENE)) return;
   const bool wheel = (JET(type) IS JET::WHEEL) or (JET(type) IS JET::WHEEL_X);
   if ((not wheel) and (JET(type) != JET::ABS_XY)) return;

   InputEvent event = {};
   event.Type = JET(type);
   event.Flags = event.Mask = wheel ? JTYPE::EXT_MOVEMENT : JTYPE::MOVEMENT;
   event.RecipientID = event.OverID = scene.obj->SurfaceID;
   event.Value = wheel ? value0 : 0;
   event.X = value0;
   event.Y = value1;
   kt::SwitchContext context(scene.obj);
   if (scene_input_events(&event, 0) IS ERR::Okay) Passed++;
}
