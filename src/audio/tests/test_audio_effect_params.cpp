#include <kotuku/main.h>
#include <kotuku/modules/audio.h>
#include <kotuku/modules/script.h>
#include <kotuku/modules/filesystem.h>
#include <cassert>
#include <limits>
#include "audio.h"

// A schema shaped like AudioEqualiser's, so that the generic machinery is tested without Core.

static const int glShelf[] = { 1, 2 };
static const int glPass[]  = { 3, 4 };

static const AudioParamOption glTypes[] = {
   { 0, "peak", "Peak", "A \"bell\" curve." }, { 1, "low_shelf", "Low Shelf" }, { 2, "high_shelf", "High Shelf" },
   { 3, "low_pass", "Low Pass" }, { 4, "high_pass", "High Pass" }
};

static const AudioParamRule glGainRules[] = { { .Key = "type", .Values = glPass, .Inactive = true } };
static const AudioParamRule glQRules[]    = { { .Key = "type", .Values = glShelf, .CapMax = 1 } };

static const AudioParamDesc glMembers[] = {
   { .Key = "type", .Label = "Type", .Type = APT::ENUM, .Default = 0, .Options = glTypes },
   { .Key = "frequency", .Label = "Frequency", .Unit = APU::HZ, .Scale = APS::LOG, .Min = 0, .Default = 1000,
     .MinExclusive = true, .MaxBound = APB::NYQUIST },
   { .Key = "gain", .Label = "Gain", .Unit = APU::DB, .Min = -48, .Max = 48, .Default = 0, .Rules = glGainRules },
   { .Key = "q", .Label = "Q", .Scale = APS::LOG, .Min = 0.1, .Max = 20, .Default = 0.707,
     .Rules = glQRules }
};

static const AudioParamDesc glParams[] = {
   { .Key = "gain", .Label = "Output \"Trim\"", .Description = "Level <after> bands & trim.", .Unit = APU::DB,
     .Min = -48, .Max = 48, .Default = 0 },
   { .Key = "steps", .Label = "Steps", .Type = APT::INT, .Min = 0, .Max = 4, .Default = 1, .Step = 1 }
};

static const AudioParamGroup glGroups[] = {
   { .Key = "bands", .Label = "Band", .Description = "Filters in order.", .MinCount = 0, .MaxCount = 2,
     .Members = glMembers }
};

static const CSTRING glOutputs[] = { "response" };

static const AudioEffectSchema glSchema = {
   .ClassName = "TestEffect", .Version = 1, .Description = "A test effect.", .Params = glParams, .Groups = glGroups,
   .Outputs = glOutputs, .Read = nullptr, .Apply = nullptr
};

static AudioParamState make_state(std::initializer_list<std::vector<double>> Bands)
{
   AudioParamState state;
   state.Params = { 0, 1 };
   state.Groups.resize(1);
   int origin = 0;
   for (auto &band : Bands) state.Groups[0].push_back({ band, origin++ });
   return state;
}

static void test_check_param()
{
   const auto &frequency = glMembers[1];
   assert(check_param(frequency, 1000, 48000) IS ERR::Okay);
   assert(check_param(frequency, 0, 48000) IS ERR::InvalidValue);          // Exclusive minimum
   assert(check_param(frequency, 0.001, 48000) IS ERR::Okay);
   assert(check_param(frequency, 24000, 48000) IS ERR::InvalidValue);      // Nyquist is exclusive
   assert(check_param(frequency, 23999, 48000) IS ERR::Okay);
   assert(check_param(frequency, 100000, 0) IS ERR::Okay);                 // No Nyquist bound without a rate
   assert(check_param(frequency, std::numeric_limits<double>::quiet_NaN(), 48000) IS ERR::InvalidValue);
   assert(check_param(frequency, std::numeric_limits<double>::infinity(), 0) IS ERR::InvalidValue);

   const auto &gain = glMembers[2];
   assert(check_param(gain, 48, 48000) IS ERR::Okay);                      // Inclusive bounds
   assert(check_param(gain, -48, 48000) IS ERR::Okay);
   assert(check_param(gain, 48.001, 48000) IS ERR::InvalidValue);

   const auto &type = glMembers[0];
   assert(check_param(type, 4, 48000) IS ERR::Okay);
   assert(check_param(type, 5, 48000) IS ERR::InvalidValue);
   assert(check_param(type, 1.5, 48000) IS ERR::InvalidValue);

   assert(check_param(glParams[1], 2, 48000) IS ERR::Okay);
   assert(check_param(glParams[1], 2.5, 48000) IS ERR::InvalidValue);     // Integers must be whole
}

static void test_rules()
{
   std::vector<double> shelf_steep = { 1, 100, 3, 2 };
   std::vector<double> shelf_gentle = { 1, 100, 3, 1 };
   std::vector<double> peak_steep = { 0, 100, 3, 2 };
   std::vector<double> pass_loud = { 3, 100, 48, 2 };
   assert(check_rules(glMembers, shelf_steep) IS ERR::InvalidValue);
   assert(check_rules(glMembers, shelf_gentle) IS ERR::Okay);
   assert(check_rules(glMembers, peak_steep) IS ERR::Okay);
   assert(check_rules(glMembers, pass_loud) IS ERR::Okay);                // Inactive rules never reject
}

static void test_validate_state()
{
   auto state = make_state({ { 0, 1000, 3, 1 }, { 1, 100, 3, 0.7 } });
   assert(validate_state(glSchema, state, 48000) IS ERR::Okay);

   auto bad_rule = state;
   bad_rule.Groups[0][1].Values[3] = 2;
   assert(validate_state(glSchema, bad_rule, 48000) IS ERR::InvalidValue);

   auto too_many = state;
   too_many.Groups[0].push_back({ { 0, 500, 0, 1 } });
   assert(validate_state(glSchema, too_many, 48000) IS ERR::OutOfRange);

   auto bad_shape = state;
   bad_shape.Groups[0][0].Values.pop_back();
   assert(validate_state(glSchema, bad_shape, 48000) IS ERR::InvalidValue);

   // A lower rate moves Nyquist below a committed value.  Unchanged values are exempt; changed values are not.

   auto committed = make_state({ { 0, 20000, 3, 1 } });
   assert(validate_state(glSchema, committed, 22050) IS ERR::InvalidValue);
   auto edit = committed;
   edit.Groups[0][0].Values[2] = 6;
   assert(validate_state(glSchema, edit, 22050, &committed) IS ERR::Okay);
   edit.Groups[0][0].Values[1] = 19000;
   assert(validate_state(glSchema, edit, 22050, &committed) IS ERR::InvalidValue);

   // A changed entry is checked against its rules even if the offending value itself is unchanged.

   auto steep = make_state({ { 0, 1000, 0, 5 } });
   auto to_shelf = steep;
   to_shelf.Groups[0][0].Values[0] = 1;
   assert(validate_state(glSchema, to_shelf, 48000, &steep) IS ERR::InvalidValue);
   to_shelf.Groups[0][0].Values[3] = 0.7;
   assert(validate_state(glSchema, to_shelf, 48000, &steep) IS ERR::Okay);

   // New entries (origin -1) are always fully checked.

   auto inserted = committed;
   inserted.Groups[0].insert(inserted.Groups[0].begin(), AudioParamEntry { { 0, 30000, 0, 1 }, -1 });
   assert(validate_state(glSchema, inserted, 48000, &committed) IS ERR::InvalidValue);
}

static void test_paths()
{
   auto state = make_state({ { 0, 1000, 3, 1 }, { 1, 100, 3, 0.7 } });
   double *value;
   const AudioParamDesc *desc;

   assert(resolve_path(glSchema, state, "gain", value, desc) IS ERR::Okay);
   assert((value IS &state.Params[0]) and (desc IS &glParams[0]));
   assert(resolve_path(glSchema, state, "bands[1].frequency", value, desc) IS ERR::Okay);
   assert((*value IS 100) and (desc IS &glMembers[1]));

   assert(resolve_path(glSchema, state, "bands[2].frequency", value, desc) IS ERR::OutOfRange);
   assert(resolve_path(glSchema, state, "bands[99999999999].q", value, desc) IS ERR::OutOfRange);
   assert(resolve_path(glSchema, state, "unknown", value, desc) IS ERR::Search);
   assert(resolve_path(glSchema, state, "bands[0].unknown", value, desc) IS ERR::Search);
   assert(resolve_path(glSchema, state, "other[0].q", value, desc) IS ERR::Search);
   assert(resolve_path(glSchema, state, "bands[].q", value, desc) IS ERR::Search);
   assert(resolve_path(glSchema, state, "bands[-1].q", value, desc) IS ERR::Search);
   assert(resolve_path(glSchema, state, "bands[0]", value, desc) IS ERR::Search);
   assert(resolve_path(glSchema, state, "bands[0]q", value, desc) IS ERR::Search);
   assert(resolve_path(glSchema, state, "bands[0.q", value, desc) IS ERR::Search);
   assert(resolve_path(glSchema, state, "", value, desc) IS ERR::Search);
}

static void test_defaults()
{
   auto entry = default_entry(glGroups[0], 48000);
   assert((entry.Values IS std::vector<double> { 0, 1000, 0, 0.707 }) and (entry.Origin IS -1));
   auto low_rate = default_entry(glGroups[0], 1600);                        // Nyquist 800 Hz
   assert(low_rate.Values[1] IS 400);
   assert(find_group(glSchema, "bands") IS 0);
   assert(find_group(glSchema, "gain") IS -1);
}

static bool has(const std::string &Text, std::string_view Part)
{
   return Text.find(Part) != std::string::npos;
}

static void test_xml()
{
   auto xml = build_schema_xml(glSchema);
   assert(xml.starts_with("<effect class=\"TestEffect\" version=\"1\" description=\"A test effect.\">\n"));
   assert(has(xml, "label=\"Output &quot;Trim&quot;\" description=\"Level &lt;after&gt; bands &amp; trim.\" "
      "type=\"double\""));
   assert(has(xml, "<group key=\"bands\" label=\"Band\" description=\"Filters in order.\" min=\"0\" max=\"2\">"));
   assert(not has(xml, "label=\"Steps\" description"));   // Omitted when not set
   assert(has(xml, "<param key=\"steps\" label=\"Steps\" type=\"int\" scale=\"linear\" min=\"0\" max=\"4\" "
      "step=\"1\" default=\"1\"/>"));
   assert(has(xml, "<param key=\"type\" label=\"Type\" type=\"enum\" default=\"0\">"));
   assert(has(xml, "<option value=\"0\" key=\"peak\" label=\"Peak\" description=\"A &quot;bell&quot; curve.\"/>"));
   assert(has(xml, "<option value=\"1\" key=\"low_shelf\" label=\"Low Shelf\"/>"));   // Omitted when not set
   assert(has(xml, "unit=\"Hz\" scale=\"log\" min=\"0\" min-exclusive=\"1\" max=\"nyquist\" "
      "max-exclusive=\"1\" default=\"1000\"/>"));
   assert(has(xml, "<rule key=\"type\" values=\"3,4\" inactive=\"1\"/>"));
   assert(has(xml, "<rule key=\"type\" values=\"1,2\" max=\"1\"/>"));
   assert(has(xml, "default=\"0.707\""));
   assert(has(xml, "<output key=\"response\" type=\"curve\"/>"));
   assert(xml.ends_with("</effect>\n"));
}

int main()
{
   test_check_param();
   test_rules();
   test_validate_state();
   test_paths();
   test_defaults();
   test_xml();
   return 0;
}
