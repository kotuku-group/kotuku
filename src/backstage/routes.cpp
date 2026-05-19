// Auto-generated source code.  Regenerate with the backstage_routes CMake target.

static std::span<BackstageRoute> backstage_routes();

#include "routes/agents.cpp"
#include "routes/ping.cpp"
#include "routes/objects.cpp"
#include "routes/classes.cpp"
#include "routes/modules.cpp"
#include "routes/errors.cpp"
#include "routes/diagnostics.cpp"
#include "routes/jobs.cpp"
#include "routes/logs.cpp"
#include "routes/subscriptions.cpp"
#include "routes/docs.cpp"
#include "routes/scripts.cpp"

static constexpr std::array<std::string_view, 0> get_agents_context_path_param_names = {};

static constexpr std::array<BackstageParam, 0> get_agents_context_path_params = {};

static constexpr std::array<BackstageParam, 1> get_agents_context_query_params = {
   BackstageParam("detail", "string", "One of summary, normal, verbose.", "", false)
};

static constexpr std::array<std::string_view, 0> get_ping_path_param_names = {};

static constexpr std::array<BackstageParam, 0> get_ping_path_params = {};

static constexpr std::array<BackstageParam, 0> get_ping_query_params = {};

static constexpr std::array<std::string_view, 0> get_objects_path_param_names = {};

static constexpr std::array<BackstageParam, 0> get_objects_path_params = {};

static constexpr std::array<BackstageParam, 5> get_objects_query_params = {
   BackstageParam("classFilter", "csv", "Only return objects belonging to these classes", "", false),
   BackstageParam("depth", "int", "In nested mode, limits the depth of the tree", "", false),
   BackstageParam("limit", "int", "Limit count for lists of objects", "", false),
   BackstageParam("nested", "bool", "Return a nested JSON tree that matches the object graph; otherwise return a flat list in order of object creation", "", false),
   BackstageParam("root", "int", "Root object UID.", "", false)
};

static constexpr std::array<std::string_view, 0> post_objects_path_param_names = {};

static constexpr std::array<BackstageParam, 0> post_objects_path_params = {};

static constexpr std::array<BackstageParam, 0> post_objects_query_params = {};

static constexpr std::array<std::string_view, 1> get_objects_uid_path_param_names = {
   "uid"
};

static constexpr std::array<BackstageParam, 1> get_objects_uid_path_params = {
   BackstageParam("uid", "int", "Object UID.", "", false)
};

static constexpr std::array<BackstageParam, 2> get_objects_uid_query_params = {
   BackstageParam("filter", "csv", "Limit the field results to this list.", "", false),
   BackstageParam("includeMeta", "bool", "Include meta data for the object.", "", false)
};

static constexpr std::array<std::string_view, 1> get_objects_uid_children_path_param_names = {
   "uid"
};

static constexpr std::array<BackstageParam, 1> get_objects_uid_children_path_params = {
   BackstageParam("uid", "int", "Object UID.", "", false)
};

static constexpr std::array<BackstageParam, 3> get_objects_uid_children_query_params = {
   BackstageParam("depth", "int", "In nested mode, limits the depth of the child tree", "", false),
   BackstageParam("limit", "int", "Limit count for lists of child objects", "", false),
   BackstageParam("nested", "bool", "Return a nested JSON tree that matches the child hierarchy", "", false)
};

static constexpr std::array<std::string_view, 1> get_objects_uid_subscribers_path_param_names = {
   "uid"
};

static constexpr std::array<BackstageParam, 1> get_objects_uid_subscribers_path_params = {
   BackstageParam("uid", "int", "Object UID.", "", false)
};

static constexpr std::array<BackstageParam, 0> get_objects_uid_subscribers_query_params = {};

static constexpr std::array<std::string_view, 1> post_objects_uid_path_param_names = {
   "uid"
};

static constexpr std::array<BackstageParam, 1> post_objects_uid_path_params = {
   BackstageParam("uid", "int", "Object UID.", "", false)
};

static constexpr std::array<BackstageParam, 1> post_objects_uid_query_params = {
   BackstageParam("async", "bool", "Return immediately.  A job ID is returned for follow-up", "false", false)
};

static constexpr std::array<std::string_view, 0> get_classes_path_param_names = {};

static constexpr std::array<BackstageParam, 0> get_classes_path_params = {};

static constexpr std::array<BackstageParam, 1> get_classes_query_params = {
   BackstageParam("filter", "csv", "CSV list of class names to filter for", "", false)
};

static constexpr std::array<std::string_view, 1> get_classes_class_path_param_names = {
   "class"
};

static constexpr std::array<BackstageParam, 1> get_classes_class_path_params = {
   BackstageParam("class", "string", "Class name.", "", false)
};

static constexpr std::array<BackstageParam, 0> get_classes_class_query_params = {};

static constexpr std::array<std::string_view, 1> get_classes_class_fields_path_param_names = {
   "class"
};

static constexpr std::array<BackstageParam, 1> get_classes_class_fields_path_params = {
   BackstageParam("class", "string", "Class name.", "", false)
};

static constexpr std::array<BackstageParam, 0> get_classes_class_fields_query_params = {};

static constexpr std::array<std::string_view, 1> get_classes_class_actions_path_param_names = {
   "class"
};

static constexpr std::array<BackstageParam, 1> get_classes_class_actions_path_params = {
   BackstageParam("class", "string", "Class name.", "", false)
};

static constexpr std::array<BackstageParam, 0> get_classes_class_actions_query_params = {};

static constexpr std::array<std::string_view, 1> get_classes_class_methods_path_param_names = {
   "class"
};

static constexpr std::array<BackstageParam, 1> get_classes_class_methods_path_params = {
   BackstageParam("class", "string", "Class name.", "", false)
};

static constexpr std::array<BackstageParam, 0> get_classes_class_methods_query_params = {};

static constexpr std::array<std::string_view, 0> get_modules_path_param_names = {};

static constexpr std::array<BackstageParam, 0> get_modules_path_params = {};

static constexpr std::array<BackstageParam, 1> get_modules_query_params = {
   BackstageParam("filter", "csv", "Limit the list to the named modules", "", false)
};

static constexpr std::array<std::string_view, 1> get_modules_name_path_param_names = {
   "name"
};

static constexpr std::array<BackstageParam, 1> get_modules_name_path_params = {
   BackstageParam("name", "string", "Module name.", "", false)
};

static constexpr std::array<BackstageParam, 0> get_modules_name_query_params = {};

static constexpr std::array<std::string_view, 0> get_errors_path_param_names = {};

static constexpr std::array<BackstageParam, 0> get_errors_path_params = {};

static constexpr std::array<BackstageParam, 1> get_errors_query_params = {
   BackstageParam("filter", "csv", "Limit the list to matching error names or numeric codes", "", false)
};

static constexpr std::array<std::string_view, 1> get_errors_code_path_param_names = {
   "code"
};

static constexpr std::array<BackstageParam, 1> get_errors_code_path_params = {
   BackstageParam("code", "string", "Error name or numeric code.", "", false)
};

static constexpr std::array<BackstageParam, 0> get_errors_code_query_params = {};

static constexpr std::array<std::string_view, 0> get_diagnostics_memory_path_param_names = {};

static constexpr std::array<BackstageParam, 0> get_diagnostics_memory_path_params = {};

static constexpr std::array<BackstageParam, 1> get_diagnostics_memory_query_params = {
   BackstageParam("list", "bool", "Returns a list of all memory allocations (ID and size) if true.", "", false)
};

static constexpr std::array<std::string_view, 0> get_diagnostics_timers_path_param_names = {};

static constexpr std::array<BackstageParam, 0> get_diagnostics_timers_path_params = {};

static constexpr std::array<BackstageParam, 1> get_diagnostics_timers_query_params = {
   BackstageParam("active", "bool", "Limit results to active timers if true.", "", false)
};

static constexpr std::array<std::string_view, 1> get_jobs_job_path_param_names = {
   "job"
};

static constexpr std::array<BackstageParam, 1> get_jobs_job_path_params = {
   BackstageParam("job", "string", "Job identifier.", "", false)
};

static constexpr std::array<BackstageParam, 0> get_jobs_job_query_params = {};

static constexpr std::array<std::string_view, 1> delete_jobs_job_path_param_names = {
   "job"
};

static constexpr std::array<BackstageParam, 1> delete_jobs_job_path_params = {
   BackstageParam("job", "string", "Job identifier.", "", false)
};

static constexpr std::array<BackstageParam, 0> delete_jobs_job_query_params = {};

static constexpr std::array<std::string_view, 0> post_logs_level_path_param_names = {};

static constexpr std::array<BackstageParam, 0> post_logs_level_path_params = {};

static constexpr std::array<BackstageParam, 0> post_logs_level_query_params = {};

static constexpr std::array<std::string_view, 0> post_logs_start_path_param_names = {};

static constexpr std::array<BackstageParam, 0> post_logs_start_path_params = {};

static constexpr std::array<BackstageParam, 0> post_logs_start_query_params = {};

static constexpr std::array<std::string_view, 0> post_logs_stop_path_param_names = {};

static constexpr std::array<BackstageParam, 0> post_logs_stop_path_params = {};

static constexpr std::array<BackstageParam, 0> post_logs_stop_query_params = {};

static constexpr std::array<std::string_view, 0> get_logs_path_param_names = {};

static constexpr std::array<BackstageParam, 0> get_logs_path_params = {};

static constexpr std::array<BackstageParam, 0> get_logs_query_params = {};

static constexpr std::array<std::string_view, 0> post_subscriptions_path_param_names = {};

static constexpr std::array<BackstageParam, 0> post_subscriptions_path_params = {};

static constexpr std::array<BackstageParam, 0> post_subscriptions_query_params = {};

static constexpr std::array<std::string_view, 1> get_subscriptions_id_path_param_names = {
   "id"
};

static constexpr std::array<BackstageParam, 1> get_subscriptions_id_path_params = {
   BackstageParam("id", "string", "Subscription identifier.", "", false)
};

static constexpr std::array<BackstageParam, 0> get_subscriptions_id_query_params = {};

static constexpr std::array<std::string_view, 1> delete_subscriptions_id_path_param_names = {
   "id"
};

static constexpr std::array<BackstageParam, 1> delete_subscriptions_id_path_params = {
   BackstageParam("id", "string", "Subscription identifier.", "", false)
};

static constexpr std::array<BackstageParam, 0> delete_subscriptions_id_query_params = {};

static constexpr std::array<std::string_view, 0> get_streaming_path_param_names = {};

static constexpr std::array<BackstageParam, 0> get_streaming_path_params = {};

static constexpr std::array<BackstageParam, 0> get_streaming_query_params = {};

static constexpr std::array<std::string_view, 0> get_docs_path_param_names = {};

static constexpr std::array<BackstageParam, 0> get_docs_path_params = {};

static constexpr std::array<BackstageParam, 0> get_docs_query_params = {};

static constexpr std::array<std::string_view, 0> get_docs_routes_path_param_names = {};

static constexpr std::array<BackstageParam, 0> get_docs_routes_path_params = {};

static constexpr std::array<BackstageParam, 2> get_docs_routes_query_params = {
   BackstageParam("methodFilter", "regex", "Regex that must match the full HTTP method.", "", false),
   BackstageParam("pathFilter", "regex", "Regex that must match the full route path.", "", false)
};

static constexpr std::array<std::string_view, 0> post_scripts_path_param_names = {};

static constexpr std::array<BackstageParam, 0> post_scripts_path_params = {};

static constexpr std::array<BackstageParam, 3> post_scripts_query_params = {
   BackstageParam("async", "bool", "Return immediately.  A job ID is returned for follow-up", "false", false),
   BackstageParam("exec", "bool", "Run the script after compilation.", "", false),
   BackstageParam("function", "string", "Run this function after compilation.", "", false)
};

static std::array<BackstageRoute, 32> glRoutes = {
   BackstageRoute(
      "GET",
      "/agents/context",
      "^/agents/context$",
      get_agents_context,
      get_agents_context_path_param_names,
      BackstageRouteMetadata(
         "Return a compact runtime context bundle for AI agents: process info, loaded modules, object counts, active windows/surfaces, recent errors, and notable diagnostics.",
         "",
         "object",
         "",
         get_agents_context_path_params,
         get_agents_context_query_params)),
   BackstageRoute(
      "GET",
      "/ping",
      "^/ping$",
      get_ping,
      get_ping_path_param_names,
      BackstageRouteMetadata(
         "Return a small JSON response to confirm that Backstage can receive HTTP requests and send responses.",
         "",
         "json",
         "",
         get_ping_path_params,
         get_ping_query_params)),
   BackstageRoute(
      "GET",
      "/objects",
      "^/objects$",
      get_objects,
      get_objects_path_param_names,
      BackstageRouteMetadata(
         "Return a JSON list of all objects and their basic meta data.",
         "",
         "json",
         "",
         get_objects_path_params,
         get_objects_query_params)),
   BackstageRoute(
      "POST",
      "/objects",
      "^/objects$",
      post_objects,
      post_objects_path_param_names,
      BackstageRouteMetadata(
         "Create a new object, using the provided JSON fields as the object field values",
         "json",
         "json",
         "",
         post_objects_path_params,
         post_objects_query_params)),
   BackstageRoute(
      "GET",
      "/objects/{uid}",
      "^/objects/(-?[0-9]+)$",
      get_objects_uid,
      get_objects_uid_path_param_names,
      BackstageRouteMetadata(
         "Get a list of all readable field values of the target object.",
         "",
         "schema:Object",
         "",
         get_objects_uid_path_params,
         get_objects_uid_query_params)),
   BackstageRoute(
      "GET",
      "/objects/{uid}/children",
      "^/objects/(-?[0-9]+)/children$",
      get_objects_uid_children,
      get_objects_uid_children_path_param_names,
      BackstageRouteMetadata(
         "Return the child objects owned by the target object.",
         "",
         "array",
         "",
         get_objects_uid_children_path_params,
         get_objects_uid_children_query_params)),
   BackstageRoute(
      "GET",
      "/objects/{uid}/subscribers",
      "^/objects/(-?[0-9]+)/subscribers$",
      get_objects_uid_subscribers,
      get_objects_uid_subscribers_path_param_names,
      BackstageRouteMetadata(
         "Return subscriptions and callbacks associated with the target object.",
         "",
         "array",
         "",
         get_objects_uid_subscribers_path_params,
         get_objects_uid_subscribers_query_params)),
   BackstageRoute(
      "POST",
      "/objects/{uid}",
      "^/objects/(-?[0-9]+)$",
      post_objects_uid,
      post_objects_uid_path_param_names,
      BackstageRouteMetadata(
         "Process a series of commands (e.g. call an action, set a field) for the target object.  Returns an error code and any result values.",
         "json",
         "json",
         "",
         post_objects_uid_path_params,
         post_objects_uid_query_params)),
   BackstageRoute(
      "GET",
      "/classes",
      "^/classes$",
      get_classes,
      get_classes_path_param_names,
      BackstageRouteMetadata(
         "Return a JSON list of all classes and their meta data.",
         "",
         "array",
         "",
         get_classes_path_params,
         get_classes_query_params)),
   BackstageRoute(
      "GET",
      "/classes/{class}",
      "^/classes/([^\\/]+)$",
      get_classes_class,
      get_classes_class_path_param_names,
      BackstageRouteMetadata(
         "Return meta data for a single class.",
         "",
         "object",
         "",
         get_classes_class_path_params,
         get_classes_class_query_params)),
   BackstageRoute(
      "GET",
      "/classes/{class}/fields",
      "^/classes/([^\\/]+)/fields$",
      get_classes_class_fields,
      get_classes_class_fields_path_param_names,
      BackstageRouteMetadata(
         "Return field meta data for a class.",
         "",
         "array",
         "",
         get_classes_class_fields_path_params,
         get_classes_class_fields_query_params)),
   BackstageRoute(
      "GET",
      "/classes/{class}/actions",
      "^/classes/([^\\/]+)/actions$",
      get_classes_class_actions,
      get_classes_class_actions_path_param_names,
      BackstageRouteMetadata(
         "Return action meta data for a class.",
         "",
         "array",
         "",
         get_classes_class_actions_path_params,
         get_classes_class_actions_query_params)),
   BackstageRoute(
      "GET",
      "/classes/{class}/methods",
      "^/classes/([^\\/]+)/methods$",
      get_classes_class_methods,
      get_classes_class_methods_path_param_names,
      BackstageRouteMetadata(
         "Return method meta data for a class.",
         "",
         "array",
         "",
         get_classes_class_methods_path_params,
         get_classes_class_methods_query_params)),
   BackstageRoute(
      "GET",
      "/modules",
      "^/modules$",
      get_modules,
      get_modules_path_param_names,
      BackstageRouteMetadata(
         "Return a JSON list of all modules and their meta data.",
         "",
         "array",
         "",
         get_modules_path_params,
         get_modules_query_params)),
   BackstageRoute(
      "GET",
      "/modules/{name}",
      "^/modules/([^\\/]+)$",
      get_modules_name,
      get_modules_name_path_param_names,
      BackstageRouteMetadata(
         "Return meta data and runtime state for a loaded module.",
         "",
         "object",
         "",
         get_modules_name_path_params,
         get_modules_name_query_params)),
   BackstageRoute(
      "GET",
      "/errors",
      "^/errors$",
      get_errors,
      get_errors_path_param_names,
      BackstageRouteMetadata(
         "Return a JSON list of known error codes and their symbolic names.",
         "",
         "array",
         "",
         get_errors_path_params,
         get_errors_query_params)),
   BackstageRoute(
      "GET",
      "/errors/{code}",
      "^/errors/([^\\/]+)$",
      get_errors_code,
      get_errors_code_path_param_names,
      BackstageRouteMetadata(
         "Return meta data for a single error code.",
         "",
         "object",
         "",
         get_errors_code_path_params,
         get_errors_code_query_params)),
   BackstageRoute(
      "GET",
      "/diagnostics/memory",
      "^/diagnostics/memory$",
      get_diagnostics_memory,
      get_diagnostics_memory_path_param_names,
      BackstageRouteMetadata(
         "Returns a summary of memory allocations.  Specifying additional parameters can result in more detail",
         "",
         "object",
         "",
         get_diagnostics_memory_path_params,
         get_diagnostics_memory_query_params)),
   BackstageRoute(
      "GET",
      "/diagnostics/timers",
      "^/diagnostics/timers$",
      get_diagnostics_timers,
      get_diagnostics_timers_path_param_names,
      BackstageRouteMetadata(
         "Return timer and scheduled callback diagnostics.",
         "",
         "array",
         "",
         get_diagnostics_timers_path_params,
         get_diagnostics_timers_query_params)),
   BackstageRoute(
      "GET",
      "/jobs/{job}",
      "^/jobs/([^\\/]+)$",
      get_jobs_job,
      get_jobs_job_path_param_names,
      BackstageRouteMetadata(
         "Return the state and result meta data for an asynchronous Backstage job.",
         "",
         "object",
         "",
         get_jobs_job_path_params,
         get_jobs_job_query_params)),
   BackstageRoute(
      "DELETE",
      "/jobs/{job}",
      "^/jobs/([^\\/]+)$",
      delete_jobs_job,
      delete_jobs_job_path_param_names,
      BackstageRouteMetadata(
         "Cancel or clear an asynchronous Backstage job.",
         "",
         "object",
         "",
         delete_jobs_job_path_params,
         delete_jobs_job_query_params)),
   BackstageRoute(
      "POST",
      "/logs/level",
      "^/logs/level$",
      post_logs_level,
      post_logs_level_path_param_names,
      BackstageRouteMetadata(
         "Change the logging level for the program (affects console output).",
         "json",
         "object",
         "",
         post_logs_level_path_params,
         post_logs_level_query_params)),
   BackstageRoute(
      "POST",
      "/logs/start",
      "^/logs/start$",
      post_logs_start,
      post_logs_start_path_param_names,
      BackstageRouteMetadata(
         "Activates internal log recording.",
         "json",
         "object",
         "",
         post_logs_start_path_params,
         post_logs_start_query_params)),
   BackstageRoute(
      "POST",
      "/logs/stop",
      "^/logs/stop$",
      post_logs_stop,
      post_logs_stop_path_param_names,
      BackstageRouteMetadata(
         "Stops internal log recording.",
         "json",
         "object",
         "",
         post_logs_stop_path_params,
         post_logs_stop_query_params)),
   BackstageRoute(
      "GET",
      "/logs",
      "^/logs$",
      get_logs,
      get_logs_path_param_names,
      BackstageRouteMetadata(
         "Returns all logged messages, then clears the log message stack (the client is responsible for maintaining a permanent record).",
         "",
         "array",
         "",
         get_logs_path_params,
         get_logs_query_params)),
   BackstageRoute(
      "POST",
      "/subscriptions",
      "^/subscriptions$",
      post_subscriptions,
      post_subscriptions_path_param_names,
      BackstageRouteMetadata(
         "Create a Backstage subscription for live object, log, or diagnostic updates.",
         "json",
         "object",
         "",
         post_subscriptions_path_params,
         post_subscriptions_query_params)),
   BackstageRoute(
      "GET",
      "/subscriptions/{id}",
      "^/subscriptions/([^\\/]+)$",
      get_subscriptions_id,
      get_subscriptions_id_path_param_names,
      BackstageRouteMetadata(
         "Return meta data and current state for a Backstage subscription.",
         "",
         "object",
         "",
         get_subscriptions_id_path_params,
         get_subscriptions_id_query_params)),
   BackstageRoute(
      "DELETE",
      "/subscriptions/{id}",
      "^/subscriptions/([^\\/]+)$",
      delete_subscriptions_id,
      delete_subscriptions_id_path_param_names,
      BackstageRouteMetadata(
         "Delete a Backstage subscription.",
         "",
         "object",
         "",
         delete_subscriptions_id_path_params,
         delete_subscriptions_id_query_params)),
   BackstageRoute(
      "GET",
      "/streaming",
      "^/streaming$",
      nullptr,
      get_streaming_path_param_names,
      BackstageRouteMetadata(
         "Upgrade to a WebSocket stream for live Backstage events.",
         "",
         "stream",
         "websocket",
         get_streaming_path_params,
         get_streaming_query_params)),
   BackstageRoute(
      "GET",
      "/docs",
      "^/docs$",
      get_docs,
      get_docs_path_param_names,
      BackstageRouteMetadata(
         "Return Backstage API documentation meta data.",
         "",
         "object",
         "",
         get_docs_path_params,
         get_docs_query_params)),
   BackstageRoute(
      "GET",
      "/docs/routes",
      "^/docs/routes$",
      get_docs_routes,
      get_docs_routes_path_param_names,
      BackstageRouteMetadata(
         "Return the declared Backstage routes and their meta data.",
         "",
         "array",
         "",
         get_docs_routes_path_params,
         get_docs_routes_query_params)),
   BackstageRoute(
      "POST",
      "/scripts",
      "^/scripts$",
      post_scripts,
      post_scripts_path_param_names,
      BackstageRouteMetadata(
         "Accepts a Tiri script for compilation.  Returns an object identifier for the script and any results if executed.",
         "raw",
         "object",
         "",
         post_scripts_path_params,
         post_scripts_query_params))
};

static std::span<BackstageRoute> backstage_routes()
{
   return glRoutes;
}
