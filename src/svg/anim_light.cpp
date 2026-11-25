//********************************************************************************************************************
// Animation of light source elements: feDistantLight, fePointLight and feSpotLight.

// Returns the member of a light's geometry that an attribute of its element controls, or nullptr if the attribute
// does not apply to that kind of light.

static double * light_member(LightGeometry &Light, uint32_t Attribute)
{
   switch (Light.Type) {
      case LS::DISTANT:
         switch (Attribute) {
            case SVF_azimuth:   return &Light.Azimuth;
            case SVF_elevation: return &Light.Elevation;
            default: return nullptr;
         }

      case LS::SPOT:
         switch (Attribute) {
            case SVF_pointsAtX:         return &Light.PX;
            case SVF_pointsAtY:         return &Light.PY;
            case SVF_pointsAtZ:         return &Light.PZ;
            case SVF_specularExponent:  return &Light.Exponent;
            case SVF_limitingConeAngle: return &Light.ConeAngle;
            default: break;
         }
         [[fallthrough]];

      case LS::POINT:
         switch (Attribute) {
            case SVF_x: return &Light.X;
            case SVF_y: return &Light.Y;
            case SVF_z: return &Light.Z;
            default: return nullptr;
         }

      default: return nullptr;
   }
}

//********************************************************************************************************************
// Resolve the endpoints before the timeline is registered.  Paced animations use cumulative distances as key times.

bool anim_light::prepare(double BaseValue)
{
   const auto parse_number = [](std::string_view Value) -> std::optional<double> {
      Value = kt::font_traits::trim(Value);
      if (Value.starts_with("+-")) return std::nullopt;
      return kt::font_traits::parse_number(Value);
   };

   if (non_interpolating) {
      auto value = parse_number(to);
      if (not value) return false;
      samples.push_back(*value);
   }
   else if (not values.empty()) {
      for (const auto &entry : values) {
         auto value = parse_number(entry);
         if (not value) return false;
         samples.push_back(*value);
      }
   }
   else {
      auto first = from.empty() ? std::optional<double>(BaseValue) : parse_number(from);
      auto last = parse_number(to.empty() ? by : to);
      if ((not first) or (not last) or (not std::isfinite(*first))) return false;
      if (to.empty()) *last += *first;
      if (not std::isfinite(*last)) return false;
      samples = { *first, *last };
   }

   if (samples.size() IS 1) return true;

   if (calc_mode IS CMODE::PACED) {
      // Scaling keeps differences and their sum finite even for endpoints at opposite extremes.
      double scale = 1.0;
      for (double value : samples) scale = std::max(scale, std::abs(value));
      timing.assign(samples.size(), 0.0);
      for (size_t i = 1; i < samples.size(); i++) {
         timing[i] = timing[i - 1] + std::abs(samples[i] / scale - samples[i - 1] / scale);
      }
      if (const double duration = timing.back(); duration > 0.0) {
         for (double &time : timing) time /= duration;
      }
      else timing.clear();
   }
   else if (calc_mode IS CMODE::SPLINE) {
      if (spline_paths.size() != samples.size() - 1) return false;
      if (not timing.empty()) {
         if ((timing.size() != samples.size()) or (timing.front() != 0.0) or (timing.back() != 1.0)) return false;
      }
   }
   return true;
}

//********************************************************************************************************************
// Write the animated value of the target attribute to Light.

void anim_light::apply(LightGeometry &Light)
{
   if (not std::isfinite(seek)) return;

   if (auto member = light_member(Light, strhash(target_attrib))) {
      if (samples.size() IS 1) {
         *member = samples.front();
         return;
      }

      size_t index;
      double progress;
      if (timing.size() IS samples.size()) {
         const double position = seek * timing.back();
         const auto next = std::upper_bound(timing.begin(), timing.end() - 1, position);
         index = std::max(size_t(next - timing.begin()), size_t(1)) - 1;
         progress = interval_seek(position, timing[index], timing[index + 1]);
      }
      else {
         const auto last = samples.size() - 1;
         const double position = seek * last;
         index = std::min(size_t(position), last - 1);
         progress = position - index;
      }

      if (calc_mode IS CMODE::SPLINE) progress = spline_seek(spline_paths[index], progress);

      const double value = interpolate_numeric(samples[index], samples[index + 1], progress);
      if (std::isfinite(value)) *member = value;
   }
}

//********************************************************************************************************************
// Apply the light animations for the current frame.  Each light starts from its base geometry and takes the
// contributing animations in document order, so a later animation of an attribute overrides an earlier one.  A light
// is only written if an animation contributes to it, or contributed to the previous frame and must be restored.

static void apply_light_animations(extSVG *SVG)
{
   struct light_state {
      LightGeometry geometry;
      bool changed = false;
   };

   std::map<std::pair<OBJECTID, int>, light_state> lights;

   for (auto &record : SVG->Animations) {
      auto anim = std::get_if<anim_light>(&record);
      if (not anim) continue;

      auto [entry, inserted] = lights.try_emplace(std::make_pair(anim->target_vector, anim->light_index),
         light_state { anim->base, false });
      auto &state = entry->second;

      const bool contributes = anim->contributes();
      if (contributes) anim->apply(state.geometry);
      if (contributes or anim->applied) state.changed = true;
      anim->applied = contributes;
   }

   for (auto &[key, state] : lights) {
      if (not state.changed) continue;
      kt::ScopedObjectLock<objLightingFX> effect(key.first, 1000);
      if (effect.granted()) effect->setLight(key.second, &state.geometry);
   }
}
